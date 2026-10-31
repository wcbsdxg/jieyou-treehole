/*
 * 解忧树洞 - NV3030B 液晶屏驱动实现 (Intel 8080 8位并口)
 *
 * 屏幕规格: 1.83 寸 240(H) x 280(V), 16bit RGB565
 * 初始化序列来自中景园 1.83 寸 NV3030B 屏公开例程(总线无关,
 * SPI 与 8080 并口共用同一套寄存器配置), 由商家原版固件验证可用。
 */
#include <stdlib.h>
#include <sys/cdefs.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_commands.h"
#include "esp_lcd_nv3030b.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "lcd_panel.nv3030b";

/* ---- NV3030B 常用寄存器 ---- */
#define NV3030B_CMD_SWRESET  0x01  // 软件复位
#define NV3030B_CMD_SLPOUT   0x11  // 退出睡眠
#define NV3030B_CMD_SLPIN    0x10  // 进入睡眠
#define NV3030B_CMD_INVON    0x21  // 反色开
#define NV3030B_CMD_INVOFF   0x20  // 反色关
#define NV3030B_CMD_DISPON   0x29  // 开显示
#define NV3030B_CMD_DISPOFF  0x28  // 关显示
#define NV3030B_CMD_MADCTL   0x36  // 扫描方向控制
#define NV3030B_CMD_TEON     0x35  // TE 信号配置

/* MADCTL(0x36) 控制位 */
#define NV3030B_MADCTL_MY    0x80  // 行地址方向(上下翻转)
#define NV3030B_MADCTL_MX    0x40  // 列地址方向(左右镜像)
#define NV3030B_MADCTL_MV    0x20  // 行列交换(横竖屏切换)
#define NV3030B_MADCTL_BGR   0x08  // RGB/BGR 顺序

static esp_err_t panel_nv3030b_del(esp_lcd_panel_t *panel);
static esp_err_t panel_nv3030b_reset(esp_lcd_panel_t *panel);
static esp_err_t panel_nv3030b_init(esp_lcd_panel_t *panel);
static esp_err_t panel_nv3030b_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start,
                                           int x_end, int y_end, const void *color_data);
static esp_err_t panel_nv3030b_invert_color(esp_lcd_panel_t *panel, bool invert_color_data);
static esp_err_t panel_nv3030b_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y);
static esp_err_t panel_nv3030b_swap_xy(esp_lcd_panel_t *panel, bool swap_axes);
static esp_err_t panel_nv3030b_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap);
static esp_err_t panel_nv3030b_disp_on_off(esp_lcd_panel_t *panel, bool on_off);
static esp_err_t panel_nv3030b_sleep(esp_lcd_panel_t *panel, bool sleep);

/* 面板对象: 把 esp_lcd_panel_t 函数表放在结构体最前面 */
typedef struct {
    esp_lcd_panel_t base;          // esp_lcd 要求的函数表(必须放第一位)
    esp_lcd_panel_io_handle_t io;  // 8080 总线 IO 句柄
    uint8_t madctl_val;            // 当前 MADCTL 寄存器值
} nv3030b_panel_t;

esp_err_t esp_lcd_new_panel_nv3030b(const esp_lcd_panel_io_handle_t io,
                                    const esp_lcd_panel_dev_config_t *panel_dev_config,
                                    esp_lcd_panel_handle_t *ret_panel)
{
    ESP_RETURN_ON_FALSE(io && panel_dev_config && ret_panel, ESP_ERR_INVALID_ARG, TAG, "invalid argument");

    nv3030b_panel_t *nv3030b = (nv3030b_panel_t *)calloc(1, sizeof(nv3030b_panel_t));
    ESP_RETURN_ON_FALSE(nv3030b, ESP_ERR_NO_MEM, TAG, "no mem for panel");

    nv3030b->io = io;
    /* 默认竖屏(MY=1), 与中景园例程 USE_HORIZONTAL=0 一致 */
    nv3030b->madctl_val = NV3030B_MADCTL_MY;

    /* 填充函数表 */
    nv3030b->base.del = panel_nv3030b_del;
    nv3030b->base.reset = panel_nv3030b_reset;
    nv3030b->base.init = panel_nv3030b_init;
    nv3030b->base.draw_bitmap = panel_nv3030b_draw_bitmap;
    nv3030b->base.invert_color = panel_nv3030b_invert_color;
    nv3030b->base.set_gap = panel_nv3030b_set_gap;
    nv3030b->base.mirror = panel_nv3030b_mirror;
    nv3030b->base.swap_xy = panel_nv3030b_swap_xy;
    nv3030b->base.disp_on_off = panel_nv3030b_disp_on_off;
    nv3030b->base.disp_sleep = panel_nv3030b_sleep;
    *ret_panel = &(nv3030b->base);

    ESP_LOGI(TAG, "new nv3030b panel @%p", nv3030b);
    return ESP_OK;
}

static esp_err_t panel_nv3030b_del(esp_lcd_panel_t *panel)
{
    nv3030b_panel_t *nv3030b = __containerof(panel, nv3030b_panel_t, base);
    free(nv3030b);
    return ESP_OK;
}

/* 复位: 板子上屏的复位脚与主控 EN 绑在一起, 没有独立 GPIO,
 * 所以这里用软件复位命令 0x01 代替硬件复位 */
static esp_err_t panel_nv3030b_reset(esp_lcd_panel_t *panel)
{
    nv3030b_panel_t *nv3030b = __containerof(panel, nv3030b_panel_t, base);
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(nv3030b->io, NV3030B_CMD_SWRESET, NULL, 0),
                        TAG, "io tx param failed");
    vTaskDelay(pdMS_TO_TICKS(150)); // 等复位完成
    return ESP_OK;
}

/* 上电初始化序列(中景园 1.83 寸 NV3030B 公开例程) */
static esp_err_t panel_nv3030b_init(esp_lcd_panel_t *panel)
{
    nv3030b_panel_t *nv3030b = __containerof(panel, nv3030b_panel_t, base);
    esp_lcd_panel_io_handle_t io = nv3030b->io;

    // 1. 使能扩展寄存器(两次 0xFD 切换寄存器页)
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xFD, (uint8_t[]){0x06, 0x08}, 2), TAG, "init 0xFD failed");
    // 2. 电源与驱动配置
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0x61, (uint8_t[]){0x07, 0x04}, 2), TAG, "init 0x61 failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0x62, (uint8_t[]){0x00, 0x44, 0x45}, 3), TAG, "init 0x62 failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0x63, (uint8_t[]){0x41, 0x07, 0x12, 0x12}, 4), TAG, "init 0x63 failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0x64, (uint8_t[]){0x37}, 1), TAG, "init 0x64 failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0x65, (uint8_t[]){0x09, 0x10, 0x21}, 3), TAG, "init 0x65 failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0x66, (uint8_t[]){0x09, 0x10, 0x21}, 3), TAG, "init 0x66 failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0x67, (uint8_t[]){0x20, 0x40}, 2), TAG, "init 0x67 failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0x68, (uint8_t[]){0x90, 0x4C, 0x7C, 0x66}, 4), TAG, "init 0x68 failed");
    // 3. 帧率与行周期
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xB1, (uint8_t[]){0x0F, 0x08, 0x01}, 3), TAG, "init 0xB1 failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xB4, (uint8_t[]){0x01}, 1), TAG, "init 0xB4 failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xB5, (uint8_t[]){0x02, 0x02, 0x0A, 0x14}, 4), TAG, "init 0xB5 failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xB6, (uint8_t[]){0x04, 0x01, 0x9F, 0x00, 0x02}, 5), TAG, "init 0xB6 failed");
    // 4. 伽马曲线
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xDF, (uint8_t[]){0x11}, 1), TAG, "init 0xDF failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xE0, (uint8_t[]){0x01, 0x03, 0x0D, 0x0E, 0x0E, 0x0C, 0x15, 0x19}, 8), TAG, "init 0xE0 failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xE1, (uint8_t[]){0x00, 0x57}, 2), TAG, "init 0xE1 failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xE2, (uint8_t[]){0x13, 0x00, 0x00, 0x30, 0x33, 0x3F}, 6), TAG, "init 0xE2 failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xE3, (uint8_t[]){0x1A, 0x16, 0x0C, 0x0F, 0x0E, 0x0D, 0x02, 0x01}, 8), TAG, "init 0xE3 failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xE4, (uint8_t[]){0x58, 0x00}, 2), TAG, "init 0xE4 failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xE5, (uint8_t[]){0x3F, 0x33, 0x30, 0x00, 0x00, 0x13}, 6), TAG, "init 0xE5 failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xE6, (uint8_t[]){0x00, 0xFF}, 2), TAG, "init 0xE6 failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xE7, (uint8_t[]){0x01, 0x04, 0x03, 0x03, 0x00, 0x12}, 6), TAG, "init 0xE7 failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xE8, (uint8_t[]){0x00, 0x70, 0x00}, 3), TAG, "init 0xE8 failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xEC, (uint8_t[]){0x52}, 1), TAG, "init 0xEC failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xF1, (uint8_t[]){0x01, 0x01, 0x02}, 3), TAG, "init 0xF1 failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xF6, (uint8_t[]){0x09, 0x10, 0x00, 0x00}, 4), TAG, "init 0xF6 failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xFD, (uint8_t[]){0xFA, 0xFC}, 2), TAG, "init 0xFD(2) failed");
    // 5. 像素格式 RGB565 + TE 关闭 + 方向
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0x3A, (uint8_t[]){0x05}, 1), TAG, "init 0x3A failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, NV3030B_CMD_TEON, (uint8_t[]){0x00}, 1), TAG, "init 0x35 failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, NV3030B_CMD_MADCTL, (uint8_t[]){nv3030b->madctl_val}, 1), TAG, "init 0x36 failed");
    // 6. 反色开 + 退出睡眠 + 开显示
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, NV3030B_CMD_INVON, NULL, 0), TAG, "init 0x21 failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, NV3030B_CMD_SLPOUT, NULL, 0), TAG, "init 0x11 failed");
    vTaskDelay(pdMS_TO_TICKS(200));
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, NV3030B_CMD_DISPON, NULL, 0), TAG, "init 0x29 failed");
    vTaskDelay(pdMS_TO_TICKS(10));
    return ESP_OK;
}

/* 刷图: 设置窗口(CASET/RASET)后连续写入像素数据 */
static esp_err_t panel_nv3030b_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start,
                                           int x_end, int y_end, const void *color_data)
{
    nv3030b_panel_t *nv3030b = __containerof(panel, nv3030b_panel_t, base);
    esp_lcd_panel_io_handle_t io = nv3030b->io;

    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_CASET, (uint8_t[]){
        (x_start >> 8) & 0xFF, x_start & 0xFF,
        ((x_end - 1) >> 8) & 0xFF, (x_end - 1) & 0xFF,
    }, 4), TAG, "io tx CASET failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_RASET, (uint8_t[]){
        (y_start >> 8) & 0xFF, y_start & 0xFF,
        ((y_end - 1) >> 8) & 0xFF, (y_end - 1) & 0xFF,
    }, 4), TAG, "io tx RASET failed");

    size_t len = (x_end - x_start) * (y_end - y_start) * 2; // RGB565 每像素2字节
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_color(io, LCD_CMD_RAMWR, color_data, len),
                        TAG, "io tx color failed");
    return ESP_OK;
}

static esp_err_t panel_nv3030b_invert_color(esp_lcd_panel_t *panel, bool invert_color_data)
{
    nv3030b_panel_t *nv3030b = __containerof(panel, nv3030b_panel_t, base);
    return esp_lcd_panel_io_tx_param(nv3030b->io,
                                     invert_color_data ? NV3030B_CMD_INVON : NV3030B_CMD_INVOFF,
                                     NULL, 0);
}

static esp_err_t panel_nv3030b_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y)
{
    nv3030b_panel_t *nv3030b = __containerof(panel, nv3030b_panel_t, base);
    if (mirror_x) nv3030b->madctl_val |= NV3030B_MADCTL_MX;
    else          nv3030b->madctl_val &= ~NV3030B_MADCTL_MX;
    if (mirror_y) nv3030b->madctl_val |= NV3030B_MADCTL_MY;
    else          nv3030b->madctl_val &= ~NV3030B_MADCTL_MY;
    return esp_lcd_panel_io_tx_param(nv3030b->io, NV3030B_CMD_MADCTL,
                                     (uint8_t[]){nv3030b->madctl_val}, 1);
}

static esp_err_t panel_nv3030b_swap_xy(esp_lcd_panel_t *panel, bool swap_axes)
{
    nv3030b_panel_t *nv3030b = __containerof(panel, nv3030b_panel_t, base);
    if (swap_axes) nv3030b->madctl_val |= NV3030B_MADCTL_MV;
    else           nv3030b->madctl_val &= ~NV3030B_MADCTL_MV;
    return esp_lcd_panel_io_tx_param(nv3030b->io, NV3030B_CMD_MADCTL,
                                     (uint8_t[]){nv3030b->madctl_val}, 1);
}

static esp_err_t panel_nv3030b_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap)
{
    return ESP_OK; // 本屏无偏移需求
}

static esp_err_t panel_nv3030b_disp_on_off(esp_lcd_panel_t *panel, bool on_off)
{
    nv3030b_panel_t *nv3030b = __containerof(panel, nv3030b_panel_t, base);
    return esp_lcd_panel_io_tx_param(nv3030b->io,
                                     on_off ? NV3030B_CMD_DISPON : NV3030B_CMD_DISPOFF,
                                     NULL, 0);
}

static esp_err_t panel_nv3030b_sleep(esp_lcd_panel_t *panel, bool sleep)
{
    nv3030b_panel_t *nv3030b = __containerof(panel, nv3030b_panel_t, base);
    esp_err_t err = esp_lcd_panel_io_tx_param(nv3030b->io,
                                              sleep ? NV3030B_CMD_SLPIN : NV3030B_CMD_SLPOUT,
                                              NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    return err;
}
