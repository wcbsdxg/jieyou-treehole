#pragma once

/*
 * 解忧树洞 - NV3030B 液晶屏驱动 (Intel 8080 8位并口)
 * 1.83 寸手表屏, 分辨率 240x280, 驱动芯片 NV3030B
 * 参考: ESP-IDF esp_lcd_panel_st7789.c 的结构 + 中景园 NV3030B 初始化序列
 */

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 创建 NV3030B 面板句柄
 * @param io        由 esp_lcd_new_panel_io_i80() 创建的 8080 总线 IO 句柄
 * @param panel_dev_config 面板配置(分辨率无关, 只关心颜色格式)
 * @param ret_panel 输出面板句柄
 */
esp_err_t esp_lcd_new_panel_nv3030b(const esp_lcd_panel_io_handle_t io,
                                    const esp_lcd_panel_dev_config_t *panel_dev_config,
                                    esp_lcd_panel_handle_t *ret_panel);

#ifdef __cplusplus
}
#endif
