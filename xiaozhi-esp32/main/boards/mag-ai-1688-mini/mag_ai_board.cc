#include "wifi_board.h"
#include "audio_codecs/no_audio_codec.h"
#include "display/nv3030b_display.h"
#include "esp_lcd_nv3030b.h"
#include "application.h"
#include "button.h"
#include "led.h"
#include "config.h"

#include <esp_log.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>

#define TAG "MagAiBoard"

/* ============================================================
 * 解忧树洞 - MAG AI 1688 Mini 板卡类 (ESP32-S3-N16R8)
 *
 * 硬件组成(引脚定义见同目录 config.h):
 *   - 麦克风 MSM261S4030H0R:  I2S 数字麦克风, 直连 IO4/IO5/IO6
 *   - 功放   NS4168:          I2S 数字输入D类功放, 直连 IO7/IO15/IO16
 *   - 屏幕   NV3030B:         1.83寸 240x280, Intel 8080 8位并口
 *   - 灯条   12颗 WS2812:     IO2 (由 application.cc 驱动)
 *   - 按键   对话=IO0, 音量+=IO1, 音量-=IO43
 * ============================================================ */
class MagAiBoard : public WifiBoard {
private:
    Button boot_button_;       // 对话键
    Button volume_up_button_;  // 音量+
    Button volume_down_button_;// 音量-
    Nv3030bDisplay* display_;

    // 初始化 1.83 寸 NV3030B 屏幕(8080 并口)
    void InitializeNv3030bDisplay() {
        // 第一步: 创建 8080 总线(DC + WR + 8根数据线)
        esp_lcd_i80_bus_handle_t i80_bus = nullptr;
        esp_lcd_i80_bus_config_t bus_config = {
            .dc_gpio_num = LCD_I80_DC_GPIO,
            .wr_gpio_num = LCD_I80_WR_GPIO,
            .clk_src = LCD_CLK_SRC_DEFAULT,
            .data_gpio_nums = {
                LCD_I80_D0_GPIO, LCD_I80_D1_GPIO, LCD_I80_D2_GPIO, LCD_I80_D3_GPIO,
                LCD_I80_D4_GPIO, LCD_I80_D5_GPIO, LCD_I80_D6_GPIO, LCD_I80_D7_GPIO,
            },
            .bus_width = 8,
            .max_transfer_bytes = DISPLAY_WIDTH * 40 * sizeof(uint16_t),
            .dma_burst_size = 64,   // DMA突发64字节(PSRAM数据要求对齐)
        };
        ESP_ERROR_CHECK(esp_lcd_new_i80_bus(&bus_config, &i80_bus));

        // 第二步: 在总线上挂载本屏(无CS脚, 独占总线)
        // 注意: C++20 指定初始化器必须按结构体字段声明顺序书写
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_io_i80_config_t io_config = {
            .cs_gpio_num = GPIO_NUM_NC,
            .pclk_hz = 10 * 1000 * 1000,   // 写时钟 10MHz
            .trans_queue_depth = 10,
            .on_color_trans_done = nullptr,
            .user_ctx = nullptr,
            .lcd_cmd_bits = 8,
            .lcd_param_bits = 8,
            .dc_levels = {
                .dc_idle_level = 0,
                .dc_cmd_level = 0,    // DC=0 写命令
                .dc_dummy_level = 0,
                .dc_data_level = 1,   // DC=1 写数据
            },
            .flags = {
                .swap_color_bytes = !LV_COLOR_16_SWAP, // 字节序交换交给LVGL做
            },
        };
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i80(i80_bus, &io_config, &panel_io));

        // 第三步: 安装 NV3030B 驱动并初始化
        esp_lcd_panel_handle_t panel = nullptr;
        esp_lcd_panel_dev_config_t panel_config = {
            .reset_gpio_num = GPIO_NUM_NC,      // 复位脚与主控EN绑在一起, 用软件复位
            .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
            .bits_per_pixel = 16,               // RGB565
        };
        ESP_ERROR_CHECK(esp_lcd_new_panel_nv3030b(panel_io, &panel_config, &panel));
        esp_lcd_panel_reset(panel);
        esp_lcd_panel_init(panel);

        display_ = new Nv3030bDisplay(panel_io, panel,
                                      DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT,
                                      DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                      DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
                                      DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }

    // 初始化三个按键
    void InitializeButtons() {
        // 对话键: 按下开始录音, 松开停止并发送
        boot_button_.OnPressDown([]() {
            Application::GetInstance().StartListening();
        });
        boot_button_.OnPressUp([]() {
            Application::GetInstance().StopListening();
        });

        // 音量+ / 音量- : 单击调节播报音量
        volume_up_button_.OnClick([]() {
            auto codec = Board::GetInstance().GetAudioCodec();
            int volume = codec->output_volume() + 10;
            if (volume > 100) {
                volume = 100;
            }
            codec->SetOutputVolume(volume);
            Board::GetInstance().GetDisplay()->ShowNotification("音量 " + std::to_string(volume) + "%");
        });
        volume_down_button_.OnClick([]() {
            auto codec = Board::GetInstance().GetAudioCodec();
            int volume = codec->output_volume() - 10;
            if (volume < 0) {
                volume = 0;
            }
            codec->SetOutputVolume(volume);
            Board::GetInstance().GetDisplay()->ShowNotification("音量 " + std::to_string(volume) + "%");
        });
    }

public:
    MagAiBoard() : boot_button_(BOOT_BUTTON_GPIO),
                   volume_up_button_(VOLUME_UP_BUTTON_GPIO),
                   volume_down_button_(VOLUME_DOWN_BUTTON_GPIO) {
        ESP_LOGI(TAG, "Init MagAiBoard (解忧树洞)");
        InitializeNv3030bDisplay();
        InitializeButtons();
    }

    // 单颗指示灯(本板没有独立指示灯, 返回空LED)
    virtual Led* GetBuiltinLed() override {
        static Led led(GPIO_NUM_NC);
        return &led;
    }

    // 音频编解码: 本板无编解码芯片, 麦克风/功放均为I2S直连
    // 输入16kHz(录音), 输出8kHz(求助提示音)
    virtual AudioCodec* GetAudioCodec() override {
        static NoAudioCodec* audio_codec = nullptr;
        if (audio_codec == nullptr) {
            audio_codec = new NoAudioCodec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
                AUDIO_I2S_SPK_BCLK, AUDIO_I2S_SPK_WS, AUDIO_I2S_SPK_DOUT,
                AUDIO_I2S_MIC_SCK, AUDIO_I2S_MIC_WS, AUDIO_I2S_MIC_DIN);
            audio_codec->SetOutputVolume(AUDIO_DEFAULT_OUTPUT_VOLUME);
        }
        return audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }
};

DECLARE_BOARD(MagAiBoard);
