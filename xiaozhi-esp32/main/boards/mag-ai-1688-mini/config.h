#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

/* ============================================================
 * 解忧树洞 - MAG AI 1688 Mini 板级配置
 * 主控: 乐鑫 ESP32-S3-WROOM-1U-N16R8 (16MB Flash + 8MB Octal PSRAM)
 * 引脚全部来自立创EDA工程 Mag_Ai_1688_Pro_V1.2 的飞针网络表,
 * 与厂商原版 xiaozhi_mag_ai_0_9_6 固件使用的配置一致。
 * ============================================================ */

/* ---------- 音频参数 ---------- */
#define AUDIO_INPUT_SAMPLE_RATE  16000   // 麦克风录音采样率 16kHz
#define AUDIO_OUTPUT_SAMPLE_RATE 8000    // 功放播报采样率 8kHz(求助提示音)
#define AUDIO_DEFAULT_OUTPUT_VOLUME 70   // 默认音量 0~100

/* 功放 NS4168 (I2S 输入, 数字信号直连, 无编解码芯片) */
#define AUDIO_I2S_SPK_BCLK  GPIO_NUM_15   // BCLK
#define AUDIO_I2S_SPK_WS    GPIO_NUM_16   // L/RCLK (字时钟)
#define AUDIO_I2S_SPK_DOUT  GPIO_NUM_7    // SDATA (数据)

/* 麦克风 MSM261S4030H0R (I2S 输出数字麦克风) */
#define AUDIO_I2S_MIC_SCK   GPIO_NUM_5    // SCK (位时钟)
#define AUDIO_I2S_MIC_WS    GPIO_NUM_4    // WS  (字时钟)
#define AUDIO_I2S_MIC_DIN   GPIO_NUM_6    // SD  (数据)
/* 注: 麦克风 L/R 引脚在电路板上已通过电阻接地 = 左声道 */

/* ---------- 按键 (按下为低电平, 内部上拉) ---------- */
#define BOOT_BUTTON_GPIO        GPIO_NUM_0    // 对话键: 按住说话, 松开发送
#define VOLUME_UP_BUTTON_GPIO   GPIO_NUM_1    // 音量+
#define VOLUME_DOWN_BUTTON_GPIO GPIO_NUM_43   // 音量-

/* ---------- 屏幕: 1.83寸 240x280 NV3030B (Intel 8080 8位并口, 带触摸) ---------- */
#define DISPLAY_WIDTH   240
#define DISPLAY_HEIGHT  280
#define DISPLAY_OFFSET_X  0
#define DISPLAY_OFFSET_Y  0
#define DISPLAY_MIRROR_X  false
#define DISPLAY_MIRROR_Y  false
#define DISPLAY_SWAP_XY   false

#define DISPLAY_BACKLIGHT_PIN           GPIO_NUM_44   // 背光控制(经MOS管点亮LEDK)
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT false         // 高电平亮

/* 8080 并口: DC/写信号/8根数据线 */
#define LCD_I80_DC_GPIO  GPIO_NUM_13
#define LCD_I80_WR_GPIO  GPIO_NUM_8
#define LCD_I80_D0_GPIO  GPIO_NUM_14
#define LCD_I80_D1_GPIO  GPIO_NUM_21
#define LCD_I80_D2_GPIO  GPIO_NUM_47
#define LCD_I80_D3_GPIO  GPIO_NUM_48
#define LCD_I80_D4_GPIO  GPIO_NUM_45
#define LCD_I80_D5_GPIO  GPIO_NUM_38
#define LCD_I80_D6_GPIO  GPIO_NUM_39
#define LCD_I80_D7_GPIO  GPIO_NUM_40
/* 屏幕触摸芯片走 I2C (SCL=IO17, SDA=IO18), 树洞固件未使用触摸 */

/* ---------- 12 颗 WS2812 RGB 灯 ---------- */
#define RGB_LED_GPIO   GPIO_NUM_2
#define RGB_LED_COUNT  12

#endif // _BOARD_CONFIG_H_
