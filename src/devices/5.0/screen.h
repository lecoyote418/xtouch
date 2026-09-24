#ifndef _XLCD_SCREEN
#define _XLCD_SCREEN

#include <Wire.h>

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_heap_caps.h"

#define screenWidth 800
#define screenHeight 480

// ---------------------------------------------------------------------------
// RGB panel, driven directly via ESP-IDF's esp_lcd RGB driver.
//
// This replaces LovyanGFX's Bus_RGB, which deliberately does NOT use the
// esp_lcd RGB driver: its esp_lcd_new_rgb_panel() call is commented out, and it
// instead creates a dummy i80 bus purely to get the LCD_CAM peripheral brought
// up, then manages its own single framebuffer and GDMA descriptors. Two
// consequences matter:
//   - it cannot use CONFIG_LCD_RGB_RESTART_IN_VSYNC, because that recovery
//     logic lives inside the driver it bypasses, so a bandwidth-induced DMA
//     desync there became a *permanent* horizontal shift;
//   - it is single-buffered, so any update during scan-out can tear.
//
// The shift itself is a DMA/PSRAM bandwidth problem: when the CPU and other
// peripherals (Wi-Fi, MQTT, SD, flash writes) compete with the LCD's EDMA for
// PSRAM, the DMA can fall behind the panel, the LCD peripheral emits dummy
// bytes, and it desynchronises from its framebuffer. What is used here instead:
//
//   1. two framebuffers in PSRAM (num_fbs = 2 below) - the EDMA reads a
//      complete frame straight from PSRAM with no CPU involvement, and LVGL
//      draws into the buffer not currently being scanned, so updates swap
//      atomically. (Bounce-buffer mode is the other IDF option, but it needs
//      the CPU to refill SRAM buffers on every DMA EOF interrupt, which the
//      bit-banged chamber-sensor read starves.)
//   2. CONFIG_LCD_RGB_RESTART_IN_VSYNC - reset the GDMA channel every VBlank
//      so a desync can never become permanent. Safe here because the parity
//      bug that makes this option dangerous applies only to bounce-buffer mode
//      (espressif/esp-idf#19070).
//   3. CONFIG_SPIRAM_XIP_FROM_PSRAM - lets the CPU keep running from PSRAM
//      while the main flash is busy.
//
// 2 and 3 are sdkconfig options set through platformio.ini / sdkconfig.xtouch50,
// and neither exists in ESP-IDF 4.4 (Arduino core 2.x) - which is the actual
// reason this board needs the pioarduino / Arduino 3.x platform, rather than
// any defect in LovyanGFX itself.
//
// Timings/pin mapping follow the vendor board definition:
// https://github.com/esp-arduino-libs/ESP32_Display_Panel/blob/master/src/board/supported/jingcai/BOARD_JINGCAI_JC8048W550C.h
// ---------------------------------------------------------------------------

esp_lcd_panel_handle_t xtouch_panel_handle = NULL;

// ---- Backlight ----
#define XTOUCH_BACKLIGHT_PIN 2
// Vendor values for this panel (BOARD_JINGCAI_JC8048W550C.h):
//   ESP_PANEL_BOARD_BACKLIGHT_PWM_FREQ_HZ        1000
//   ESP_PANEL_BOARD_BACKLIGHT_PWM_DUTY_RESOLUTION 10
// These matter: the harness drove the backlight with exactly these values and it
// survived Wi-Fi association and TLS traffic in every run, whereas the app's
// earlier LEDC setup (5000 Hz, 8-bit) stopped driving the backlight once the
// radio started.
#define XTOUCH_BACKLIGHT_FREQ 1000
#define XTOUCH_BACKLIGHT_RES_BITS 10

// 0 = LEDC PWM (dimming works), 1 = plain static GPIO (on/off only).
//
// Set to 1 only as a fallback if the backlight ever stops responding again:
// with a plain output the pin cannot change on its own, which distinguishes a
// dead PWM output from the backlight hardware losing power.
#define XTOUCH_BACKLIGHT_GPIO_BYPASS 0

// Last requested backlight level.
static volatile uint8_t xtouch_screen_brightness = 0;

// ---- Touch (GT911, I2C bus 1) ----
#define XTOUCH_TOUCH_SDA 19
#define XTOUCH_TOUCH_SCL 20
#define XTOUCH_TOUCH_RST 38
#define XTOUCH_TOUCH_FREQ 400000
uint8_t xtouch_gt911_addr = 0x5D; // probed at setup; GT911's other common address is 0x14

#if defined(__XTOUCH_SCREEN_50__)
#include "ui/5.0/ui.h"
#elif defined(__XTOUCH_SCREEN_28__)
#include "ui/2.8/ui.h"
#endif
bool xtouch_screen_getTouch(int16_t *outX, int16_t *outY);
void xtouch_screen_drawFastHLine(int16_t x, int16_t y, int16_t len, uint32_t color);
void xtouch_screen_drawFastVLine(int16_t x, int16_t y, int16_t len, uint32_t color);
#include "touch.h"
#include "xtouch/globals.h"

bool xtouch_screen_touchFromPowerOff = false;

// ---------------------------------------------------------------------------
// Panel init
// ---------------------------------------------------------------------------
void xtouch_screen_initPanel()
{
    esp_lcd_rgb_panel_config_t panel_config = {};
    panel_config.clk_src = LCD_CLK_SRC_PLL160M;

    // Timings from the vendor board definition for this exact panel. The
    // previous values (HBP 16 / VBP 4 / VFP 4) were a mis-translation of the
    // LovyanGFX config and would shift the image on their own.
    panel_config.timings.pclk_hz = 16 * 1000 * 1000;
    panel_config.timings.h_res = screenWidth;
    panel_config.timings.v_res = screenHeight;
    panel_config.timings.hsync_pulse_width = 4;
    panel_config.timings.hsync_back_porch = 8;
    panel_config.timings.hsync_front_porch = 8;
    panel_config.timings.vsync_pulse_width = 4;
    panel_config.timings.vsync_back_porch = 8;
    panel_config.timings.vsync_front_porch = 8;
    panel_config.timings.flags.pclk_active_neg = true; // data clocked out on falling edge
    panel_config.timings.flags.hsync_idle_low = false;
    panel_config.timings.flags.vsync_idle_low = false;
    panel_config.timings.flags.de_idle_high = false;
    panel_config.timings.flags.pclk_idle_high = false;

    panel_config.data_width = 16;
    panel_config.bits_per_pixel = 16;

    // Double framebuffer (2 x 800x480x2 B = 1.5 MB of PSRAM): the EDMA reads a
    // complete frame straight from PSRAM with no per-line CPU work, and LVGL
    // writes into whichever buffer is not currently being scanned.
    //
    // Verified in the minimal test (src/lcdtest, -DLCDTEST_LVGL_TORTURE): a
    // full-screen repaint on every loop iteration - far heavier than the
    // application's status-push repaints - runs clean with this configuration,
    // provided CONFIG_LCD_RGB_RESTART_IN_VSYNC is OFF. That option was the
    // cause of the "sideways shift plus black flash": it resets and restarts the
    // GDMA channel on every VBlank, which is invisible on a static screen but
    // tears visibly whenever the framebuffer is being updated mid-frame - hence
    // glitches that appeared exactly when the UI repainted.
    //
    // Known cost of this mode (measured in the IDF driver source,
    // components/esp_lcd/rgb/esp_lcd_panel_rgb.c, rgb_panel_draw_bitmap): when
    // the framebuffer is in PSRAM and there is no bounce buffer, every
    // draw_bitmap call runs
    //     esp_cache_msync(fb, v_res * bytes_per_line, ...)
    // i.e. it cache-syncs the whole 768 KB framebuffer regardless of how small
    // the flushed rectangle is. That is wasteful, but the torture test shows it
    // is affordable. Bounce-buffer mode (num_fbs = 1 plus
    // bounce_buffer_size_px = screenWidth * 10) would skip that sync entirely -
    // worth revisiting if the pixel clock ever needs raising.
    panel_config.num_fbs = 2;
    panel_config.bounce_buffer_size_px = 0;
    panel_config.dma_burst_size = 64;

    panel_config.hsync_gpio_num = GPIO_NUM_39;
    panel_config.vsync_gpio_num = GPIO_NUM_41;
    panel_config.de_gpio_num = GPIO_NUM_40;
    panel_config.pclk_gpio_num = GPIO_NUM_42;
    panel_config.disp_gpio_num = GPIO_NUM_NC;

    // Same bit-position convention as LovyanGFX's pin_d0..pin_d15: bits 0-4
    // = Blue[4:0], bits 5-10 = Green[5:0], bits 11-15 = Red[4:0]. These pin
    // numbers carry over directly from the old screen.h.
    panel_config.data_gpio_nums[0] = GPIO_NUM_8;   // B0
    panel_config.data_gpio_nums[1] = GPIO_NUM_3;   // B1
    panel_config.data_gpio_nums[2] = GPIO_NUM_46;  // B2
    panel_config.data_gpio_nums[3] = GPIO_NUM_9;   // B3
    panel_config.data_gpio_nums[4] = GPIO_NUM_1;   // B4
    panel_config.data_gpio_nums[5] = GPIO_NUM_5;   // G0
    panel_config.data_gpio_nums[6] = GPIO_NUM_6;   // G1
    panel_config.data_gpio_nums[7] = GPIO_NUM_7;   // G2
    panel_config.data_gpio_nums[8] = GPIO_NUM_15;  // G3
    panel_config.data_gpio_nums[9] = GPIO_NUM_16;  // G4
    panel_config.data_gpio_nums[10] = GPIO_NUM_4;  // G5
    panel_config.data_gpio_nums[11] = GPIO_NUM_45; // R0
    panel_config.data_gpio_nums[12] = GPIO_NUM_48; // R1
    panel_config.data_gpio_nums[13] = GPIO_NUM_47; // R2
    panel_config.data_gpio_nums[14] = GPIO_NUM_21; // R3
    panel_config.data_gpio_nums[15] = GPIO_NUM_14; // R4

    panel_config.flags.fb_in_psram = true;

    ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&panel_config, &xtouch_panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(xtouch_panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(xtouch_panel_handle));
}

// ---------------------------------------------------------------------------
// Re-sync the RGB DMA with the panel.
//
// The DMA can lose its place whenever the CPU/cache is held off long enough
// for the LCD to underrun - Wi-Fi PHY init (esp_wifi_start) is the classic
// trigger, as are flash writes. esp_lcd_rgb_panel_restart() sets a flag that
// makes the driver restart the DMA from the start of the frame at the next
// VSYNC, which also re-arms the bounce-buffer parity counter. It is cheap and
// safe to call at any time; it is the safer replacement for
// CONFIG_LCD_RGB_RESTART_IN_VSYNC, which is broken in bounce-buffer mode (see
// platformio.ini and espressif/esp-idf#19070).
// ---------------------------------------------------------------------------
void xtouch_screen_restartPanel()
{
    if (xtouch_panel_handle != NULL)
    {
        esp_lcd_rgb_panel_restart(xtouch_panel_handle);
    }
}

// ---------------------------------------------------------------------------
// Backlight output. LEDC PWM (dimming) unless XTOUCH_BACKLIGHT_GPIO_BYPASS is 1,
// in which case the pin is a plain static output (on/off only).
//
// The level is always 0..255, the range the settings and the UI slider use,
// while the LEDC channel runs at XTOUCH_BACKLIGHT_RES_BITS (10 for this panel),
// so scale.
// ---------------------------------------------------------------------------
void xtouch_screen_backlightWrite(byte brightness)
{
#if XTOUCH_BACKLIGHT_GPIO_BYPASS
    digitalWrite(XTOUCH_BACKLIGHT_PIN, brightness > 0 ? HIGH : LOW);
#else
    const uint32_t maxDuty = (1u << XTOUCH_BACKLIGHT_RES_BITS) - 1u;
    ledcWrite(XTOUCH_BACKLIGHT_PIN, ((uint32_t)brightness * maxDuty) / 255u);
#endif
}

// ---------------------------------------------------------------------------
// Temporary diagnostics for the "screen goes black under Wi-Fi load" hunt.
//
// xtouch_screen_printBuildConfig() reports which display options the flashed
// binary was actually compiled with, so a stale flash is immediately obvious.
//
// xtouch_screen_diagTick() runs once a second from loop() and reports whether
// LVGL is still flushing (i.e. whether the app/UI is alive) and re-syncs the
// RGB DMA. That self-heal matters because CONFIG_LCD_RGB_RESTART_IN_VSYNC is
// deliberately off here: without it nothing else brings a desynced/stalled
// DMA back, so a single lost VSYNC would otherwise mean a permanently black
// screen. Remove both once the display is stable.
// ---------------------------------------------------------------------------
void xtouch_screen_printBuildConfig()
{
    ConsoleInfo.printf(
        "[XTouch][SCREEN] build cfg: restart_in_vsync=%d cache_line=%d xip_from_psram=%d wifi_lwip_in_psram=%d\n",
#if defined(CONFIG_LCD_RGB_RESTART_IN_VSYNC)
        1,
#else
        0,
#endif
        (int)CONFIG_ESP32S3_DATA_CACHE_LINE_SIZE,
#if defined(CONFIG_SPIRAM_XIP_FROM_PSRAM)
        1,
#else
        0,
#endif
#if defined(CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP)
        1
#else
        0
#endif
    );
}

// ---------------------------------------------------------------------------
// Backlight
// ---------------------------------------------------------------------------
void xtouch_screen_setBrightness(byte brightness)
{
    xtouch_screen_brightness = brightness;
    // Logged so a spurious backlight-off is distinguishable from the RGB panel
    // losing its picture (both look like "the screen went black").
    ConsoleInfo.printf("[XTouch][SCREEN] brightness=%u\n", (unsigned)brightness);
    xtouch_screen_backlightWrite(brightness);
}

void xtouch_screen_setBackLedOff()
{
    xtouch_screen_setBrightness(0);
}

// ---------------------------------------------------------------------------
// Direct framebuffer line drawing (used only by the touch calibration
// routine in touch.h, which draws crosshairs outside of LVGL).
// ---------------------------------------------------------------------------
uint16_t xtouch_screen_rgb888to565(uint32_t color)
{
    uint8_t r = (color >> 16) & 0xFF;
    uint8_t g = (color >> 8) & 0xFF;
    uint8_t b = color & 0xFF;
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

void xtouch_screen_drawFastHLine(int16_t x, int16_t y, int16_t len, uint32_t color)
{
    static uint16_t lineBuf[screenWidth];
    uint16_t c565 = xtouch_screen_rgb888to565(color);
    for (int16_t i = 0; i < len; i++)
        lineBuf[i] = c565;
    esp_lcd_panel_draw_bitmap(xtouch_panel_handle, x, y, x + len, y + 1, lineBuf);
}

void xtouch_screen_drawFastVLine(int16_t x, int16_t y, int16_t len, uint32_t color)
{
    static uint16_t colBuf[screenHeight];
    uint16_t c565 = xtouch_screen_rgb888to565(color);
    for (int16_t i = 0; i < len; i++)
        colBuf[i] = c565;
    esp_lcd_panel_draw_bitmap(xtouch_panel_handle, x, y, x + 1, y + len, colBuf);
}

// ---------------------------------------------------------------------------
// Touch (GT911, minimal single-point I2C reader)
// ---------------------------------------------------------------------------
bool xtouch_screen_gt911Probe(uint8_t addr)
{
    Wire1.beginTransmission(addr);
    return Wire1.endTransmission() == 0;
}

void xtouch_screen_touchInit()
{
    pinMode(XTOUCH_TOUCH_RST, OUTPUT);
    digitalWrite(XTOUCH_TOUCH_RST, LOW);
    delay(10);
    digitalWrite(XTOUCH_TOUCH_RST, HIGH);
    delay(50);

    Wire1.begin(XTOUCH_TOUCH_SDA, XTOUCH_TOUCH_SCL, XTOUCH_TOUCH_FREQ);

    // GT911 ships at one of two I2C addresses depending on board wiring.
    // Probe 0x5D first (the more common default), fall back to 0x14.
    if (xtouch_screen_gt911Probe(0x5D))
    {
        xtouch_gt911_addr = 0x5D;
    }
    else if (xtouch_screen_gt911Probe(0x14))
    {
        xtouch_gt911_addr = 0x14;
    }
    else
    {
        ConsoleError.println(F("[XTouch][TOUCH] GT911 not found at 0x5D or 0x14"));
    }
}

bool xtouch_screen_getTouch(int16_t *outX, int16_t *outY)
{
    // Read the status register (0x814E): bit7 = data-ready flag,
    // low nibble = number of touch points currently reported.
    uint8_t reg[2] = {0x81, 0x4E};
    Wire1.beginTransmission(xtouch_gt911_addr);
    Wire1.write(reg, 2);
    if (Wire1.endTransmission(false) != 0)
        return false;

    if (Wire1.requestFrom((int)xtouch_gt911_addr, 1) != 1)
        return false;
    uint8_t status = Wire1.read();

    bool dataReady = status & 0x80;
    uint8_t pointCount = status & 0x0F;

    if (!dataReady || pointCount == 0)
    {
        // Clear the status flag even on a "no touch" read so the sensor
        // is ready to report the next point.
        uint8_t clearReg[3] = {0x81, 0x4E, 0x00};
        Wire1.beginTransmission(xtouch_gt911_addr);
        Wire1.write(clearReg, 3);
        Wire1.endTransmission();
        return false;
    }

    // Point data starts at 0x814F, NOT 0x8150:
    //   0x814F id, 0x8150 x_low, 0x8151 x_high,
    //   0x8152 y_low, 0x8153 y_high, 0x8154.. size
    // Getting this off by one shifts every field: the byte then read as
    // "x_high" is really y_low, which produces coordinates that look like
    // byte-swapped nonsense. This is the layout LovyanGFX used on this panel
    // (it reads the status byte at 0x814E and then keeps reading sequentially).
    uint8_t pointReg[2] = {0x81, 0x4F};
    Wire1.beginTransmission(xtouch_gt911_addr);
    Wire1.write(pointReg, 2);
    Wire1.endTransmission(false);

    if (Wire1.requestFrom((int)xtouch_gt911_addr, 5) != 5)
        return false;

    Wire1.read(); // track id, unused (single-touch only)

    // Read each byte into its own variable. Writing this as
    //   x = Wire1.read() | (Wire1.read() << 8);
    // is also a bug: the evaluation order of the two calls is unspecified in
    // C++, so the two bytes can be assembled in the wrong order.
    uint8_t xLow = Wire1.read();
    uint8_t xHigh = Wire1.read();
    uint8_t yLow = Wire1.read();
    uint8_t yHigh = Wire1.read();
    uint16_t x = (uint16_t)xLow | ((uint16_t)xHigh << 8);
    uint16_t y = (uint16_t)yLow | ((uint16_t)yHigh << 8);

    // Clear the status flag so the next read reflects fresh data.
    uint8_t clearReg[3] = {0x81, 0x4E, 0x00};
    Wire1.beginTransmission(xtouch_gt911_addr);
    Wire1.write(clearReg, 3);
    Wire1.endTransmission();

    *outX = x;
    *outY = y;
    return true;
}

void xtouch_screen_wakeUp()
{
    lv_timer_reset(xtouch_screen_onScreenOffTimer);
    xtouch_screen_touchFromPowerOff = false;
    loadScreen(0);
    xtouch_screen_setBrightness(xTouchConfig.xTouchBacklightLevel);
}

void xtouch_screen_onScreenOff(lv_timer_t *timer)
{
    if (bambuStatus.print_status == XTOUCH_PRINT_STATUS_RUNNING)
    {
        return;
    }

    if (xTouchConfig.xTouchTFTOFFValue < XTOUCH_LCD_MIN_SLEEP_TIME)
    {
        return;
    }

    ConsoleInfo.println("[XTouch][SCREEN] Screen Off");
    xtouch_screen_setBrightness(0);
    xtouch_screen_touchFromPowerOff = true;
}

void xtouch_screen_setupScreenTimer()
{
    xtouch_screen_onScreenOffTimer = lv_timer_create(xtouch_screen_onScreenOff, xTouchConfig.xTouchTFTOFFValue * 1000 * 60, NULL);
    lv_timer_pause(xtouch_screen_onScreenOffTimer);
}

void xtouch_screen_startScreenTimer()
{
    lv_timer_resume(xtouch_screen_onScreenOffTimer);
}

void xtouch_screen_setScreenTimer(uint32_t period)
{
    lv_timer_set_period(xtouch_screen_onScreenOffTimer, period);
}

byte xtouch_screen_getTFTFlip()
{
    byte val = xtouch_eeprom_read(XTOUCH_EEPROM_POS_TFTFLIP);
    xTouchConfig.xTouchTFTFlip = val;
    return val;
}

void xtouch_screen_setTFTFlip(byte mode)
{
    xTouchConfig.xTouchTFTFlip = mode;
    xtouch_eeprom_write(XTOUCH_EEPROM_POS_TFTFLIP, mode);
}

void xtouch_screen_toggleTFTFlip()
{
    xtouch_screen_setTFTFlip(!xtouch_screen_getTFTFlip());
    xtouch_resetTouchConfig();
}

void xtouch_screen_setupTFTFlip()
{
    byte eepromTFTFlip = xtouch_screen_getTFTFlip();
    // Rotation isn't implemented in this driver yet (it wasn't functional
    // in the previous LovyanGFX-based screen.h either -- both the rotation
    // call and its touch-side equivalent were already commented out).
}

// ---------------------------------------------------------------------------
// LVGL glue
// ---------------------------------------------------------------------------
void xtouch_screen_dispFlush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p)
{
    esp_lcd_panel_draw_bitmap(xtouch_panel_handle, area->x1, area->y1, area->x2 + 1, area->y2 + 1, color_p);
    lv_disp_flush_ready(disp);
}

void xtouch_screen_touchRead(lv_indev_drv_t *indev_driver, lv_indev_data_t *data)
{
    int16_t touchX, touchY;
    bool touched = xtouch_screen_getTouch(&touchX, &touchY);
    if (touched)
    {
        lv_timer_reset(xtouch_screen_onScreenOffTimer);
        // dont pass first touch after power on
        if (xtouch_screen_touchFromPowerOff)
        {
            xtouch_screen_wakeUp();
            while (xtouch_screen_getTouch(&touchX, &touchY))
                ;
            return;
        }

        // The GT911 is factory-configured for this panel's 800x480 resolution,
        // so its coordinates are already screen pixels and are passed straight
        // through - no calibration mapping (see xtouch_touch_setup()).
        data->state = LV_INDEV_STATE_PR;
        data->point.x = touchX;
        data->point.y = touchY;

#if DEBUG_TOUCH != 0
        Serial.printf("Data touch %d,%d\n", touchX, touchY);
#endif
    }
    else
    {
        data->state = LV_INDEV_STATE_REL;
    }
}

static lv_disp_draw_buf_t draw_buf;
// Small partial render buffer for LVGL, kept in fast internal SRAM (not
// PSRAM) via heap_caps_malloc with MALLOC_CAP_DMA -- this is deliberately
// separate from the panel's own framebuffer (which lives in PSRAM, managed
// entirely by the esp_lcd driver above). LVGL only ever renders into this
// small buffer; xtouch_screen_dispFlush() copies each flushed rect into the
// driver's framebuffer, which the bounce-buffer ISR streams to the panel.
static lv_color_t *buf1;

void xtouch_screen_setup()
{
    ConsoleInfo.println("[XTouch][SCREEN] Setup");
    xtouch_screen_printBuildConfig();

#if XTOUCH_BACKLIGHT_GPIO_BYPASS
    // Fallback only: a plain output cannot dim, but it cannot be knocked out by
    // whatever affected the PWM either.
    pinMode(XTOUCH_BACKLIGHT_PIN, OUTPUT);
#else
    // Arduino core 3.x merged ledcSetup()/ledcAttachPin() into ledcAttach(),
    // and ledcWrite() now takes the pin instead of a channel. Note that
    // LovyanGFX's Light_PWM uses exactly this same ledcAttach() path on Arduino
    // core 3.x - the old channel-based ledcSetup()/ledcAttachPin() calls only
    // exist on core 1.x/2.x.
    ledcAttach(XTOUCH_BACKLIGHT_PIN, XTOUCH_BACKLIGHT_FREQ, XTOUCH_BACKLIGHT_RES_BITS);
#endif
    xtouch_screen_setBackLedOff();

    xtouch_screen_initPanel();
    xtouch_screen_touchInit();

    xtouch_screen_setupTFTFlip();

    xtouch_screen_setBrightness(255);

    lv_init();

    // Small partial draw buffer, in internal SRAM on purpose.
    //
    // The LCD's EDMA reads the framebuffer out of PSRAM continuously (~32 MB/s
    // at this pixel clock), so *any* extra PSRAM traffic competes with it and
    // can starve the DMA - which shows up as the picture shifting sideways with
    // a black flash, recovered on the next VBlank by RESTART_IN_VSYNC. Keeping
    // the buffer LVGL renders into in internal SRAM means a flush only reads
    // 16 KB of SRAM and writes a small rectangle to PSRAM, instead of streaming
    // a whole frame through PSRAM on every UI update.
    //
    // A full-screen buffer (with disp_drv.full_refresh) was tried here and made
    // things worse for exactly that reason.
    //
    // 64-byte aligned because the driver runs cache maintenance on it.
    size_t bufSize = screenWidth * 10 * sizeof(lv_color_t);
    buf1 = (lv_color_t *)heap_caps_aligned_alloc(64, bufSize, MALLOC_CAP_DMA);
    if (buf1 == NULL)
    {
        ConsoleError.println("[XTouch][SCREEN] aligned draw buffer alloc failed, falling back");
        buf1 = (lv_color_t *)heap_caps_malloc(bufSize, MALLOC_CAP_DMA);
    }
    lv_disp_draw_buf_init(&draw_buf, buf1, NULL, screenWidth * 10);

    /*Initialize the display*/
    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = screenWidth;
    disp_drv.ver_res = screenHeight;
    disp_drv.flush_cb = xtouch_screen_dispFlush;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    /*Initialize the input device driver*/
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = xtouch_screen_touchRead;
    lv_indev_drv_register(&indev_drv);

    /*Initialize the graphics library */
    LV_EVENT_GET_COMP_CHILD = lv_event_register_id();

    lv_disp_t *dispp = lv_disp_get_default();
    lv_theme_t *theme = lv_theme_default_init(dispp, lv_palette_main(LV_PALETTE_BLUE), lv_palette_main(LV_PALETTE_RED), true, LV_FONT_DEFAULT);
    lv_disp_set_theme(dispp, theme);

    initTopLayer();
}

#endif
