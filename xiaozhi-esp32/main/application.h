#ifndef _APPLICATION_H_
#define _APPLICATION_H_

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>

#include <string>
#include <atomic>
#include <mutex>
#include <list>

#include <opus_encoder.h>
#include <opus_decoder.h>
#include <opus_resampler.h>

#include "protocol.h"
#include "ota.h"
#include "background_task.h"

#include <led_strip.h>
#include <lwip/sockets.h>

#if CONFIG_IDF_TARGET_ESP32S3
#include "wake_word_detect.h"
#include "audio_processor.h"
#endif

#define SCHEDULE_EVENT (1 << 0)
#define AUDIO_INPUT_READY_EVENT (1 << 1)
#define AUDIO_OUTPUT_READY_EVENT (1 << 2)

enum ChatState {
    kChatStateUnknown,
    kChatStateIdle,
    kChatStateConnecting,
    kChatStateListening,
    kChatStateSpeaking,
    kChatStateUpgrading
};

/* ============ 解忧树洞新增: 终端状态机 ============
 * 待命 → 录音(按住对话键) → 发送 → 等待答复 → 显示结果
 * 等待答复时会依次启用: 本地即时安慰 → 小智云端完整回答(老师不在时)
 */
enum TreeHoleState {
    kTreeHoleIdle,      // 待命
    kTreeHoleRecording, // 正在录音
    kTreeHoleSending,   // 正在发送
    kTreeHoleWaiting,   // 等待答复(老师端 / 本地兜底 / 云端)
    kTreeHoleCloud,     // 小智云端正在组织并播放完整回答
    kTreeHoleShowing,   // 正在显示结果
};

#define OPUS_FRAME_DURATION_MS 60

class Application {
public:
    static Application& GetInstance() {
        static Application instance;
        return instance;
    }
    // 删除拷贝构造函数和赋值运算符
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    void Start();
    ChatState GetChatState() const { return chat_state_.load(); }
    void Schedule(std::function<void()> callback);
    void SetChatState(ChatState state);
    void Alert(const std::string& title, const std::string& message);
    void AbortSpeaking(AbortReason reason);
    void ToggleChatState();
    void StartListening();
    void StopListening();
    void UpdateIotStates();

    // ============ 解忧树洞新增公开方法 ============
    void ShowResult(const std::string &level, const std::string &text); // 显示服务端结果
    void PlayHelpAudio();        // 播放Flash中的求助提示音(红级)
    void UpdateLedBreathing();   // 刷新12颗RGB灯的呼吸动画
    void UpdateRecordScreen();   // 录音中刷新屏幕计时

private:
    Application();
    ~Application();

#if CONFIG_IDF_TARGET_ESP32S3
    WakeWordDetect wake_word_detect_;
    AudioProcessor audio_processor_;
#endif
    Ota ota_;
    std::mutex mutex_;
    std::list<std::function<void()>> main_tasks_;
    std::unique_ptr<Protocol> protocol_;
    EventGroupHandle_t event_group_;
    std::atomic<ChatState> chat_state_{kChatStateUnknown};
    bool keep_listening_ = false;
    bool aborted_ = false;
    std::string last_iot_states_;

    // Audio encode / decode
    BackgroundTask background_task_;
    std::chrono::steady_clock::time_point last_output_time_;
    std::list<std::vector<uint8_t>> audio_decode_queue_;

    std::unique_ptr<OpusEncoderWrapper> opus_encoder_;
    std::unique_ptr<OpusDecoderWrapper> opus_decoder_;

    int opus_decode_sample_rate_ = -1;
    OpusResampler input_resampler_;
    OpusResampler reference_resampler_;
    OpusResampler output_resampler_;

    // ============ 解忧树洞新增成员 ============
    TreeHoleState treehole_state_ = kTreeHoleIdle;  // 当前状态
    uint8_t *rec_buf_ = nullptr;     // 录音缓冲(PSRAM, 前44字节留作WAV头)
    uint32_t rec_len_ = 0;           // 已录PCM字节数
    bool record_full_ = false;       // 是否录满10秒自动发送
    int udp_sock_ = -1;              // UDP套接字
    struct sockaddr_in server_addr_; // 服务端地址
    uint32_t wait_start_ms_ = 0;     // 等待回复的起始时刻
    char result_level_[8] = {0};     // 当前结果级别 green/yellow/red
    std::string result_text_;        // 当前结果正文
    uint32_t last_wifi_try_ms_ = 0;  // 上次WiFi重连时刻
    std::atomic<bool> wifi_connected_{false}; // WiFi事件线程写、主循环读
    led_strip_handle_t rgb_strip_ = nullptr; // 12颗WS2812灯条句柄
    uint32_t last_record_ms_ = 0;    // 上次刷新录音屏幕的时刻
    bool local_reply_shown_ = false; // 教师端未回复时是否已先本地回复
    uint8_t local_reply_index_ = 0;  // 本地安慰语轮换下标

    // ---- 老师发来的语音(老师端录音 → 终端播报) ----
    uint8_t *teacher_buf_ = nullptr;     // 老师语音缓冲(PSRAM, 老师发来的完整WAV原样存放, 从第0字节开始)
    uint32_t teacher_bytes_ = 0;         // 已收到的语音数据字节数
    uint32_t teacher_expect_ = 0;        // 收齐时应收到的总字节数
    uint8_t teacher_seen_[267] = {0};    // 320000/1200向上取整；防重复包把实收计数虚增
    bool teacher_have_seq0_ = false;     // 本段是否已见到 seq=0(没见到时先暂存"预头分片")
    bool playing_voice_ = false;         // 是否正在播放老师语音(用于蓝灯呼吸)
    bool teacher_voice_pending_ = false; // 老师正在发语音(云端回答要让位给老师)
    bool teacher_voice_ready_ = false;   // 老师语音已收齐, 等云端说完再播
    bool teacher_voice_rejected_ = false;// 本段协议/长度不合法, 等下一个seq=0再恢复
    uint32_t teacher_last_packet_ms_ = 0;// 不完整语音超时回收，避免连按时串段
    uint32_t last_heartbeat_ms_ = 0;     // 上次给教师端发心跳的时刻(每10秒一次)

    // ---- 小智云端兜底(老师不在时由云端把话说完) ----
    bool cloud_started_ = false;         // 本轮是否已启动云端兜底
    std::atomic<bool> tts_finished_{false}; // 协议线程写、主循环读
    uint32_t cloud_start_ms_ = 0;        // 云端回答起始时刻
    bool teacher_reply_pending_ = false; // 云端回答期间老师回复已到达(优先级最高)
    char pending_level_[8] = {0};        // 老师迟到的级别
    std::string pending_text_;           // 老师迟到的正文
    std::vector<std::string> cloud_sentences_; // 云端回答的最近几句(小屏只留最近4句)

    // 树洞内部流程
    void ConnectWifi();                    // 连接局域网WiFi(宏定义账号密码)
    void EnsureWifi();                     // 掉线自动重连
    void StartRecording();                 // 开始录音
    void RecordTick();                     // 录音循环(读麦克风)
    void StopRecordingAndSend();           // 停止录音并发送
    void SendWavToServer();                // UDP分包发送WAV给老师端
    void WaitReplyTick();                  // 等待答复(本地兜底 + 云端兜底)
    void ShowLocalReply();                 // 老师端未回复时先给本地安慰回复
    void CloudFallbackTick();              // 小智云端完整回答(阻塞直到答完)
    void ShowCloudText();                  // 把云端回答上屏
    bool PollTeacherReply();               // 收一次老师端UDP回复(收到返回true)
    bool UdpHasData();                     // 套接字里现在有数据吗(0超时, 不阻塞)
    void FlushStaleReplies();              // 清掉上一轮遗留的过期回复包(老师语音保留)
    void PollIdlePackets();                // 待命时也收包: 老师语音/迟到文字回复
    void HandleIdlePacket(char *buf, int len);       // 处理待命期间收到的包(语音/迟到回复)
    bool AssembleTeacherVoice(const char *buf, int len); // 落位一个XZSV分片(收齐返回true)
    void BeginTeacherSegment();            // 开始新一段老师语音(清计数/去重表)
    void SendTeacherAck(uint8_t kind, uint8_t status); // 回送达确认(6字节 XZAK)
    void SendHeartbeat();                  // 每10秒给教师端发一次心跳(8字节 XZHB)
    void CollectTeacherVoice();            // 录音/发送期间只收老师的语音(不播放)
    void PlayTeacherVoice();               // 播放老师发来的语音
    bool PlayWavBuffer(const uint8_t *wav, size_t size, const char *what); // 通用WAV播放
    void ResetTeacherBuffer();             // 清空老师语音缓冲
    void ExpireTeacherVoice();             // 丢弃长时间未收齐的老师语音
    void HandleReply(const char *json);    // 解析老师端JSON
    void WriteWavHeader();                 // 在缓冲区头部写WAV文件头
    void StopLeds();                       // 关闭全部RGB灯

    void MainLoop();
    void SetDecodeSampleRate(int sample_rate);
    void CheckNewVersion();
};

#endif // _APPLICATION_H_
