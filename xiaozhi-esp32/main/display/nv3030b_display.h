#ifndef NV3030B_DISPLAY_H
#define NV3030B_DISPLAY_H

#include "display.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <driver/gpio.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_timer.h>

/* 解忧树洞显示类: 1.83寸 240x280 NV3030B (8080并口) + LVGL
 * 界面布局:
 *   ┌──────────────┐
 *   │ 状态栏(提示文字) │
 *   ├──────────────┤
 *   │   级别图标     │
 *   │  回复正文(换行) │
 *   │  求助信息(红级) │
 *   └──────────────┘
 */
class Nv3030bDisplay : public Display {
private:
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    gpio_num_t backlight_pin_ = GPIO_NUM_NC;
    bool backlight_output_invert_ = false;
    SemaphoreHandle_t lvgl_mutex_ = nullptr;
    esp_timer_handle_t lvgl_tick_timer_ = nullptr;

    lv_obj_t* status_bar_ = nullptr;
    lv_obj_t* content_ = nullptr;
    lv_obj_t* container_ = nullptr;
    lv_obj_t* chat_label_ = nullptr;   // 服务端回复正文(自动换行)
    lv_obj_t* help_label_ = nullptr;   // 红级求助信息(热线+心理老师)

    void InitializeBacklight(gpio_num_t backlight_pin);
    void SetBacklight(uint8_t brightness);
    void SetupUI();
    void LvglTask();

    virtual bool Lock(int timeout_ms = 0) override;
    virtual void Unlock() override;

public:
    Nv3030bDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                   gpio_num_t backlight_pin, bool backlight_output_invert,
                   int width, int height, int offset_x, int offset_y,
                   bool mirror_x, bool mirror_y, bool swap_xy);
    ~Nv3030bDisplay();

    // 显示服务端返回的结果(级别 + 正文)
    //   level: "green"/"yellow"/"red" = 老师判定; "local" = 终端本地陪伴回复(暖橙)
    virtual void SetResult(const std::string &level, const std::string &text) override;
    // 显示红级求助信息(热线电话 + 心理老师联系方式), 空字符串则隐藏
    virtual void SetHelpText(const std::string &help_text) override;
    // 下面两个供"小智云端AI"模式使用, 让云端对话文字/表情也能显示在本屏
    virtual void SetEmotion(const std::string &emotion) override;
    virtual void SetChatMessage(const std::string &role, const std::string &content) override;
};

#endif // NV3030B_DISPLAY_H
