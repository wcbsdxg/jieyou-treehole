#include "nv3030b_display.h"
#include "font_awesome_symbols.h"
#include "esp_lcd_nv3030b.h"

#include <esp_log.h>
#include <esp_err.h>
#include <driver/ledc.h>
#include <vector>

#define TAG "Nv3030bDisplay"
#define LCD_LEDC_CH LEDC_CHANNEL_0

#define NV3030B_LVGL_TICK_PERIOD_MS 2
#define NV3030B_LVGL_TASK_MAX_DELAY_MS 20
#define NV3030B_LVGL_TASK_MIN_DELAY_MS 1
#define NV3030B_LVGL_TASK_STACK_SIZE (4 * 1024)
#define NV3030B_LVGL_TASK_PRIORITY 10

/* 使用内置的阿里巴巴普惠体14号(中文)与FontAwesome图标字体 */
LV_FONT_DECLARE(font_puhui_14_1);
LV_FONT_DECLARE(font_awesome_30_1);
LV_FONT_DECLARE(font_awesome_14_1);

static lv_disp_drv_t disp_drv;

/* esp_lcd 的颜色传输由 DMA 在后台完成。只有回调到达后 LVGL 才能复用双缓冲。 */
static bool nv3030b_color_trans_done(esp_lcd_panel_io_handle_t panel_io,
                                    esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    lv_disp_drv_t *drv = static_cast<lv_disp_drv_t *>(user_ctx);
    lv_disp_flush_ready(drv);
    return false;
}

/* LVGL 刷新回调: 把一块像素交给 esp_lcd 画到屏幕上 */
static void nv3030b_lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t)drv->user_data;
    esp_err_t err = esp_lcd_panel_draw_bitmap(panel_handle, area->x1, area->y1,
                                              area->x2 + 1, area->y2 + 1, color_map);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LCD 刷新提交失败: %s", esp_err_to_name(err));
        lv_disp_flush_ready(drv); // 提交失败不会有 DMA 完成回调，必须在这里解锁 LVGL
    }
}

void Nv3030bDisplay::LvglTask() {
    ESP_LOGI(TAG, "Starting LVGL task");
    uint32_t task_delay_ms = NV3030B_LVGL_TASK_MAX_DELAY_MS;
    while (1) {
        // LVGL 不是线程安全的, 操作前必须加锁
        if (Lock()) {
            task_delay_ms = lv_timer_handler();
            Unlock();
        }
        if (task_delay_ms > NV3030B_LVGL_TASK_MAX_DELAY_MS) {
            task_delay_ms = NV3030B_LVGL_TASK_MAX_DELAY_MS;
        } else if (task_delay_ms < NV3030B_LVGL_TASK_MIN_DELAY_MS) {
            task_delay_ms = NV3030B_LVGL_TASK_MIN_DELAY_MS;
        }
        vTaskDelay(pdMS_TO_TICKS(task_delay_ms));
    }
}

Nv3030bDisplay::Nv3030bDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                               gpio_num_t backlight_pin, bool backlight_output_invert,
                               int width, int height, int offset_x, int offset_y,
                               bool mirror_x, bool mirror_y, bool swap_xy)
    : panel_io_(panel_io), panel_(panel), backlight_pin_(backlight_pin),
      backlight_output_invert_(backlight_output_invert) {
    width_ = width;
    height_ = height;

    InitializeBacklight(backlight_pin);

    // LVGL 首帧准备好之前先关背光，避免看到未初始化显存。
    SetBacklight(0);
    esp_lcd_panel_disp_on_off(panel_, true);

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

    // 为 LVGL 分配两块"画布"缓冲(8080总线要求按DMA突发长度对齐)
    static lv_disp_draw_buf_t disp_buf;
    lv_color_t *buf1 = (lv_color_t *)esp_lcd_i80_alloc_draw_buffer(panel_io_, width_ * 10 * sizeof(lv_color_t), MALLOC_CAP_DMA);
    lv_color_t *buf2 = (lv_color_t *)esp_lcd_i80_alloc_draw_buffer(panel_io_, width_ * 10 * sizeof(lv_color_t), MALLOC_CAP_DMA);
    assert(buf1);
    assert(buf2);
    lv_disp_draw_buf_init(&disp_buf, buf1, buf2, width_ * 10);

    // 注册屏幕驱动(竖屏 240x280, 固定方向)
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = width_;
    disp_drv.ver_res = height_;
    disp_drv.offset_x = offset_x;
    disp_drv.offset_y = offset_y;
    disp_drv.flush_cb = nv3030b_lvgl_flush_cb;
    disp_drv.draw_buf = &disp_buf;
    disp_drv.user_data = panel_;
    lv_disp_drv_register(&disp_drv);

    const esp_lcd_panel_io_callbacks_t io_callbacks = {
        .on_color_trans_done = nv3030b_color_trans_done,
    };
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(panel_io_, &io_callbacks, &disp_drv));

    // LVGL 心跳定时器(每2ms加一次时间)
    const esp_timer_create_args_t lvgl_tick_timer_args = {
        .callback = [](void* arg) {
            lv_tick_inc(NV3030B_LVGL_TICK_PERIOD_MS);
        },
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "LVGL Tick Timer",
        .skip_unhandled_events = false
    };
    ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer_));
    ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer_, NV3030B_LVGL_TICK_PERIOD_MS * 1000));

    lvgl_mutex_ = xSemaphoreCreateRecursiveMutex();
    assert(lvgl_mutex_ != nullptr);
    ESP_LOGI(TAG, "Create LVGL task");
    xTaskCreate([](void *arg) {
        static_cast<Nv3030bDisplay*>(arg)->LvglTask();
        vTaskDelete(NULL);
    }, "LVGL", NV3030B_LVGL_TASK_STACK_SIZE, this, NV3030B_LVGL_TASK_PRIORITY, NULL);

    SetupUI();
    SetBacklight(100);
}

Nv3030bDisplay::~Nv3030bDisplay() {
    ESP_ERROR_CHECK(esp_timer_stop(lvgl_tick_timer_));
    ESP_ERROR_CHECK(esp_timer_delete(lvgl_tick_timer_));

    /* content_/status_bar_ 都是 container_ 的子对象，删父对象会递归释放；
     * 分别删除后再删父对象会二次释放。 */
    if (container_ != nullptr) {
        lv_obj_del(container_);
        container_ = nullptr;
        content_ = nullptr;
        status_bar_ = nullptr;
    }

    if (panel_ != nullptr) {
        esp_lcd_panel_del(panel_);
    }
    if (panel_io_ != nullptr) {
        esp_lcd_panel_io_del(panel_io_);
    }
    vSemaphoreDelete(lvgl_mutex_);
}

void Nv3030bDisplay::InitializeBacklight(gpio_num_t backlight_pin) {
    if (backlight_pin == GPIO_NUM_NC) {
        return;
    }

    // 用 LEDC(硬件PWM)控制背光亮度
    const ledc_channel_config_t backlight_channel = {
        .gpio_num = backlight_pin,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LCD_LEDC_CH,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
        .flags = {
            .output_invert = backlight_output_invert_,
        }
    };
    const ledc_timer_config_t backlight_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
        .deconfigure = false
    };

    ESP_ERROR_CHECK(ledc_timer_config(&backlight_timer));
    ESP_ERROR_CHECK(ledc_channel_config(&backlight_channel));
}

void Nv3030bDisplay::SetBacklight(uint8_t brightness) {
    if (backlight_pin_ == GPIO_NUM_NC) {
        return;
    }
    if (brightness > 100) {
        brightness = 100;
    }
    ESP_LOGI(TAG, "Setting LCD backlight: %d%%", brightness);
    uint32_t duty_cycle = (1023 * brightness) / 100;
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, LCD_LEDC_CH, duty_cycle));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, LCD_LEDC_CH));
}

bool Nv3030bDisplay::Lock(int timeout_ms) {
    const TickType_t timeout_ticks = (timeout_ms == 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTakeRecursive(lvgl_mutex_, timeout_ticks) == pdTRUE;
}

void Nv3030bDisplay::Unlock() {
    xSemaphoreGiveRecursive(lvgl_mutex_);
}

void Nv3030bDisplay::SetupUI() {
    DisplayLockGuard lock(this);

    auto screen = lv_disp_get_scr_act(lv_disp_get_default());
    lv_obj_set_style_bg_color(screen, lv_color_white(), 0);
    lv_obj_set_style_text_font(screen, &font_puhui_14_1, 0);
    lv_obj_set_style_text_color(screen, lv_color_black(), 0);

    // 总容器: 纵向布局, 状态栏 + 内容区
    container_ = lv_obj_create(screen);
    lv_obj_set_size(container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_flex_flow(container_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_pad_row(container_, 0, 0);

    // 状态栏(顶部18像素): 中间显示提示文字
    status_bar_ = lv_obj_create(container_);
    lv_obj_set_size(status_bar_, LV_HOR_RES, 18);
    lv_obj_set_style_radius(status_bar_, 0, 0);
    lv_obj_set_style_bg_color(status_bar_, lv_color_hex(0x2E7D32), 0); // 树洞绿
    lv_obj_set_style_pad_all(status_bar_, 0, 0);
    lv_obj_set_style_border_width(status_bar_, 0, 0);

    notification_label_ = lv_label_create(status_bar_);
    lv_obj_set_flex_grow(notification_label_, 1);
    lv_obj_set_style_text_align(notification_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(notification_label_, lv_color_white(), 0);
    lv_obj_set_style_text_font(notification_label_, &font_puhui_14_1, 0);
    lv_label_set_text(notification_label_, "解忧树洞");
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    status_label_ = lv_label_create(status_bar_);
    lv_obj_set_flex_grow(status_label_, 1);
    lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(status_label_, lv_color_white(), 0);
    lv_obj_set_style_text_font(status_label_, &font_puhui_14_1, 0);
    lv_label_set_text(status_label_, "正在初始化");

    // 内容区: 纵向布局, 图标 + 正文 + 求助信息
    content_ = lv_obj_create(container_);
    lv_obj_set_scrollbar_mode(content_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_radius(content_, 0, 0);
    lv_obj_set_style_border_width(content_, 0, 0);
    lv_obj_set_width(content_, LV_HOR_RES);
    lv_obj_set_flex_grow(content_, 1);
    lv_obj_set_flex_flow(content_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(content_, 4, 0);
    lv_obj_set_style_pad_row(content_, 4, 0);

    // 级别图标(绿色√ / 黄色! / 红色铃铛)
    emotion_label_ = lv_label_create(content_);
    lv_obj_set_style_text_font(emotion_label_, &font_awesome_30_1, 0);
    lv_label_set_text(emotion_label_, FONT_AWESOME_AI_CHIP);
    lv_obj_set_style_text_color(emotion_label_, lv_color_hex(0x2E7D32), 0);
    lv_obj_set_width(emotion_label_, LV_HOR_RES - 8);
    lv_obj_set_style_text_align(emotion_label_, LV_TEXT_ALIGN_CENTER, 0);

    // 回复正文: 自动换行
    chat_label_ = lv_label_create(content_);
    lv_obj_set_width(chat_label_, LV_HOR_RES - 8);
    lv_obj_set_style_text_font(chat_label_, &font_puhui_14_1, 0);
    lv_obj_set_style_text_color(chat_label_, lv_color_black(), 0);
    lv_label_set_long_mode(chat_label_, LV_LABEL_LONG_WRAP);   // 超宽自动换行
    lv_label_set_text(chat_label_, "按住对话键开始说话\n松开自动发送\n录音仅存内存, 不保存");

    // 求助信息(仅红级显示)
    help_label_ = lv_label_create(content_);
    lv_obj_set_width(help_label_, LV_HOR_RES - 8);
    lv_obj_set_style_text_font(help_label_, &font_puhui_14_1, 0);
    lv_obj_set_style_text_color(help_label_, lv_color_hex(0xC62828), 0);
    lv_label_set_long_mode(help_label_, LV_LABEL_LONG_WRAP);
    lv_label_set_text(help_label_, "");
    lv_obj_add_flag(help_label_, LV_OBJ_FLAG_HIDDEN);
}

/* 显示服务端返回的结果 */
void Nv3030bDisplay::SetResult(const std::string &level, const std::string &text) {
    DisplayLockGuard lock(this);

    const char *icon = "";
    uint32_t color = 0x2E7D32; // 默认树洞绿
    if (level == "green") {
        icon = FONT_AWESOME_CHECK;      // √
        color = 0x2E7D32;               // 绿
    } else if (level == "yellow") {
        icon = FONT_AWESOME_WARNING;    // !
        color = 0xF9A825;               // 黄
    } else if (level == "red") {
        icon = FONT_AWESOME_BELL;       // 铃铛(求助)
        color = 0xC62828;               // 红
    } else if (level == "local") {
        icon = FONT_AWESOME_EMOJI_LOVING; // 爱心笑脸(终端陪伴回复)
        color = 0xF57C00;                 // 暖橙
    } else if (level == "cloud") {
        icon = FONT_AWESOME_AI_CHIP;      // 小智云端回答
        color = 0x2E7D32;                 // 绿
    } else if (level == "voice") {
        icon = FONT_AWESOME_VOLUME_HIGH;  // 老师发来的语音
        color = 0x1565C0;                 // 蓝
    }

    if (emotion_label_ != nullptr) {
        lv_label_set_text(emotion_label_, icon);
        lv_obj_set_style_text_color(emotion_label_, lv_color_hex(color), 0);
    }
    if (chat_label_ != nullptr) {
        lv_label_set_text(chat_label_, text.empty() ? "..." : text.c_str());
    }
}

/* 显示/隐藏红级求助信息(热线 + 心理老师) */
void Nv3030bDisplay::SetHelpText(const std::string &help_text) {
    DisplayLockGuard lock(this);

    if (help_label_ == nullptr) {
        return;
    }
    if (help_text.empty()) {
        lv_obj_add_flag(help_label_, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(help_label_, "");
    } else {
        lv_label_set_text(help_label_, help_text.c_str());
        lv_obj_clear_flag(help_label_, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ============ 以下两个接口供"小智云端AI"模式使用 ============ */

/* 云端模式: 把服务端下发的情绪名换成图标显示 */
void Nv3030bDisplay::SetEmotion(const std::string &emotion) {
    DisplayLockGuard lock(this);
    if (emotion_label_ == nullptr) {
        return;
    }

    const char *icon = FONT_AWESOME_EMOJI_NEUTRAL;
    if (emotion == "happy") {
        icon = FONT_AWESOME_EMOJI_HAPPY;
    } else if (emotion == "laughing") {
        icon = FONT_AWESOME_EMOJI_LAUGHING;
    } else if (emotion == "sad") {
        icon = FONT_AWESOME_EMOJI_SAD;
    } else if (emotion == "angry") {
        icon = FONT_AWESOME_EMOJI_ANGRY;
    } else if (emotion == "crying") {
        icon = FONT_AWESOME_EMOJI_CRYING;
    } else if (emotion == "loving") {
        icon = FONT_AWESOME_EMOJI_LOVING;
    } else if (emotion == "surprised") {
        icon = FONT_AWESOME_EMOJI_SURPRISED;
    } else if (emotion == "thinking") {
        icon = FONT_AWESOME_EMOJI_THINKING;
    } else if (emotion == "cool") {
        icon = FONT_AWESOME_EMOJI_COOL;
    } else if (emotion == "confused") {
        icon = FONT_AWESOME_EMOJI_CONFUSED;
    }

    lv_label_set_text(emotion_label_, icon);
    lv_obj_set_style_text_color(emotion_label_, lv_color_hex(0x2E7D32), 0);
}

/* 云端模式: 显示对话内容(role 为 "user"/"assistant", 这里统一当正文显示) */
void Nv3030bDisplay::SetChatMessage(const std::string &role, const std::string &content) {
    DisplayLockGuard lock(this);
    if (chat_label_ == nullptr) {
        return;
    }
    lv_label_set_text(chat_label_, content.c_str());
}
