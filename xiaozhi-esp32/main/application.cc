/* ====================================================================
 * 解忧树洞 (Worry-Relief Tree Hole) —— ESP32-S3 终端固件
 * --------------------------------------------------------------------
 * 本文件基于开源项目 xiaozhi-esp32 (github.com/78/xiaozhi-esp32)
 * v0.9.7 的 main/application.cc 修改而来: 板级/显示/音频/云端协议全部复用,
 * 应用逻辑合并成一条完整链路 —— 老师优先, 老师不在就由小智云端把话说完:
 *
 *   待命 → 按住对话键录音(16kHz/16bit WAV, 仅存内存) → 松开停止
 *        → ① UDP 分包发送给教师电脑(局域网, 老师端判定)
 *        → ② 1.5 秒没回复 → 终端先给本地安慰回复(暖橙呼吸灯), 同学不会干等
 *        → ③ 4 秒还没回复 → 小智云端接管: 把刚才的录音编码上传云端,
 *             云端识别→回答→回传语音, 终端边播边显示(绿色呼吸灯),
 *             也就是"老师不在时机器人也能完整回复"
 *        → ④ 老师在 15 秒内回复, 一律以老师的判定为准(绿/黄/红 + 求助信息)
 *        → 红级: 额外显示 12355 热线 + 心理老师电话, 并播放内置求助提示音
 *
 * 【关于云端与隐私】
 *   - 老师端链路全程只在局域网内; 只有在"老师没回复"时才会走小智云端(②③)
 *   - 走云端时, 这段录音会上传 api.tenclass.net(与小智音箱同款行为),
 *     不需要云端兜底就把下面的 ENABLE_CLOUD_FALLBACK 改成 0
 *   - 录音始终只存在内存(PSRAM)中, 发送后即被下次录音覆盖, 不写入Flash
 *   - 串口日志只打印状态, 不打印音频数据
 * ==================================================================== */
#include "application.h"
#include "board.h"
#include "display.h"
#include "audio_codec.h"
#include "font_awesome_symbols.h"
#include "system_info.h"
#include "mqtt_protocol.h"
#include "websocket_protocol.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_netif.h>
#include <esp_heap_caps.h>
#include <cJSON.h>
#include <led_strip.h>
#include <lwip/sockets.h>
#include <arpa/inet.h>

#include <vector>
#include <cmath>
#include <cerrno>
#include <cstring>
#include <cstdio>

#define TAG "TreeHole"

/* ==================== 网络配置(按你的局域网修改) ==================== */
#define WIFI_SSID     "你的WiFi名称"      // WiFi 账号
#define WIFI_PASS     "你的WiFi密码"      // WiFi 密码
#define SERVER_IP     "192.168.1.100"   // 电脑服务端 IP 地址
#define SERVER_PORT   8888              // 服务端 UDP 监听端口
#define LOCAL_PORT    8889              // 本机 UDP 端口(服务端回复发到这里)

/* ============ 答复策略(三步走, 老师优先, 小智兜底) ============
 *   0    ~ 1.5秒 : 屏显"等待回复...", 等老师端判定
 *   1.5秒        : 老师还没回 → 终端先本地安慰回复(暖橙呼吸灯)
 *   4秒          : 老师还没回 → 启动小智云端完整回答(上传录音→云端回答→出声)
 *   15秒         : 老师仍未回 → 以已经给出的回答为准, 提示"老师稍后会回复你"
 * 老师端任何时候回复都优先显示; 想改节奏就调这三个宏。
 */
#define LOCAL_REPLY_DELAY_MS   1500     // 多久没回复就先给本地安慰
#define CLOUD_FALLBACK_DELAY_MS 4000    // 多久没回复就交给小智云端完整回答
#define REPLY_TIMEOUT_MS       15000    // 等老师端的总超时时间
#define CLOUD_ANSWER_TIMEOUT_MS 25000   // 等小智云端回答的最长时间(含播报)
#define ENABLE_CLOUD_FALLBACK  1        // 1=开启小智云端兜底, 0=只用本地(纯局域网)

/* ==================== 录音参数(一般不用改) ==================== */
#define REC_SAMPLE_RATE 16000           // 录音采样率 16kHz
#define REC_MAX_SECONDS 10              // 最长录音 10 秒
#define REC_BUF_SIZE    (REC_SAMPLE_RATE * 2 * REC_MAX_SECONDS) // 16bit=2字节 → 320000字节
#define WAV_HEADER_SIZE 44              // WAV 文件头固定 44 字节
#define MIN_REC_BYTES   3200            // 少于 0.1 秒的录音视为误触丢弃
#define UDP_CHUNK_SIZE  1200            // 每个 UDP 包携带的数据字节数

/* ==================== 老师语音(老师端录音发给终端播报) ====================
 * 与"终端→老师"用同一套分包格式, 只是魔数不同:
 *   终端发给老师: "XZSD" (D=device, 学生语音)
 *   老师发给终端: "XZSV" (V=voice,  老师语音)
 * 老师端会把录音转成 8kHz/16bit/单声道 再发(终端功放就是 8kHz)。
 */
#define VOICE_MAGIC     "XZSV"
#define VOICE_MAX_BYTES REC_BUF_SIZE    // 整段 WAV 最多 320000 字节(含头，8kHz PCM 约 19.99 秒)
#define VOICE_RECEIVE_TIMEOUT_MS 3000   // 末包丢失后3秒回收，避免旧分片串进下一段

/* ==================== 与教师端的两个短控制包 ====================
 * 心跳  XZHB(4) + uptime秒(4,大端)              固定 8 字节, 每 10 秒一发
 * 确认  XZAK(4) + kind(1) + status(1)           固定 6 字节
 *       kind:   1=文字判定   2=老师语音
 *       status: 0=已应用/已播放   1=被拒绝
 * 都是固定长度, 教师端按长度即可与音频分包区分, 不占用 XZSD/XZSV/JSON 的格式。
 */
#define HEARTBEAT_MAGIC "XZHB"
#define ACK_MAGIC       "XZAK"
#define HEARTBEAT_INTERVAL_MS 10000     // 心跳间隔
#define ACK_KIND_TEXT   1               // 文字判定
#define ACK_KIND_VOICE  2               // 老师语音
#define ACK_STATUS_OK   0               // 已应用 / 已播放
#define ACK_STATUS_DENY 1               // 被拒绝(协议/长度/WAV 不合法, 或没收全)

/* ==================== 12 颗 WS2812 RGB 灯 ==================== */
#define RGB_LED_GPIO   2                // 灯条数据脚(见板级配置 config.h)
#define RGB_LED_COUNT  12               // 灯珠数量

/* ==================== 红级求助信息(改成学校真实信息) ==================== */
#define HOTLINE_TEXT   "青少年心理热线 12355"
#define TEACHER_TEXT   "心理老师: 138xxxx1234"

/* ============ 本地兜底回复(教师端未回复时终端先说的安慰语) ============
 * 说明: 本地回复不做级别判定(机器不替老师下结论), 只做陪伴式回应,
 * 并明确告诉同学"老师正在赶来"。可自由增删句子, 数量不限。
 */
static const char *LOCAL_REPLY_TEXTS[] = {
    "我在听, 你说的每一句都很重要。\n老师正在赶来, 请稍等我片刻。",
    "谢谢你愿意说出来, 这本身就很勇敢。\n老师马上就来陪你聊聊。",
    "别急, 慢慢说, 我一直在这里陪你。\n老师正在赶来的路上。",
    "你的感受很重要, 值得被认真对待。\n老师正在赶来, 请稍等一会儿。",
};
#define LOCAL_REPLY_COUNT (sizeof(LOCAL_REPLY_TEXTS) / sizeof(LOCAL_REPLY_TEXTS[0]))

/* 内置求助提示音(8kHz/16bit/单声道 WAV, 由构建系统打进固件 Flash) */
extern const char help_wav_start[] asm("_binary_help_wav_start");
extern const char help_wav_end[] asm("_binary_help_wav_end");

/* 取系统运行毫秒数(ESP-IDF 里没有 Arduino 的 millis(), 自己封装一个) */
static uint32_t now_ms() {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* 小端写 32 位 / 16 位整数, 用于拼 WAV 文件头 */
static void put32(uint8_t *p, uint32_t v) {
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}
static void put16(uint8_t *p, uint16_t v) {
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF;
}

static bool is_teacher_level(const char *level) {
    return level != nullptr && (strcmp(level, "green") == 0 || strcmp(level, "yellow") == 0 ||
                                strcmp(level, "red") == 0);
}

Application::Application() : background_task_(4096 * 8) {
    event_group_ = xEventGroupCreate();
    memset(&server_addr_, 0, sizeof(server_addr_));

    /* 云端(小智)配置: 版本检查地址 + 设备标识(沿用上游原版逻辑) */
    ota_.SetCheckVersionUrl(CONFIG_OTA_VERSION_URL);
    ota_.SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
}

Application::~Application() {
    if (event_group_ != nullptr) {
        vEventGroupDelete(event_group_);
    }
    if (udp_sock_ >= 0) {
        close(udp_sock_);
    }
    if (rec_buf_ != nullptr) {
        free(rec_buf_);
    }
    if (teacher_buf_ != nullptr) {
        free(teacher_buf_);
    }
    if (rgb_strip_ != nullptr) {
        led_strip_del(rgb_strip_);
    }
}

/* ====================================================================
 * 启动流程: 板卡(屏幕/按键/音频)在 Board::GetInstance() 时自动初始化,
 * 这里依次准备录音缓冲、RGB灯条、UDP套接字、WiFi, 最后进入主循环。
 * ==================================================================== */
void Application::Start() {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    display->SetStatus("正在初始化...");

    /* 1. 音频: 注册I2S回调并开启麦克风/功放通道 */
    auto codec = board.GetAudioCodec();
    codec->Start();

    /* 2. 录音缓冲: 放到 PSRAM(8MB), 前面留 44 字节放 WAV 头 */
    rec_buf_ = (uint8_t *)heap_caps_malloc(REC_BUF_SIZE + WAV_HEADER_SIZE, MALLOC_CAP_SPIRAM);
    if (rec_buf_ == nullptr) {
        ESP_LOGE(TAG, "PSRAM 分配失败! 请在 menuconfig 里打开 PSRAM(Octal)");
        display->SetStatus("内存不足, 请重启");
        return;
    }
    /* 老师语音缓冲(同样是 PSRAM，整段上限 320000 字节) */
    teacher_buf_ = (uint8_t *)heap_caps_malloc(VOICE_MAX_BYTES, MALLOC_CAP_SPIRAM);
    if (teacher_buf_ == nullptr) {
        ESP_LOGW(TAG, "老师语音缓冲分配失败, 将无法播放老师语音");
    }
    ResetTeacherBuffer();

    /* 3. 12 颗 WS2812 灯条(用 RMT 外设驱动) */
    led_strip_config_t strip_config = {};
    strip_config.strip_gpio_num = (gpio_num_t)RGB_LED_GPIO;
    strip_config.max_leds = RGB_LED_COUNT;
    strip_config.led_pixel_format = LED_PIXEL_FORMAT_GRB;  // 颜色不对时改 LED_PIXEL_FORMAT_RGB
    strip_config.led_model = LED_MODEL_WS2812;
    led_strip_rmt_config_t rmt_config = {};
    rmt_config.resolution_hz = 10 * 1000 * 1000;           // RMT 时钟 10MHz
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &rgb_strip_));
    led_strip_clear(rgb_strip_);

    /* 4. UDP 套接字: 绑定本机端口, 接收超时设 100ms(等待回复时轮询用) */
    udp_sock_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_sock_ < 0) {
        ESP_LOGE(TAG, "创建 UDP 套接字失败");
        display->SetStatus("网络初始化失败");
        return;
    }
    struct sockaddr_in local_addr = {};
    local_addr.sin_family = AF_INET;
    local_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    local_addr.sin_port = htons(LOCAL_PORT);
    if (bind(udp_sock_, (struct sockaddr *)&local_addr, sizeof(local_addr)) != 0) {
        ESP_LOGE(TAG, "UDP 端口 %d 绑定失败: errno=%d", LOCAL_PORT, errno);
        close(udp_sock_);
        udp_sock_ = -1;
        display->SetStatus("网络端口被占用, 请重启");
        return;
    }
    struct timeval tv = { .tv_sec = 0, .tv_usec = 100000 };
    if (setsockopt(udp_sock_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) {
        ESP_LOGW(TAG, "设置 UDP 接收超时失败: errno=%d", errno);
    }

    /* 5. 服务端地址(电脑上跑的服务程序) */
    server_addr_.sin_family = AF_INET;
    server_addr_.sin_port = htons(SERVER_PORT);
    if (inet_aton(SERVER_IP, &server_addr_.sin_addr) == 0) {
        ESP_LOGE(TAG, "教师端 IP 格式错误: %s", SERVER_IP);
        display->SetStatus("教师端 IP 配置错误");
        return;
    }

    /* 6. 连接局域网 WiFi(账号密码在文件顶部宏定义) */
    ConnectWifi();

    /* 7. 初始化小智云端通道(老师端没回复时, 由它把话说完) + 语音编解码器 */
#if ENABLE_CLOUD_FALLBACK
    display->SetStatus("初始化云端通道...");
#ifdef CONFIG_CONNECTION_TYPE_WEBSOCKET
    protocol_ = std::make_unique<WebsocketProtocol>();
#else
    protocol_ = std::make_unique<MqttProtocol>();
#endif
    opus_encoder_ = std::make_unique<OpusEncoderWrapper>(REC_SAMPLE_RATE, 1, OPUS_FRAME_DURATION_MS);

    protocol_->OnNetworkError([](const std::string &message) {
        ESP_LOGW(TAG, "云端网络错误: %s", message.c_str());
    });
    /* 云端回传的语音帧: 先入队, 由 CloudFallbackTick 逐帧解码播放 */
    protocol_->OnIncomingAudio([this](std::vector<uint8_t>&& data) {
        std::lock_guard<std::mutex> lock(mutex_);
        audio_decode_queue_.emplace_back(std::move(data));
    });
    /* 云端通道建立后, 按服务端采样率准备解码器(可能需要重采样) */
    protocol_->OnAudioChannelOpened([this]() {
        SetDecodeSampleRate(protocol_->server_sample_rate());
        ESP_LOGI(TAG, "云端通道已建立, 服务端采样率 %d", protocol_->server_sample_rate());
    });
    protocol_->OnAudioChannelClosed([this]() {
        ESP_LOGI(TAG, "云端通道已关闭");
    });
    /* 云端返回的 JSON: 识别文字(stt)/回答文字(tts)/情绪(llm) */
    protocol_->OnIncomingJson([this](const cJSON *root) {
        auto type = cJSON_GetObjectItem(root, "type");
        if (type == nullptr || type->valuestring == nullptr) {
            return;
        }
        if (strcmp(type->valuestring, "tts") == 0) {
            auto state = cJSON_GetObjectItem(root, "state");
            if (state != nullptr && state->valuestring != nullptr) {
                if (strcmp(state->valuestring, "stop") == 0) {
                    tts_finished_ = true;                     // 云端播报结束
                } else if (strcmp(state->valuestring, "sentence_start") == 0) {
                    auto text = cJSON_GetObjectItem(root, "text");
                    if (text != nullptr && text->valuestring != nullptr) {
                        {
                            std::lock_guard<std::mutex> lock(mutex_);
                            cloud_sentences_.emplace_back(text->valuestring);
                            if (cloud_sentences_.size() > 4) {     // 小屏只保留最近4句
                                cloud_sentences_.erase(cloud_sentences_.begin());
                            }
                        }
                        ShowCloudText();                           // 边回答边上屏
                    }
                }
            }
        } else if (strcmp(type->valuestring, "stt") == 0) {
            auto text = cJSON_GetObjectItem(root, "text");
            if (text != nullptr && text->valuestring != nullptr) {
                ESP_LOGI(TAG, "云端识别: %s", text->valuestring);
                Board::GetInstance().GetDisplay()->SetStatus(std::string("听到: ") + text->valuestring);
            }
        } else if (strcmp(type->valuestring, "llm") == 0) {
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (emotion != nullptr && emotion->valuestring != nullptr) {
                Board::GetInstance().GetDisplay()->SetEmotion(emotion->valuestring);
            }
        } else if (strcmp(type->valuestring, "alert") == 0) {
            /* 云端提示(例如设备需要激活、服务繁忙), 直接弹给学生看并结束等待 */
            auto message = cJSON_GetObjectItem(root, "message");
            if (message != nullptr && message->valuestring != nullptr) {
                ESP_LOGW(TAG, "云端提示: %s", message->valuestring);
                Board::GetInstance().GetDisplay()->ShowNotification(message->valuestring);
                tts_finished_ = true;   // 让等待循环尽快收尾, 不干等
            }
        }
    });

    /* 8. 启动云端版本检查任务(原版逻辑: 顺带完成设备激活/取服务地址) */
    xTaskCreate([](void *arg) {
        Application *app = (Application *)arg;
        app->CheckNewVersion();
        vTaskDelete(NULL);
    }, "check_new_version", 4096 * 2, this, 1, nullptr);
#endif // ENABLE_CLOUD_FALLBACK

    /* 9. 启动主循环任务(处理按键事件/录音/灯效/答复) */
    xTaskCreate([](void *arg) {
        Application *app = (Application *)arg;
        app->MainLoop();
        vTaskDelete(NULL);
    }, "main_loop", 4096 * 2, this, 2, nullptr);

    display->SetStatus("待命: 按住对话键说话");
    SetChatState(kChatStateIdle);
    ESP_LOGI(TAG, "解忧树洞启动完成");
}

/* ====================================================================
 * WiFi 连接(纯局域网, 不经过任何配网服务)
 * ==================================================================== */
void Application::ConnectWifi() {
    /* main.cc 只创建了默认事件循环；创建 STA netif 前仍必须初始化 esp-netif。 */
    ESP_ERROR_CHECK(esp_netif_init());
    /* 注册 WiFi 事件:
     *  - STA 启动完成 → 发起连接
     *  - 断线            → 标记掉线(由 EnsureWifi 每5秒重连)
     *  - 拿到 IP         → 标记在线, 屏幕显示本机 IP(方便调试) */
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_START,
        [](void *arg, esp_event_base_t base, int32_t id, void *data) {
            esp_wifi_connect();
        }, NULL);
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
        [](void *arg, esp_event_base_t base, int32_t id, void *data) {
            Application::GetInstance().wifi_connected_ = false;
            ESP_LOGW(TAG, "WiFi 已断开");
        }, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
        [](void *arg, esp_event_base_t base, int32_t id, void *data) {
            Application::GetInstance().wifi_connected_ = true;
            ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
            char buf[64];
            snprintf(buf, sizeof(buf), "IP: " IPSTR " 按住对话键", IP2STR(&event->ip_info.ip));
            Board::GetInstance().GetDisplay()->SetStatus(buf);
            ESP_LOGI(TAG, "WiFi 已连接");
        }, NULL);

    /* 初始化 WiFi 为 STA(工作站)模式并写入账号密码 */
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    wifi_config_t wifi_config = {};
    strncpy((char *)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, WIFI_PASS, sizeof(wifi_config.sta.password) - 1);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());  // 连接动作由 WIFI_EVENT_STA_START 事件触发
}

/* 掉线后每 5 秒自动重连一次 */
void Application::EnsureWifi() {
    if (wifi_connected_) {
        return;
    }
    if (now_ms() - last_wifi_try_ms_ < 5000) {
        return;
    }
    last_wifi_try_ms_ = now_ms();
    esp_wifi_connect();
}

/* ====================================================================
 * 按键事件入口(由板卡按键回调调用, 通过 Schedule 转交主循环)
 * ==================================================================== */
void Application::StartListening() {
    Schedule([this]() { StartRecording(); });
}

void Application::StopListening() {
    Schedule([this]() { StopRecordingAndSend(); });
}

void Application::ToggleChatState() {
    Schedule([this]() {
        if (treehole_state_ == kTreeHoleIdle || treehole_state_ == kTreeHoleShowing) {
            StartRecording();
        }
    });
}

/* 提示弹窗(状态栏显示 3 秒) */
void Application::Alert(const std::string& title, const std::string& message) {
    ESP_LOGW(TAG, "Alert: %s, %s", title.c_str(), message.c_str());
    Board::GetInstance().GetDisplay()->ShowNotification(message);
}

void Application::AbortSpeaking(AbortReason reason) {
    // 树洞固件没有需要中断的连续播报, 留空
}

void Application::UpdateIotStates() {
    // 树洞固件不使用物联网设备, 留空
}

void Application::SetChatState(ChatState state) {
    chat_state_.store(state);
}

/* ====================================================================
 * 状态一: 开始录音(按住对话键触发)
 * ==================================================================== */
void Application::StartRecording() {
    if (treehole_state_ != kTreeHoleIdle && treehole_state_ != kTreeHoleShowing) {
        return;  // 正在录音/发送/等待时忽略新的按键
    }
    /* 按键任务比状态轮询先执行。若“上一轮太短→立刻再按”恰好撞上老师语音，
     * 先播已收齐的；仍在传的则暂缓录音，不能 Reset 后只留下后半段。 */
    ExpireTeacherVoice();
    if (teacher_expect_ > 0 && teacher_bytes_ >= teacher_expect_) {
        PlayTeacherVoice();
    }
    if (teacher_bytes_ > 0) {
        Board::GetInstance().GetDisplay()->SetStatus("老师正在给你留言，请稍候...");
        return;
    }

    /* 清掉上一轮遗留的过期回复包。
     * ★ 必须放在"本轮开始录音之前": 它内部会有一次最多 100ms 的阻塞收包,
     *   放在发送之后会把老师刚开始回的包当成旧包删掉(竞态)。 */
    FlushStaleReplies();

    /* FlushStaleReplies() 可能刚从 socket 里组装出一段老师语音。
     * 完整的先播，仍在传的继续等，不能直接开始录音后再把状态清掉。 */
    if (teacher_expect_ > 0 && teacher_bytes_ >= teacher_expect_) {
        PlayTeacherVoice();
    }
    if (teacher_bytes_ > 0) {
        Board::GetInstance().GetDisplay()->SetStatus("老师正在给你留言，请稍候...");
        return;
    }

    rec_len_ = 0;
    record_full_ = false;
    local_reply_shown_ = false;   // 新一轮对话, 重置"本地已回复"标记
    cloud_started_ = false;       // 新一轮对话, 重置云端兜底标记
    teacher_reply_pending_ = false;
    teacher_voice_pending_ = false;
    teacher_voice_ready_ = false;
    result_level_[0] = 0;
    StopLeds();
    treehole_state_ = kTreeHoleRecording;
    SetChatState(kChatStateListening);

    auto display = Board::GetInstance().GetDisplay();
    display->SetResult("", "正在聆听...");
    display->SetHelpText("");
    display->SetStatus("聆听中... 松开发送");
    last_record_ms_ = now_ms();
    ESP_LOGI(TAG, "开始录音");
}

/* 录音循环: 每次从麦克风取 30ms 音频追加到内存缓冲 */
void Application::RecordTick() {
    auto codec = Board::GetInstance().GetAudioCodec();

    /* 录音期间也把老师的语音收下来(只攒不播, 播放会打断录音) ——
     * 否则学生按住说话的这 10 秒里, 老师发的语音会被收件箱冲爆后丢掉 */
    CollectTeacherVoice();

    std::vector<int16_t> data;
    if (codec->InputData(data)) {
        uint32_t bytes = data.size() * sizeof(int16_t);
        uint32_t room = REC_BUF_SIZE - rec_len_;
        uint32_t copy_bytes = bytes < room ? bytes : room;
        if (copy_bytes > 0) {
            memcpy(rec_buf_ + WAV_HEADER_SIZE + rec_len_, data.data(), copy_bytes);
            rec_len_ += copy_bytes;
        }
    }

    /* 每 500ms 刷新一次屏幕上的录音秒数 */
    if (now_ms() - last_record_ms_ >= 500) {
        last_record_ms_ = now_ms();
        UpdateRecordScreen();
    }

    /* 录满 10 秒自动发送(不用等松开按键) */
    if (rec_len_ >= REC_BUF_SIZE && !record_full_) {
        record_full_ = true;
        SendWavToServer();
    }
}

/* 刷新录音中的屏幕提示 */
void Application::UpdateRecordScreen() {
    char buf[64];
    snprintf(buf, sizeof(buf), "正在录音 %d 秒  松开发送", (int)(rec_len_ / (REC_SAMPLE_RATE * 2)));
    Board::GetInstance().GetDisplay()->SetStatus(buf);
}

/* ====================================================================
 * 状态二: 停止录音并发送(松开对话键触发)
 * ==================================================================== */
void Application::StopRecordingAndSend() {
    if (treehole_state_ != kTreeHoleRecording) {
        return;
    }
    if (record_full_) {
        return;  // 已经录满自动发送过, 正在等待回复
    }
    if (rec_len_ < MIN_REC_BYTES) {
        // 录音太短视为误触, 直接回到待命
        treehole_state_ = kTreeHoleIdle;
        SetChatState(kChatStateIdle);
        Board::GetInstance().GetDisplay()->SetStatus("说话时间太短, 请重试");
        ESP_LOGI(TAG, "录音太短, 丢弃");
        return;
    }
    SendWavToServer();
}

/* 在录音缓冲最前面填上 44 字节 WAV 文件头 */
void Application::WriteWavHeader() {
    uint8_t *h = rec_buf_;
    uint32_t data_len = rec_len_;
    memcpy(h + 0, "RIFF", 4);
    put32(h + 4, 36 + data_len);
    memcpy(h + 8, "WAVE", 4);
    memcpy(h + 12, "fmt ", 4);
    put32(h + 16, 16);                 // fmt 块长度
    put16(h + 20, 1);                  // 格式: PCM
    put16(h + 22, 1);                  // 声道数: 单声道
    put32(h + 24, REC_SAMPLE_RATE);    // 采样率
    put32(h + 28, REC_SAMPLE_RATE * 2);// 字节率 = 采样率 × 2字节
    put16(h + 32, 2);                  // 块对齐
    put16(h + 34, 16);                 // 位深 16bit
    memcpy(h + 36, "data", 4);
    put32(h + 40, data_len);           // 音频数据长度
}

/* UDP 分包发送: 每包 = 魔数"XZSD" + 序号 + 长度 + 结束标志 + 数据 */
void Application::SendWavToServer() {
    treehole_state_ = kTreeHoleSending;
    SetChatState(kChatStateListening);
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus("正在发送...");
    display->SetResult("", "正在发送...");

    if (!wifi_connected_) {
        ESP_LOGW(TAG, "WiFi 未连接, 无法发送");
        ShowResult("", "网络未连接，请重试");
        return;
    }

    WriteWavHeader();
    uint32_t total = WAV_HEADER_SIZE + rec_len_;
    uint16_t seq = 0;
    uint32_t off = 0;
    uint8_t pkt[9 + UDP_CHUNK_SIZE];

    while (off < total) {
        uint16_t n = (uint16_t)((total - off > UDP_CHUNK_SIZE) ? UDP_CHUNK_SIZE : (total - off));
        memcpy(pkt, "XZSD", 4);          // 4字节魔数
        pkt[4] = (seq >> 8) & 0xFF;      // 2字节序号(大端)
        pkt[5] = seq & 0xFF;
        pkt[6] = (n >> 8) & 0xFF;        // 2字节本包数据长度(大端)
        pkt[7] = n & 0xFF;
        pkt[8] = (off + n >= total) ? 1 : 0; // 1字节标志: bit0=最后一包
        memcpy(pkt + 9, rec_buf_ + off, n);
        if (sendto(udp_sock_, pkt, 9 + n, 0,
                   (struct sockaddr *)&server_addr_, sizeof(server_addr_)) < 0) {
            ESP_LOGW(TAG, "UDP 发送失败(第%d包)", seq);
        }
        off += n;
        seq++;
        /* 避免几十个 UDP 包瞬间挤满教师端或校园网络队列。 */
        if (seq % 4 == 0 && off < total) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
    ESP_LOGI(TAG, "WAV 已发送: %u 字节, %u 包", (unsigned)total, (unsigned)seq);

    treehole_state_ = kTreeHoleWaiting;
    /* 学生说话的这段时间里老师可能已经发来一整段语音: 现在先放给学生听,
     * 放完再开始算"等回复"的 15 秒(不然播放期间的时间会把等待窗口吃掉) */
    if (teacher_expect_ > 0 && teacher_bytes_ >= teacher_expect_) {
        ESP_LOGI(TAG, "学生说话期间老师发来了语音, 先播老师的");
        PlayTeacherVoice();
    }
    wait_start_ms_ = now_ms();
    local_reply_shown_ = false;
    cloud_started_ = false;
    teacher_reply_pending_ = false;
    display->SetStatus("等待回复...");
}

/* ====================================================================
 * 状态三: 等待答复 —— 老师优先, 老师不在则由小智把话说完
 *   Step1 本地先安慰(1.5秒)  Step2 云端完整回答(4秒)  Step3 老师超时收尾(15秒)
 * ==================================================================== */
void Application::WaitReplyTick() {
    /* 先收老师包再判断 1.5/4/15 秒门槛。否则恰好已在 socket 中的老师回复
     * 会先触发本地/云端，再到本轮末尾才被看到，违背“老师始终优先”。 */
    PollIdlePackets();
    if (treehole_state_ != kTreeHoleWaiting) {
        return;
    }
    uint32_t waited_ms = now_ms() - wait_start_ms_;

    /* ① 老师在 1.5 秒内没回 → 终端先给本地安慰回复, 让同学马上有回应 */
    if (!local_reply_shown_ && !teacher_voice_pending_ && waited_ms >= LOCAL_REPLY_DELAY_MS) {
        ShowLocalReply();
    }

    /* ② 老师到 4 秒还没回 → 交给小智云端: 上传刚才的录音, 云端组织完整回答并播报 */
#if ENABLE_CLOUD_FALLBACK
    if (!cloud_started_ && !teacher_voice_pending_ && waited_ms >= CLOUD_FALLBACK_DELAY_MS) {
        CloudFallbackTick();   // 阻塞直到云端答完(或连不上云端)
        return;
    }
#endif

    /* ③ 老师端总超时 15 秒 */
    if (waited_ms > REPLY_TIMEOUT_MS) {
        if (cloud_started_) {
            // 云端已经答过(或正在答) → 保留当前内容
            treehole_state_ = kTreeHoleShowing;
            Board::GetInstance().GetDisplay()->SetStatus("老师稍后会回复你");
        } else if (local_reply_shown_) {
            // 只有本地安慰回复 → 保留安慰语
            ESP_LOGW(TAG, "教师端超时未回复, 保留本地回复");
            treehole_state_ = kTreeHoleShowing;
            Board::GetInstance().GetDisplay()->SetStatus("老师稍后会回复你");
        } else {
            ESP_LOGW(TAG, "等待回复超时");
            ShowResult("", "网络未连接，请重试");
        }
        return;
    }

    /* 老师回复已在本轮开头批量收取。 */
}

/* 教师端未回复时, 终端自己先给出的陪伴式回复
 * 特点: 不判定级别(不替老师下结论), 只安抚 + 说明老师在赶来 */
void Application::ShowLocalReply() {
    local_reply_shown_ = true;
    local_reply_index_ = (local_reply_index_ + 1) % LOCAL_REPLY_COUNT;  // 轮流用, 不重复
    result_text_ = LOCAL_REPLY_TEXTS[local_reply_index_];
    strncpy(result_level_, "local", sizeof(result_level_) - 1);         // 本地回复样式(橙色灯)
    result_level_[sizeof(result_level_) - 1] = 0;

    auto display = Board::GetInstance().GetDisplay();
    display->SetResult("local", result_text_);   // 显示"陪伴"图标(爱心笑脸)
    display->SetHelpText("");                    // 未判定级别, 暂不显示热线
    display->SetStatus("老师正在赶来...");
    ESP_LOGI(TAG, "教师端未回复, 终端先本地回复");
}

/* ====================================================================
 * 小智云端完整回答(核心: 老师不在时机器人也把话说完)
 *   1) 打开云端语音通道
 *   2) 把刚录的 16kHz PCM 按 60ms 一帧编码上传, 再发"说完了"
 *   3) 循环接收云端返回的回答文字(上屏) 与 语音(解码播放)
 *   4) 期间如果老师端回复到了, 记下来, 答完后以老师的判定为准
 * ==================================================================== */
void Application::CloudFallbackTick() {
    cloud_started_ = true;
    treehole_state_ = kTreeHoleCloud;
    SetChatState(kChatStateSpeaking);

    auto display = Board::GetInstance().GetDisplay();
    auto codec = Board::GetInstance().GetAudioCodec();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        cloud_sentences_.clear();
        audio_decode_queue_.clear();
    }
    tts_finished_ = false;
    teacher_reply_pending_ = false;

#if !ENABLE_CLOUD_FALLBACK
    display->SetStatus("老师稍后会回复你");
    treehole_state_ = kTreeHoleShowing;
    SetChatState(kChatStateIdle);
    return;
#else
    /* 1) 打开云端通道 */
    if (protocol_ == nullptr || !protocol_->OpenAudioChannel()) {
        ESP_LOGW(TAG, "小智云端连接失败(可能断网或设备未激活)");
        display->SetStatus("小智暂时联系不上, 老师稍后回复你");
        result_level_[0] = 0;
        strncpy(result_level_, "local", sizeof(result_level_) - 1);  // 保留本地安慰样式
        treehole_state_ = kTreeHoleShowing;
        SetChatState(kChatStateIdle);
        return;
    }

    display->SetStatus("小智正在回答...");
    strncpy(result_level_, "cloud", sizeof(result_level_) - 1);       // 绿色呼吸灯
    result_level_[sizeof(result_level_) - 1] = 0;

    /* 2) 上传刚才这段录音: 16kHz PCM → Opus(60ms一帧) */
    protocol_->SendStartListening(kListeningModeManualStop);
    if (opus_encoder_ != nullptr) {
        opus_encoder_->ResetState();
    }
    const int frame_samples = REC_SAMPLE_RATE * OPUS_FRAME_DURATION_MS / 1000;  // 960
    const int16_t *pcm = (const int16_t *)(rec_buf_ + WAV_HEADER_SIZE);
    uint32_t total_samples = rec_len_ / 2;

    for (uint32_t off = 0; off < total_samples; off += frame_samples) {
        std::vector<int16_t> frame(frame_samples, 0);   // 末帧补零
        uint32_t n = total_samples - off;
        if (n > (uint32_t)frame_samples) {
            n = frame_samples;
        }
        memcpy(frame.data(), pcm + off, n * sizeof(int16_t));

        // 编码是同步的, 回调里只把结果收集起来, 保证协议调用都在主循环线程
        std::vector<std::vector<uint8_t>> opus_frames;
        opus_encoder_->Encode(std::move(frame), [&opus_frames](std::vector<uint8_t>&& opus) {
            opus_frames.emplace_back(std::move(opus));
        });
        for (auto &f : opus_frames) {
            if (protocol_->IsAudioChannelOpened()) {
                protocol_->SendAudio(f);
            }
        }
        UpdateLedBreathing();   // 保持呼吸灯
    }
    protocol_->SendStopListening();   // 告诉云端"我说完了"

    /* 3) 等云端回答: 有语音就播, 有文字就上屏 */
    cloud_start_ms_ = now_ms();
    codec->EnableOutput(true);
    while (now_ms() - cloud_start_ms_ < CLOUD_ANSWER_TIMEOUT_MS) {
        /* 老师回复/老师语音优先: 一到就结束云端回答, 改用老师的内容
         * ★ 先用 UdpHasData() 判有没有数据再收: recvfrom 会阻塞满 SO_RCVTIMEO(100ms),
         *   每帧音频前都白等 100ms 的话, 喂给 I2S 的节奏跟不上(缓冲只有约 180ms),
         *   云端回答听起来就会断断续续。 */
        if (UdpHasData()) {
            PollTeacherReply();
        }
        if (teacher_reply_pending_ || teacher_voice_pending_ || teacher_voice_ready_) {
            protocol_->CloseAudioChannel();
            teacher_voice_pending_ = false;
            if (teacher_voice_ready_) {
                /* 老师的语音已经收齐: 直接播老师的话 */
                teacher_voice_ready_ = false;
                treehole_state_ = kTreeHoleShowing;
                ESP_LOGI(TAG, "云端回答期间收到老师语音, 改用老师的话");
                PlayTeacherVoice();
                return;
            }
            if (teacher_reply_pending_) {
                ESP_LOGI(TAG, "云端回答期间收到老师回复, 改用老师判定");
                ShowResult(pending_level_, pending_text_);
                SendTeacherAck(ACK_KIND_TEXT, ACK_STATUS_OK);
                return;
            }
            /* 老师语音还在传: 先退出云端回答, 剩下的包由待命轮询收完并自动播放 */
            ESP_LOGI(TAG, "云端回答期间老师正在发语音, 退出云端把喇叭让给老师");
            treehole_state_ = kTreeHoleShowing;
            Board::GetInstance().GetDisplay()->SetStatus("老师正在给你留言...");
            return;
        }

        /* 取一帧云端语音播放 */
        std::vector<uint8_t> opus;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!audio_decode_queue_.empty()) {
                opus = std::move(audio_decode_queue_.front());
                audio_decode_queue_.pop_front();
            }
        }
        if (!opus.empty()) {
            std::vector<int16_t> out_pcm;
            if (opus_decoder_ != nullptr && opus_decoder_->Decode(std::move(opus), out_pcm)) {
                // 云端 24kHz → 本机功放 8kHz, 需要重采样
                if (opus_decode_sample_rate_ != codec->output_sample_rate()) {
                    int target = output_resampler_.GetOutputSamples(out_pcm.size());
                    std::vector<int16_t> resampled(target);
                    output_resampler_.Process(out_pcm.data(), out_pcm.size(), resampled.data());
                    out_pcm = std::move(resampled);
                }
                codec->OutputData(out_pcm);   // 阻塞写I2S, 节奏自动对齐
            }
            UpdateLedBreathing();
            continue;
        }
        if (tts_finished_) {
            break;      // 云端说完了, 且语音已播完
        }
        vTaskDelay(pdMS_TO_TICKS(20));
        UpdateLedBreathing();
    }
    protocol_->CloseAudioChannel();

    /* 4) 收尾: 老师的判定优先, 否则用云端回答 */
    if (teacher_reply_pending_) {
        ShowResult(pending_level_, pending_text_);
        SendTeacherAck(ACK_KIND_TEXT, ACK_STATUS_OK);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!cloud_sentences_.empty()) {
            result_text_.clear();
            for (auto &s : cloud_sentences_) {
                if (!result_text_.empty()) {
                    result_text_ += "\n";
                }
                result_text_ += s;
            }
            result_level_[0] = 0;
            strncpy(result_level_, "cloud", sizeof(result_level_) - 1);
            treehole_state_ = kTreeHoleShowing;
            SetChatState(kChatStateIdle);
            display->SetStatus("小智回复");
            display->SetResult("cloud", result_text_);
            display->SetHelpText("");
            UpdateLedBreathing();
            ESP_LOGI(TAG, "小智云端回答完成");
            return;
        }
    }

    /* 云端没给出内容 → 保留本地安慰回复 */
    display->SetStatus("小智暂时没有回复, 老师稍后会回复你");
    treehole_state_ = kTreeHoleShowing;
    SetChatState(kChatStateIdle);
#endif // ENABLE_CLOUD_FALLBACK
}

/* 把云端已经说出的句子拼起来显示(只保留最近几句, 适配小屏) */
void Application::ShowCloudText() {
    std::string text;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto &s : cloud_sentences_) {
            if (!text.empty()) {
                text += "\n";
            }
            text += s;
        }
    }
    if (text.empty()) {
        return;
    }
    result_text_ = text;
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus("小智回复中...");
    display->SetResult("cloud", text);
}

/* 清掉套接字里"上一轮遗留"的回复包
 * 场景: 老师答得太晚(超过15秒), 或者老师端主动发消息时终端没在等待,
 * 这些数据包会留在缓冲区里, 如果不清理, 下一次对话开始时就会被当成
 * 本轮的结果 → 显示错乱。所以在开始新一轮之前先倒掉。 */
void Application::FlushStaleReplies() {
    if (udp_sock_ < 0) {
        return;
    }
    char buf[UDP_CHUNK_SIZE + 10]; // 1209B XZSV 或 1024B JSON，再留1B字符串结尾
    int dropped = 0;
    for (int got = 0; got < 32; got++) {    // 最多清 32 个, 防止异常情况下卡住
        int r = recvfrom(udp_sock_, buf, sizeof(buf) - 1, 0, NULL, NULL);
        if (r <= 0) {
            break;
        }
        if (r >= 9 && memcmp(buf, VOICE_MAGIC, 4) == 0) {
            /* ★ 老师语音不是"过期回复", 不能扔:
             *   学生刚才说话的这十几秒里老师可能已经发来一段语音,
             *   这里把它收进缓冲(收齐了由 SendWavToServer 播出来)。 */
            buf[r] = '\0';
            AssembleTeacherVoice(buf, r);
        } else {
            dropped++;
        }
    }
    if (dropped > 0) {
        ESP_LOGW(TAG, "清掉 %d 个过期回复包", dropped);
    }
}

/* 收一次老师端 UDP 回复
 *  - 普通等待阶段: 直接按老师判定显示
 *  - 云端回答阶段: 先存起来(老师优先), 由 CloudFallbackTick 收尾时显示
 */
bool Application::PollTeacherReply() {
    char buf[UDP_CHUNK_SIZE + 10]; // 云端期也必须能完整接下1209B语音包
    int n = recvfrom(udp_sock_, buf, sizeof(buf) - 1, 0, NULL, NULL);
    if (n <= 0) {
        return false;
    }
    buf[n] = '\0';

    /* 老师在等待/云端回答期间发来的语音包: 也要收下来拼好
     * (以前这里一律当文字回复处理, 老师的语音会被直接丢掉) */
    if (n >= 4 && memcmp(buf, VOICE_MAGIC, 4) == 0) {
        HandleIdlePacket(buf, n);
        return false;
    }

    if (treehole_state_ != kTreeHoleCloud) {
        HandleReply(buf);
        return true;
    }

    cJSON *root = cJSON_Parse(buf);
    if (root == nullptr) {
        return false;
    }
    const char *level = "";
    const char *text = "";
    cJSON *item = cJSON_GetObjectItem(root, "level");
    if (item != nullptr && cJSON_IsString(item)) {
        level = item->valuestring;
    }
    item = cJSON_GetObjectItem(root, "text");
    if (item != nullptr && cJSON_IsString(item)) {
        text = item->valuestring;
    }
    if (!is_teacher_level(level)) {
        ESP_LOGW(TAG, "忽略非法老师级别: %s", level);
        cJSON_Delete(root);
        return false;
    }
    strncpy(pending_level_, level, sizeof(pending_level_) - 1);
    pending_level_[sizeof(pending_level_) - 1] = 0;
    pending_text_ = text;
    teacher_reply_pending_ = true;
    cJSON_Delete(root);
    return true;
}

/* ============ 云端版本检查(原版逻辑保留: 完成设备激活/版本校验) ============
 * 注意: 本固件是项目定制固件, 这里只检查、不执行云端自动升级,
 * 避免云端把"解忧树洞"固件覆盖成官方固件(要升级时自己 idf.py flash) */
void Application::CheckNewVersion() {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    ota_.SetPostData(board.GetJson());

    while (true) {
        if (ota_.CheckVersion()) {
            if (ota_.HasNewVersion()) {
                ESP_LOGW(TAG, "云端有新版本 %s, 树洞固件不自动升级", ota_.GetFirmwareVersion().c_str());
                display->ShowNotification("有新版本(不自动升级)");
            } else {
                ota_.MarkCurrentVersionValid();
            }
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(60000));   // 60 秒后重试(设备未激活/断网时)
    }
}

/* 云端语音采样率与解码器准备(原版逻辑) */
void Application::SetDecodeSampleRate(int sample_rate) {
    if (opus_decode_sample_rate_ == sample_rate && opus_decoder_ != nullptr) {
        return;
    }
    opus_decode_sample_rate_ = sample_rate;
    opus_decoder_ = std::make_unique<OpusDecoderWrapper>(opus_decode_sample_rate_, 1);

    auto codec = Board::GetInstance().GetAudioCodec();
    if (opus_decode_sample_rate_ != codec->output_sample_rate()) {
        ESP_LOGI(TAG, "云端音频 %d Hz → 本机功放 %d Hz, 需要重采样",
                 opus_decode_sample_rate_, codec->output_sample_rate());
        output_resampler_.Configure(opus_decode_sample_rate_, codec->output_sample_rate());
    }
}

/* 解析服务端返回的 JSON: {"level":"green|yellow|red","text":"..."} */
void Application::HandleReply(const char *json) {
    cJSON *root = cJSON_Parse(json);
    if (root == nullptr) {
        ESP_LOGW(TAG, "JSON 解析失败: %s", json);
        ShowResult("", "服务端回复格式错误");
        SendTeacherAck(ACK_KIND_TEXT, ACK_STATUS_DENY);
        return;
    }
    const char *level = "";
    const char *text = "";
    cJSON *item = cJSON_GetObjectItem(root, "level");
    if (item != nullptr && cJSON_IsString(item)) {
        level = item->valuestring;
    }
    item = cJSON_GetObjectItem(root, "text");
    if (item != nullptr && cJSON_IsString(item)) {
        text = item->valuestring;
    }
    if (!is_teacher_level(level)) {
        ESP_LOGW(TAG, "老师回复级别无效: %s", level);
        cJSON_Delete(root);
        ShowResult("", "服务端回复级别错误");
        SendTeacherAck(ACK_KIND_TEXT, ACK_STATUS_DENY);
        return;
    }
    ShowResult(level, text);
    cJSON_Delete(root);
    SendTeacherAck(ACK_KIND_TEXT, ACK_STATUS_OK);           // 已经显示给学生了
}

/* ====================================================================
 * 状态四: 显示结果(屏幕 + 灯效 + 红级求助音)
 * ==================================================================== */
void Application::ShowResult(const std::string &level, const std::string &text) {
    strncpy(result_level_, level.c_str(), sizeof(result_level_) - 1);
    result_level_[sizeof(result_level_) - 1] = 0;
    result_text_ = text;
    local_reply_shown_ = false;   // 已收到正式(或错误)结果, 清掉本地回复标记
    treehole_state_ = kTreeHoleShowing;
    SetChatState(kChatStateIdle);

    auto display = Board::GetInstance().GetDisplay();
    if (strcmp(result_level_, "red") == 0) {
        // 红级: 显示热线+心理老师, 播放求助提示音
        display->SetStatus("红色警报: 请及时求助");
        display->SetResult(result_level_, result_text_);
        display->SetHelpText(HOTLINE_TEXT "\n" TEACHER_TEXT);
        PlayHelpAudio();
    } else if (strcmp(result_level_, "yellow") == 0) {
        display->SetStatus("黄色提醒");
        display->SetResult(result_level_, result_text_);
        display->SetHelpText("");
    } else if (strcmp(result_level_, "green") == 0) {
        display->SetStatus("一切安好");
        display->SetResult(result_level_, result_text_);
        display->SetHelpText("");
    } else {
        // 普通提示(含"网络未连接，请重试"等错误信息)
        display->SetStatus("提示");
        display->SetResult("", result_text_);
        display->SetHelpText("");
    }
    UpdateLedBreathing();  // 立即刷一次灯效
}

/* ====================================================================
 * 通用 WAV 播放(求助提示音 / 老师语音 都用它)
 *   入参是完整 WAV 数据(44字节标准头 + PCM), 按 8kHz 单声道播放
 * ==================================================================== */
bool Application::PlayWavBuffer(const uint8_t *wav, size_t size, const char *what) {
    if (wav == nullptr || size < WAV_HEADER_SIZE || memcmp(wav, "RIFF", 4) != 0 || memcmp(wav + 8, "WAVE", 4) != 0) {
        ESP_LOGE(TAG, "%s 数据无效(不是WAV)", what);
        return false;
    }
    uint32_t rate = wav[24] | (wav[25] << 8) | (wav[26] << 16) | ((uint32_t)wav[27] << 24);
    if (rate != 8000) {
        ESP_LOGW(TAG, "%s 采样率 %u Hz(不是8kHz), 播放会变调", what, (unsigned)rate);
    }
    uint32_t data_size = wav[40] | (wav[41] << 8) | (wav[42] << 16) | ((uint32_t)wav[43] << 24);
    size_t avail = size - WAV_HEADER_SIZE;
    uint32_t total_samples = (uint32_t)((data_size > 0 && data_size < avail ? data_size : avail) / 2);
    if (total_samples == 0) {
        ESP_LOGW(TAG, "%s 没有音频数据", what);
        return false;
    }

    auto codec = Board::GetInstance().GetAudioCodec();
    const int16_t *samples = (const int16_t *)(wav + WAV_HEADER_SIZE);
    ESP_LOGI(TAG, "播放%s: %u 个采样点", what, (unsigned)total_samples);

    /* 分块播放: 每块 400 采样(50ms)。写 I2S 会阻塞等待 DMA 空间, 节奏自动对齐 8kHz */
    std::vector<int16_t> chunk(400);
    uint32_t off = 0;
    while (off < total_samples) {
        uint32_t n = total_samples - off;
        if (n > 400) {
            n = 400;
        }
        chunk.assign(samples + off, samples + off + n);
        codec->OutputData(chunk);
        off += n;
        UpdateLedBreathing();  // 播放期间保持呼吸灯
    }
    return true;
}

/* 播放 Flash 内置的求助提示音(8kHz/16bit/单声道 WAV) */
void Application::PlayHelpAudio() {
    SetChatState(kChatStateSpeaking);
    Board::GetInstance().GetDisplay()->SetStatus("正在播放求助提示...");
    PlayWavBuffer((const uint8_t *)help_wav_start, help_wav_end - help_wav_start, "求助提示音");
    SetChatState(kChatStateIdle);
}

/* ====================================================================
 * 老师语音: 收齐后播放(蓝灯呼吸 + 屏幕提示), 播完恢复原来的画面
 * ==================================================================== */
void Application::BeginTeacherSegment() {
    /* 只清"本段"的记账, 不动缓冲内容: 缓冲在首次落位时才整体清零,
     * 这样"末包/中间包先到、seq0 后到"的乱序分片不会被清掉。 */
    teacher_bytes_ = 0;
    teacher_expect_ = 0;
    memset(teacher_seen_, 0, sizeof(teacher_seen_));
    teacher_have_seq0_ = false;
    teacher_voice_ready_ = false;
    teacher_voice_rejected_ = false;
}

void Application::ResetTeacherBuffer() {
    BeginTeacherSegment();
    teacher_voice_pending_ = false;
    teacher_last_packet_ms_ = 0;
}

/* 给教师端回一个送达确认(6 字节固定长): XZAK + kind + status
 * 发到教师端服务端口(server_addr_), 教师端据此把"等待确认"改成"已送达/被拒绝" */
void Application::SendTeacherAck(uint8_t kind, uint8_t status) {
    if (udp_sock_ < 0 || !wifi_connected_) {
        return;
    }
    uint8_t pkt[6] = { 'X', 'Z', 'A', 'K', kind, status };
    if (sendto(udp_sock_, pkt, sizeof(pkt), 0,
               (struct sockaddr *)&server_addr_, sizeof(server_addr_)) < 0) {
        ESP_LOGW(TAG, "送达确认发送失败");
    }
}

/* 每 10 秒给教师端发一次心跳(8 字节固定长): XZHB + 开机秒数(大端)
 * 教师端用它显示"终端在线/离线"; 收到任何合法终端包也会刷新活跃时间 */
void Application::SendHeartbeat() {
    if (udp_sock_ < 0 || !wifi_connected_) {
        return;
    }
    uint32_t up_s = (uint32_t)(esp_timer_get_time() / 1000000);
    uint8_t pkt[8] = { 'X', 'Z', 'H', 'B',
                       (uint8_t)(up_s >> 24), (uint8_t)(up_s >> 16),
                       (uint8_t)(up_s >> 8), (uint8_t)up_s };
    if (sendto(udp_sock_, pkt, sizeof(pkt), 0,
               (struct sockaddr *)&server_addr_, sizeof(server_addr_)) < 0) {
        ESP_LOGW(TAG, "心跳发送失败");
    }
}

void Application::ExpireTeacherVoice() {
    if (teacher_bytes_ == 0 || teacher_last_packet_ms_ == 0 ||
        (teacher_expect_ > 0 && teacher_bytes_ >= teacher_expect_)) {
        return;
    }
    if (now_ms() - teacher_last_packet_ms_ < VOICE_RECEIVE_TIMEOUT_MS) {
        return;
    }
    ESP_LOGW(TAG, "老师语音等待末包超时, 已丢弃 %u 字节", (unsigned)teacher_bytes_);
    ResetTeacherBuffer();
    SendTeacherAck(ACK_KIND_VOICE, ACK_STATUS_DENY);   // 没收全 → 告诉老师重录
    auto display = Board::GetInstance().GetDisplay();
    if (treehole_state_ == kTreeHoleWaiting) {
        display->SetStatus("老师语音未收完整，继续等待...");
    } else {
        display->SetStatus("老师语音未收完整，请老师重发");
    }
}

void Application::PlayTeacherVoice() {
    if (teacher_buf_ == nullptr || teacher_expect_ == 0) {
        return;
    }
    auto display = Board::GetInstance().GetDisplay();

    /* 先记下当前画面, 播完恢复(避免把老师/云端给的结果冲掉) */
    char prev_level[8];
    strncpy(prev_level, result_level_, sizeof(prev_level) - 1);
    prev_level[sizeof(prev_level) - 1] = 0;
    std::string prev_text = result_text_;

    playing_voice_ = true;
    SetChatState(kChatStateSpeaking);
    display->SetStatus("老师在跟你说话...");
    display->SetResult("voice", "老师给你留了一段话，好好听哦");
    display->SetHelpText("");

    /* 老师发来的就是一份完整 WAV(含 44 字节头), 所以从缓冲开头播、长度就是收齐的字节数 */
    bool ok = PlayWavBuffer(teacher_buf_, teacher_expect_, "老师语音");
    playing_voice_ = false;
    ResetTeacherBuffer();

    /* 恢复: 之前有结果就恢复结果, 否则回到待命 */
    if (prev_level[0] != 0) {
        strncpy(result_level_, prev_level, sizeof(result_level_) - 1);
        result_level_[sizeof(result_level_) - 1] = 0;
        result_text_ = prev_text;
        display->SetResult(result_level_, result_text_);
        display->SetHelpText(strcmp(result_level_, "red") == 0 ? HOTLINE_TEXT "\n" TEACHER_TEXT : "");
        display->SetStatus(strcmp(result_level_, "red") == 0 ? "红色警报: 请及时求助" : "提示");
    } else {
        result_level_[0] = 0;
        StopLeds();
        /* 按当前真实状态显示: 学生还在等老师回复时不能写"待命"(会让他以为可以按按钮了) */
        if (treehole_state_ == kTreeHoleWaiting || treehole_state_ == kTreeHoleCloud) {
            display->SetStatus("老师正在回复...");
        } else {
            display->SetStatus("待命: 按住对话键说话");
        }
    }
    SetChatState(kChatStateIdle);
    if (ok) {
        ESP_LOGI(TAG, "老师语音播放完毕");
        SendTeacherAck(ACK_KIND_VOICE, ACK_STATUS_OK);      // 播完了 → 老师端显示"已送达"
    } else {
        SendTeacherAck(ACK_KIND_VOICE, ACK_STATUS_DENY);    // 播不出来也要让老师知道
    }
}

/* 把一个 "XZSV" 分片落进老师语音缓冲; 返回 true 表示这是"最后一包"(已经收齐)
 *
 * 落位规则: 老师端从 WAV 的第 0 个字节(也就是 'R')开始切包, 所以这里直接
 *   落在 teacher_buf_ 的开头, 不能再收 +44 的偏移 ——
 *   PlayWavBuffer() 会在偏移 0 校验 "RIFF"/"WAVE"、在偏移 24 读采样率、偏移 40 读长度,
 *   要是把数据挪到 +44, 它读到的就是未初始化的内存, 会判"数据无效"直接不播。
 */
bool Application::AssembleTeacherVoice(const char *buf, int len) {
    if (teacher_buf_ == nullptr || len < 9) {
        return false;
    }
    uint16_t seq  = ((uint16_t)(uint8_t)buf[4] << 8) | (uint8_t)buf[5];
    uint16_t n    = ((uint16_t)(uint8_t)buf[6] << 8) | (uint8_t)buf[7];
    bool     last = ((uint8_t)buf[8] & 0x01) == 1;
    uint32_t off  = (uint32_t)seq * UDP_CHUNK_SIZE;    // 老师端用固定 1200 字节分片

    /* 单缓冲只能保留一段：上一段已收齐但还没播放时，任何新分片（包括 seq0）
     * 都不能触发清空或覆盖。播完后 ResetTeacherBuffer() 才允许下一段进入。 */
    if (teacher_expect_ > 0 && teacher_bytes_ >= teacher_expect_) {
        ESP_LOGW(TAG, "上一段老师语音还没播放, 忽略新分片 seq=%u", (unsigned)seq);
        return false;
    }

    /* 分片长度必须自洽: 非末包必须正好 1200 字节, 且声明长度要和实际长度一致 */
    if (n > UDP_CHUNK_SIZE || n != len - 9 || (!last && n != UDP_CHUNK_SIZE)) {
        ESP_LOGW(TAG, "老师语音分片不合法: seq=%u len=%u udp=%d last=%d",
                 (unsigned)seq, (unsigned)n, len, last);
        ResetTeacherBuffer();
        teacher_voice_rejected_ = true;
        SendTeacherAck(ACK_KIND_VOICE, ACK_STATUS_DENY);
        return false;
    }
    if (off + n > VOICE_MAX_BYTES) {
        ESP_LOGW(TAG, "老师语音超出缓冲(%u > %u), 丢弃整段", (unsigned)(off + n),
                 (unsigned)VOICE_MAX_BYTES);
        ResetTeacherBuffer();
        teacher_voice_rejected_ = true;
        SendTeacherAck(ACK_KIND_VOICE, ACK_STATUS_DENY);
        return false;
    }

    /* 段边界判定(支持乱序):
     * ① 同一段里重复到达的 seq0(重传/乱序) → 只覆盖这一片, 不清已收到的分片;
     * ② seq0 迟到(前面已经收过"预头分片") → 保留那些分片, 只把头补上;
     * ③ 其它 seq0 → 开新的一段(清记账, 缓冲内容在首次落位时整体清零);
     * ④ 非 seq0 且距上一包超过回收时间 → 上一段的残留, 清掉且不收这一包。*/
    bool recent = (teacher_last_packet_ms_ != 0 &&
                   now_ms() - teacher_last_packet_ms_ <= VOICE_RECEIVE_TIMEOUT_MS);
    if (seq == 0 && teacher_have_seq0_ && teacher_seen_[0] && recent) {
        /* ① 同段重复 seq0: 什么都不清 */
    } else if (seq == 0 && !teacher_have_seq0_ && teacher_bytes_ > 0 && recent) {
        /* ② 预头分片已在缓冲里, 现在补头: 保留分片, 只清"非法段"标记 */
        teacher_voice_rejected_ = false;
    } else if (seq == 0) {
        BeginTeacherSegment();      /* ③ */
    } else if (!recent && teacher_last_packet_ms_ != 0) {
        ResetTeacherBuffer();       /* ④ */
        return false;
    }
    if (teacher_voice_rejected_) {
        return false;   // 本段已被判非法, 等下一个 seq=0 重开
    }
    if (seq == 0) {
        const uint8_t *wav = (const uint8_t *)buf + 9;
        if (n < WAV_HEADER_SIZE) {
            ESP_LOGW(TAG, "老师语音 WAV 头不完整，整段丢弃");
            ResetTeacherBuffer();
            teacher_voice_rejected_ = true;
            SendTeacherAck(ACK_KIND_VOICE, ACK_STATUS_DENY);
            return false;
        }
        uint32_t rate = wav[24] | ((uint32_t)wav[25] << 8) | ((uint32_t)wav[26] << 16) |
                        ((uint32_t)wav[27] << 24);
        uint16_t format = wav[20] | ((uint16_t)wav[21] << 8);
        uint16_t channels = wav[22] | ((uint16_t)wav[23] << 8);
        uint16_t bits = wav[34] | ((uint16_t)wav[35] << 8);
        if (memcmp(wav, "RIFF", 4) != 0 || memcmp(wav + 8, "WAVE", 4) != 0 ||
            memcmp(wav + 12, "fmt ", 4) != 0 || memcmp(wav + 36, "data", 4) != 0 ||
            format != 1 || channels != 1 || rate != 8000 || bits != 16) {
            ESP_LOGW(TAG, "老师语音 WAV 头不合法，整段丢弃");
            ResetTeacherBuffer();
            teacher_voice_rejected_ = true;
            SendTeacherAck(ACK_KIND_VOICE, ACK_STATUS_DENY);
            return false;
        }
        teacher_have_seq0_ = true;
    }
    /* 非 seq0 且还没见过 seq0 → "预头暂存": 乱序先到的中间包/末包也收下, 等 seq0 补头 */

    if (teacher_bytes_ == 0) {
        /* 本段第一次落位: 整体清零, 丢包处按静音播放, 也不混入上一段的旧音频 */
        memset(teacher_buf_, 0, VOICE_MAX_BYTES);
    }
    memcpy(teacher_buf_ + off, buf + 9, n);
    if (!teacher_seen_[seq]) {
        teacher_seen_[seq] = 1;
        teacher_bytes_ += n;
    }
    teacher_last_packet_ms_ = now_ms();
    if (last) {
        teacher_expect_ = off + n;
    }
    /* 完整条件: 见过 seq0 + 见过末包 + 实收字节够 + 0..last_seq 全部到齐 */
    if (!teacher_have_seq0_ || teacher_expect_ == 0 || teacher_bytes_ < teacher_expect_) {
        return false;
    }
    uint16_t last_seq = (uint16_t)((teacher_expect_ - 1) / UDP_CHUNK_SIZE);
    for (uint16_t i = 0; i <= last_seq; ++i) {
        if (!teacher_seen_[i]) {
            return false;  // 末包可能先到；等齐中间包再播放
        }
    }
    uint32_t riff_size = teacher_buf_[4] | ((uint32_t)teacher_buf_[5] << 8) |
                         ((uint32_t)teacher_buf_[6] << 16) | ((uint32_t)teacher_buf_[7] << 24);
    uint32_t data_size = teacher_buf_[40] | ((uint32_t)teacher_buf_[41] << 8) |
                         ((uint32_t)teacher_buf_[42] << 16) | ((uint32_t)teacher_buf_[43] << 24);
    if (teacher_expect_ < WAV_HEADER_SIZE || riff_size != teacher_expect_ - 8 ||
        data_size != teacher_expect_ - WAV_HEADER_SIZE) {
        ESP_LOGW(TAG, "老师语音 WAV 长度声明不一致(%u/%u, 实际%u)，整段丢弃",
                 (unsigned)(riff_size + 8), (unsigned)(data_size + WAV_HEADER_SIZE),
                 (unsigned)teacher_expect_);
        ResetTeacherBuffer();
        teacher_voice_rejected_ = true;
        SendTeacherAck(ACK_KIND_VOICE, ACK_STATUS_DENY);
        return false;
    }
    ESP_LOGI(TAG, "老师语音收齐: %u 字节(实收 %u)", (unsigned)teacher_expect_, (unsigned)teacher_bytes_);
    return true;
}

/* 录音/发送期间也把网络上的包收一收(只攒不播)
 *
 * 为什么需要: 学生按住说话的这 10 秒里, 老师很可能正在发语音。
 * 如果这期间完全不收包, 终端 UDP 收件箱(32 个)会被冲爆, 剩下的包被 lwIP 丢掉,
 * 老师的语音就只剩前 0.16 秒 —— 听起来就是坏的。
 *
 * 为什么不能在这里播放: PlayWavBuffer() 是阻塞式的(一直写到 I2S 放完为止),
 * 在这里播放会把正在录音的学生声音录坏(输入缓冲被覆盖), 所以只落位, 不播。
 * 收齐的语音由 SendWavToServer() 在"一轮结束、开始等回复之前"播出来。
 */
void Application::CollectTeacherVoice() {
    if (udp_sock_ < 0 || teacher_buf_ == nullptr) {
        return;
    }
    for (int i = 0; i < 64; i++) {          // 一轮最多收 64 个, 和 PollIdlePackets 一致
        if (!UdpHasData()) {
            return;
        }
        char buf[1600];
        int n = recvfrom(udp_sock_, buf, sizeof(buf) - 1, 0, NULL, NULL);
        if (n <= 0) {
            return;
        }
        if (n >= 9 && memcmp(buf, VOICE_MAGIC, 4) == 0) {
            AssembleTeacherVoice(buf, n); // 只落位, 不播放
            /* AssembleTeacherVoice 遇到收齐后的重复/后续分片会返回 false，但已收齐的
             * 缓冲仍然有效；状态必须按缓冲本身重算，不能把 ready 错改成 pending。 */
            bool ready = teacher_expect_ > 0 && teacher_bytes_ >= teacher_expect_;
            teacher_voice_ready_ = ready;
            teacher_voice_pending_ = !ready && teacher_bytes_ > 0;
        }
        /* 其它包(上一轮的过期文字回复)直接丢, 与原来的语义一致 */
    }
}

/* 套接字里现在有数据吗? (0 超时, 绝不阻塞)
 * 直接用 recvfrom 会阻塞满 SO_RCVTIMEO(100ms) —— 在云端播报的循环里这么干,
 * 会把喂给 I2S 的节奏拖慢, 老师/云端的声音就会断断续续。
 */
bool Application::UdpHasData() {
    if (udp_sock_ < 0) {
        return false;
    }
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(udp_sock_, &rfds);
    struct timeval tv = { .tv_sec = 0, .tv_usec = 0 };
    return select(udp_sock_ + 1, &rfds, NULL, NULL, &tv) > 0;
}

/* 处理待命期间收到的包:
 *   "XZSV" → 老师语音分片(按偏移直接落位, 收齐即播)
 *   '{'    → 老师迟到的文字回复(直接显示, 不再丢弃)
 */
void Application::HandleIdlePacket(char *buf, int len) {
    if (len < 9) {
        return;
    }
    buf[len] = '\0';   // 后面按字符串解析, 得自己补结尾(调用方的缓冲留了空位)
    if (memcmp(buf, VOICE_MAGIC, 4) == 0) {
        bool complete = AssembleTeacherVoice(buf, len);
        if (treehole_state_ == kTreeHoleCloud) {
            /* 云端还在播报: 先不让两边同时出声 */
            if (complete) {
                teacher_voice_ready_ = true;    // 已收齐, 等云端说完再播
                teacher_voice_pending_ = false;
            } else if (teacher_bytes_ > 0) {
                teacher_voice_pending_ = true;  // 正在传, 让 CloudFallbackTick 让位给老师
            } else {
                teacher_voice_pending_ = false; // 畸形/超限分片已整段拒收，不影响云端
            }
            return;
        }
        if (complete) {
            teacher_voice_pending_ = false;
            if (treehole_state_ == kTreeHoleWaiting) {
                /* 语音本身就是老师回复；播完不能继续跑4秒云端兜底。 */
                treehole_state_ = kTreeHoleShowing;
                local_reply_shown_ = false;
                result_level_[0] = 0;
                result_text_.clear();
            }
            PlayTeacherVoice();
            if (treehole_state_ == kTreeHoleShowing && result_level_[0] == 0) {
                Board::GetInstance().GetDisplay()->SetStatus("老师已语音回复");
            }
        } else if (teacher_bytes_ > 0) {
            teacher_voice_pending_ = true;
        }
        return;
    }
    if (buf[0] == '{') {
        /* 老师答得晚(超过有效期)—— 现在也让他显示出来, 不白回一次 */
        ESP_LOGI(TAG, "待命期间收到老师回复, 直接显示");
        HandleReply(buf);
    }
}

/* 待命/显示结果/等待回复时都来看看有没有老师的语音或迟到的回复 */
void Application::PollIdlePackets() {
    if (udp_sock_ < 0) {
        return;
    }
    for (int i = 0; i < 64; i++) {
        /* 用 select(0秒) 判断有没有数据, 避免 recvfrom 的 100ms 超时拖慢主循环 */
        if (!UdpHasData()) {
            return;
        }
        char buf[1600];
        int n = recvfrom(udp_sock_, buf, sizeof(buf) - 1, 0, NULL, NULL);   // 留 1 字节给结尾的 0
        if (n <= 0) {
            return;
        }
        HandleIdlePacket(buf, n);
        if (playing_voice_) {   // 正在播放老师语音, 先不处理更多包
            return;
        }
    }
}

/* ====================================================================
 * 12 颗 RGB 灯的柔和呼吸动画
 * 绿=(0,255,0) 黄=(255,200,0) 红=(255,0,0) 本地陪伴回复=暖橙(255,140,0)
 * 亮度按正弦在 25%~100% 之间变化, 周期约 2.5 秒
 * ==================================================================== */
void Application::UpdateLedBreathing() {
    if (rgb_strip_ == nullptr) {
        return;
    }
    /* 老师在说话: 蓝色呼吸(优先级最高) */
    if (playing_voice_) {
        float phase = (now_ms() % 2500) / 2500.0f;
        float k = 0.25f + 0.75f * (sinf(phase * 2.0f * 3.14159265f) + 1.0f) / 2.0f;
        for (int i = 0; i < RGB_LED_COUNT; i++) {
            led_strip_set_pixel(rgb_strip_, i, 0, (uint32_t)(120 * k), (uint32_t)(255 * k));
        }
        led_strip_refresh(rgb_strip_);
        return;
    }
    /* 只在"已显示结果"或"等老师期间已本地回复"这两种情况下亮灯 */
    bool should_light = (treehole_state_ == kTreeHoleShowing) ||
                        (treehole_state_ == kTreeHoleWaiting && local_reply_shown_);
    if (!should_light || result_level_[0] == 0) {
        return;
    }

    uint8_t r = 0, g = 0, b = 0;
    if (strcmp(result_level_, "green") == 0) {
        r = 0; g = 255; b = 0;
    } else if (strcmp(result_level_, "yellow") == 0) {
        r = 255; g = 200; b = 0;
    } else if (strcmp(result_level_, "red") == 0) {
        r = 255; g = 0; b = 0;
    } else if (strcmp(result_level_, "local") == 0) {
        r = 255; g = 140; b = 0;   // 本地陪伴回复: 暖橙色
    } else if (strcmp(result_level_, "cloud") == 0) {
        r = 0; g = 255; b = 0;     // 小智云端回答: 绿色(正常对话)
    } else {
        StopLeds();
        return;
    }

    float phase = (now_ms() % 2500) / 2500.0f;          // 0~1
    float k = 0.25f + 0.75f * (sinf(phase * 2.0f * 3.14159265f) + 1.0f) / 2.0f;
    for (int i = 0; i < RGB_LED_COUNT; i++) {
        led_strip_set_pixel(rgb_strip_, i,
                            (uint32_t)(r * k), (uint32_t)(g * k), (uint32_t)(b * k));
    }
    led_strip_refresh(rgb_strip_);
}

/* 熄灭全部 RGB 灯 */
void Application::StopLeds() {
    if (rgb_strip_ != nullptr) {
        led_strip_clear(rgb_strip_);
    }
}

/* ====================================================================
 * 主循环: 处理按键事件 → 推进状态机 → WiFi 保活
 * ==================================================================== */
void Application::Schedule(std::function<void()> callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    main_tasks_.push_back(std::move(callback));
    xEventGroupSetBits(event_group_, SCHEDULE_EVENT);
}

void Application::MainLoop() {
    while (true) {
        /* 等待按键事件, 最多等 30ms(保证灯效/超时判断流畅) */
        auto bits = xEventGroupWaitBits(event_group_, SCHEDULE_EVENT,
                                        pdTRUE, pdFALSE, pdMS_TO_TICKS(30));

        /* 先执行按键触发的任务(开始/停止录音等) */
        if (bits & SCHEDULE_EVENT) {
            mutex_.lock();
            std::list<std::function<void()>> tasks = std::move(main_tasks_);
            mutex_.unlock();
            for (auto &task : tasks) {
                task();
            }
        }

        ExpireTeacherVoice();

        /* 每 10 秒给教师端发一次心跳, 教师端据此显示"终端在线/离线" */
        if (now_ms() - last_heartbeat_ms_ >= HEARTBEAT_INTERVAL_MS) {
            last_heartbeat_ms_ = now_ms();
            SendHeartbeat();
        }

        /* 再推进当前状态 */
        switch (treehole_state_) {
        case kTreeHoleRecording:
            RecordTick();
            break;
        case kTreeHoleWaiting:
            WaitReplyTick();
            break;
        default:
            /* 待命/显示结果时也收包: 老师可以随时发语音或迟到的文字回复 */
            PollIdlePackets();
            /* 已经收齐但还没播出去的老师语音, 补播一次(PlayTeacherVoice 播完会清空缓冲,
             * 所以不会重复播)。什么情况会走到这里: 学生录音太短被丢弃、发送时没网… */
            if (!playing_voice_ && teacher_expect_ > 0 && teacher_bytes_ >= teacher_expect_) {
                ESP_LOGI(TAG, "补播老师之前发来的语音");
                PlayTeacherVoice();
            }
            break;
        }

        /* 灯效: 是否需要点亮/呼吸由函数内部按当前状态判断 */
        UpdateLedBreathing();

        /* WiFi 掉线自动重连 */
        EnsureWifi();
    }
}
