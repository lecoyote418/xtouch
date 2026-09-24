// ---------------------------------------------------------------------------
// Minimal RGB panel test for the 5" Jingcai JC8048W550C (ESP32-S3).
//
// Deliberately contains nothing else: no LVGL, no UI, no Wi-Fi, no MQTT, no SD
// card, no sensors, no touch. The point is to answer one question in isolation:
//
//     can this board drive its RGB panel stably with ESP-IDF's esp_lcd driver
//     and the vendor's own timing/pin parameters?
//
// Everything here comes from the vendor board definition:
// https://github.com/esp-arduino-libs/ESP32_Display_Panel/blob/master/src/board/supported/jingcai/BOARD_JINGCAI_JC8048W550C.h
// (800x480, ST7262, HPW 4 / HBP 8 / HFP 8, VPW 4 / VBP 8 / VFP 8, 16 MHz,
//  pclk active negative, backlight on GPIO2.)
//
// What it does:
//   - brings the panel up, then paints colour bars plus a slowly moving white
//     bar. A moving bar makes any DMA desync obvious: it jumps sideways and
//     does not come back on its own.
//   - prints a heartbeat once a second (frame count, fps, free heap) so a
//     stall or a reset is visible on the serial console.
//
// Usage:
//   pio run -e jc8048w550c_i-lcdtest -t upload
//   pio device monitor -e jc8048w550c_i-lcdtest
//
// Optional stressor, add to the env's build_flags one step at a time:
//   -DLCDTEST_WIFI          associate with the AP in config.json and keep
//                           drawing, to see whether Wi-Fi alone disturbs the
//                           panel (and whether the LEDC backlight survives it)
// ---------------------------------------------------------------------------

#include <Arduino.h>
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_heap_caps.h"

#define TEST_W 800
#define TEST_H 480

// ---- Vendor pin map ----
#define PIN_HSYNC 39
#define PIN_VSYNC 41
#define PIN_DE 40
#define PIN_PCLK 42
#define PIN_BL 2

static const int kDataPins[16] = {8, 3, 46, 9, 1, 5, 6, 7, 15, 16, 4, 45, 48, 47, 21, 14};

static esp_lcd_panel_handle_t s_panel = NULL;
static uint16_t *s_fb = NULL;
static uint32_t s_frames = 0;

static void panelInit()
{
    esp_lcd_rgb_panel_config_t c = {};
    c.clk_src = LCD_CLK_SRC_PLL160M;
    c.timings.pclk_hz = 16 * 1000 * 1000;
    c.timings.h_res = TEST_W;
    c.timings.v_res = TEST_H;
    c.timings.hsync_pulse_width = 4;
    c.timings.hsync_back_porch = 8;
    c.timings.hsync_front_porch = 8;
    c.timings.vsync_pulse_width = 4;
    c.timings.vsync_back_porch = 8;
    c.timings.vsync_front_porch = 8;
    c.timings.flags.pclk_active_neg = true;
    c.timings.flags.hsync_idle_low = false;
    c.timings.flags.vsync_idle_low = false;
    c.timings.flags.de_idle_high = false;
    c.timings.flags.pclk_idle_high = false;
    c.data_width = 16;
    c.bits_per_pixel = 16;
    c.num_fbs = 2;                  // double framebuffer, EDMA straight from PSRAM
    c.bounce_buffer_size_px = 0;
    c.dma_burst_size = 64;
    c.hsync_gpio_num = PIN_HSYNC;
    c.vsync_gpio_num = PIN_VSYNC;
    c.de_gpio_num = PIN_DE;
    c.pclk_gpio_num = PIN_PCLK;
    c.disp_gpio_num = -1;
    for (int i = 0; i < 16; i++)
    {
        c.data_gpio_nums[i] = kDataPins[i];
    }
    c.flags.fb_in_psram = true;

    ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&c, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
}

static void backlightInit()
{
    // The vendor board definition drives the backlight with LEDC at 1 kHz and
    // 10-bit resolution.
    ledcAttach(PIN_BL, 1000, 10);
    ledcWrite(PIN_BL, 1023);
}

static void drawPattern(uint32_t frame)
{
    static const uint16_t bars[8] = {
        0xF800, // red
        0x07E0, // green
        0x001F, // blue
        0xFFFF, // white
        0xF81F, // magenta
        0x07FF, // cyan
        0xFFE0, // yellow
        0x0000, // black
    };
    const int barW = TEST_W / 8;

    for (int y = 0; y < TEST_H; y++)
    {
        uint16_t *row = &s_fb[y * TEST_W];
        for (int x = 0; x < TEST_W; x++)
        {
            row[x] = bars[x / barW];
        }
    }

    // Moving white bar plus 1px border: any horizontal shift of the image shows
    // up immediately as the bar jumping position, and a stale/black frame is
    // obvious because the border disappears.
    int bx = (int)((frame * 6) % (TEST_W - 24));
    for (int y = 0; y < TEST_H; y++)
    {
        uint16_t *row = &s_fb[y * TEST_W];
        for (int x = bx; x < bx + 24; x++)
        {
            row[x] = 0xFFFF;
        }
    }
    for (int x = 0; x < TEST_W; x++)
    {
        s_fb[x] = 0x0000;
        s_fb[(TEST_H - 1) * TEST_W + x] = 0x0000;
    }
    for (int y = 0; y < TEST_H; y++)
    {
        s_fb[y * TEST_W] = 0x0000;
        s_fb[y * TEST_W + TEST_W - 1] = 0x0000;
    }

    esp_lcd_panel_draw_bitmap(s_panel, 0, 0, TEST_W, TEST_H, s_fb);
    s_frames++;
}

#if defined(LCDTEST_LVGL)
// ---------------------------------------------------------------------------
// Stage 3: drive the panel through LVGL instead of painting full frames by
// hand. This is the last big difference from the application - the app renders
// through LVGL with a small partial draw buffer and flushes rectangles, while
// every test so far has copied whole frames.
//
// The UI is deliberately near-static (a border and one label) with the label
// updated every 2 s, which is what the application does: an incoming MQTT
// message updates a few labels, LVGL redraws those areas, and each redraw
// flushes one or more rectangles into the framebuffer.
// ---------------------------------------------------------------------------
#include <lvgl.h>

#define LVGL_LABELS 12

static lv_disp_draw_buf_t s_lvDrawBuf;
static lv_color_t *s_lvDraw = NULL;
static lv_obj_t *s_lvGroup = NULL;
static lv_obj_t *s_lvLabels[LVGL_LABELS];
static lv_obj_t *s_lvBar = NULL;

static void lvFlush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p)
{
    esp_lcd_panel_draw_bitmap(s_panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, color_p);
    lv_disp_flush_ready(disp);
}

static void lvglInit()
{
    lv_init();

    // Same shape as the application: 10 lines, internal DMA-capable SRAM,
    // 64-byte aligned.
    size_t n = TEST_W * 10;
    s_lvDraw = (lv_color_t *)heap_caps_aligned_alloc(64, n * sizeof(lv_color_t), MALLOC_CAP_DMA);
    if (!s_lvDraw)
    {
        Serial.println("[lcbtest] FATAL: could not allocate the LVGL draw buffer");
        while (true)
            delay(1000);
    }
    lv_disp_draw_buf_init(&s_lvDrawBuf, s_lvDraw, NULL, n);

    static lv_disp_drv_t drv;
    lv_disp_drv_init(&drv);
    drv.hor_res = TEST_W;
    drv.ver_res = TEST_H;
    drv.flush_cb = lvFlush;
    drv.draw_buf = &s_lvDrawBuf;
    lv_disp_drv_register(&drv);

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101820), 0);

    lv_obj_t *box = lv_obj_create(scr);
    lv_obj_set_size(box, TEST_W - 20, TEST_H - 20);
    lv_obj_center(box);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(box, 4, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(0x00FF00), 0);

    // Group of labels plus a bar, all updated together once per burst. An
    // application status push sends ~15 LVGL messages at once (bed/nozzle
    // temperatures and targets, fans, AMS, print status), so LVGL redraws many
    // separate areas within a single lv_timer_handler() pass. Updating one
    // label was not representative.
    s_lvGroup = lv_obj_create(box);
    lv_obj_set_size(s_lvGroup, TEST_W - 40, TEST_H - 120);
    lv_obj_align(s_lvGroup, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_flex_flow(s_lvGroup, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_color(s_lvGroup, lv_color_hex(0x202830), 0);
    lv_obj_set_style_pad_all(s_lvGroup, 8, 0);

    for (int i = 0; i < LVGL_LABELS; i++)
    {
        s_lvLabels[i] = lv_label_create(s_lvGroup);
        lv_label_set_text_fmt(s_lvLabels[i], "field %d: --", i);
        lv_obj_set_style_text_color(s_lvLabels[i], lv_color_hex(0xE0E0E0), 0);
    }

    s_lvBar = lv_bar_create(scr);
    lv_obj_set_size(s_lvBar, TEST_W - 60, 18);
    lv_obj_align(s_lvBar, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_bar_set_range(s_lvBar, 0, 100);
    lv_bar_set_value(s_lvBar, 50, LV_ANIM_OFF);

    lv_timer_handler();
}
#endif

// One place to pump whichever display path is active.
static void pumpDisplay()
{
#if defined(LCDTEST_LVGL)
    lv_timer_handler();
    s_frames++;
#else
    drawPattern(s_frames);
#endif
}

#if defined(LCDTEST_WIFI)
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <FS.h>
#include <SD.h>
#include <ArduinoJson.h>

static void wifiPhase()
{
    // Same config.json the application uses, so the test exercises the same
    // Wi-Fi path (association + PHY calibration).
    if (!SD.begin())
    {
        Serial.println("[lcbtest] no SD card, skipping Wi-Fi phase");
        return;
    }
    DynamicJsonDocument doc(512);
    File f = SD.open("/config.json");
    if (!f)
    {
        Serial.println("[lcbtest] no /config.json, skipping Wi-Fi phase");
        return;
    }
    deserializeJson(doc, f);
    f.close();

    const char *ssid = doc["ssid"];
    const char *pwd = doc["pwd"];
    if (!ssid)
    {
        Serial.println("[lcbtest] config.json has no ssid, skipping Wi-Fi phase");
        return;
    }

    Serial.printf("[lcbtest] associating with \"%s\" (watch the panel and the backlight)\n", ssid);
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, pwd);
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 30000)
    {
        pumpDisplay();
        delay(10);
    }
    Serial.printf("[lcbtest] wifi status=%d ip=%s (panel still drawing at %u frames)\n",
                  (int)WiFi.status(), WiFi.localIP().toString().c_str(), (unsigned)s_frames);
}

// Periodic TLS request, to imitate what an incoming MQTT message costs the
// application: a TLS record decrypt plus parsing, on top of the Wi-Fi stack.
// Override the target with -DLCDTEST_TLS_URL=\"...\" if you have no internet
// access (a local TLS endpoint works just as well).
#ifndef LCDTEST_TLS_URL
#define LCDTEST_TLS_URL "https://example.com/"
#endif

static void tlsGet(uint32_t n)
{
    WiFiClientSecure client;
    client.setInsecure(); // diagnostic only, certificate not needed
    HTTPClient https;
    https.setTimeout(5000);

    uint32_t t0 = micros();
    if (https.begin(client, LCDTEST_TLS_URL))
    {
        int code = https.GET();
        int len = https.getSize();
        https.end();
        Serial.printf("[lcbtest] TLS GET #%u -> %d (%d bytes) in %u us - did the panel glitch just now?\n",
                      (unsigned)n, code, len, (unsigned)(micros() - t0));
    }
    else
    {
        Serial.printf("[lcbtest] TLS GET #%u could not start (no route to %s?)\n",
                      (unsigned)n, LCDTEST_TLS_URL);
    }
}
#endif

#if defined(LCDTEST_LOAD)
// ---------------------------------------------------------------------------
// Synthetic load: every 2 s, burn CPU and PSRAM bandwidth for a short burst,
// imitating what an incoming MQTT message costs the application (TLS record
// decryption plus JSON parsing, both CPU-heavy and both with a large working
// set). If the panel glitches in step with these bursts, the problem is pure
// CPU/PSRAM contention and has nothing to do with the network stack.
//
// Two flavours so the cause can be separated:
//   -DLCDTEST_LOAD_PSRAM   memcpy between two PSRAM buffers (bandwidth)
//   -DLCDTEST_LOAD_CPU     arithmetic over a small buffer (CPU only)
// Defining plain -DLCDTEST_LOAD enables the PSRAM one.
// ---------------------------------------------------------------------------
#define LOAD_CHUNK 65536
static uint8_t *s_loadA = NULL;
static uint8_t *s_loadB = NULL;

static void loadInit()
{
    s_loadA = (uint8_t *)heap_caps_malloc(LOAD_CHUNK, MALLOC_CAP_SPIRAM);
    s_loadB = (uint8_t *)heap_caps_malloc(LOAD_CHUNK, MALLOC_CAP_SPIRAM);
    if (!s_loadA || !s_loadB)
    {
        Serial.println("[lcbtest] WARNING: could not allocate the load buffers");
    }
}

static void loadBurst()
{
#if defined(LCDTEST_LOAD_CPU)
    // ~a few ms of pure arithmetic, tiny working set.
    volatile uint32_t acc = 1;
    for (uint32_t i = 0; i < 400000; i++)
    {
        acc = acc * 1664525u + 1013904223u;
        acc ^= (acc >> 13);
    }
#else
    // ~2 MB of PSRAM->PSRAM copying: the bandwidth-hungry version.
    if (!s_loadA || !s_loadB)
    {
        return;
    }
    for (int i = 0; i < 32; i++)
    {
        memcpy(s_loadA, s_loadB, LOAD_CHUNK);
    }
#endif
}
#endif

#if defined(LCDTEST_FLASH)
// ---------------------------------------------------------------------------
// Flash-write stressor: rewrite a small NVS entry every 2 s while the panel is
// drawing.
//
// On ESP32-S3 the SPI flash and the PSRAM share the MSPI bus, and while the
// flash is being written/erased PSRAM is not accessible - so the LCD's EDMA,
// which reads the framebuffer *from PSRAM*, is starved. The visible result is
// the image shifting sideways with a black flash, recovered on the next VBlank
// by CONFIG_LCD_RGB_RESTART_IN_VSYNC.
//
// NOTE: this used to write 4 KB blobs under rotating keys, which filled the
// 20 KB NVS partition within a minute or so; after that putBytes() stopped
// touching flash at all and the glitches "stopped for no reason". One small
// entry under a single key is rewritten/compacted indefinitely, so the flash
// traffic continues for the whole run. The log prints the write size and the
// free entry count so it is obvious whether writes are still succeeding.
// ---------------------------------------------------------------------------
#include <Preferences.h>
#define NVS_BLOB 256
static Preferences s_prefs;
static uint8_t *s_nvsBuf = NULL;

static void flashInit()
{
    s_nvsBuf = (uint8_t *)malloc(NVS_BLOB);
    if (s_nvsBuf)
    {
        memset(s_nvsBuf, 0x5A, NVS_BLOB);
    }
    if (!s_prefs.begin("lcbtest", false))
    {
        Serial.println("[lcbtest] WARNING: could not open the NVS namespace");
    }
    Serial.printf("[lcbtest] NVS namespace open, freeEntries=%u\n", (unsigned)s_prefs.freeEntries());
}

static void flashBurst(uint32_t n)
{
    if (!s_nvsBuf)
    {
        return;
    }
    s_nvsBuf[0] = (uint8_t)n;
    // One small entry, rewritten every time: NVS keeps writing it and
    // compacting the page, so flash traffic never runs out.
    size_t written = s_prefs.putBytes("blob", s_nvsBuf, NVS_BLOB);
    Serial.printf("[lcbtest] FLASH WRITE %u/%u bytes ok=%d freeEntries=%u\n",
                  (unsigned)written, (unsigned)NVS_BLOB, written == NVS_BLOB ? 1 : 0,
                  (unsigned)s_prefs.freeEntries());
}
#endif

void setup()
{
    Serial.begin(115200);
    delay(200);
    Serial.println();
    Serial.println("[lcbtest] minimal RGB panel test - vendor parameters, no LVGL/Wi-Fi/MQTT");
    Serial.printf("[lcbtest] %dx%d @ 16 MHz, HPW4 HBP8 HFP8 VPW4 VBP8 VFP8, pclk_active_neg, 2 framebuffers in PSRAM\n",
                  TEST_W, TEST_H);

    s_fb = (uint16_t *)heap_caps_aligned_alloc(64, (size_t)TEST_W * TEST_H * 2, MALLOC_CAP_SPIRAM);
    if (!s_fb)
    {
        Serial.println("[lcbtest] FATAL: could not allocate the PSRAM framebuffer");
        while (true)
            delay(1000);
    }
    Serial.printf("[lcbtest] framebuffer in PSRAM at %p, free heap %u, free PSRAM %u\n",
                  s_fb, (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram());

#if defined(LCDTEST_LOAD)
    loadInit();
#endif
#if defined(LCDTEST_FLASH)
    flashInit();
#endif

    backlightInit();
    panelInit();
    Serial.printf("[lcbtest] panel up, free heap %u\n", (unsigned)ESP.getFreeHeap());

#if defined(LCDTEST_LVGL)
    lvglInit();
    Serial.println("[lcbtest] LVGL display registered (partial 10-line buffer, internal SRAM)");
#endif

#if defined(LCDTEST_WIFI)
    wifiPhase();
#endif

#if defined(LCDTEST_LOAD)
    Serial.println("[lcbtest] synthetic load enabled: a CPU/PSRAM burst every 2 s");
#endif
#if defined(LCDTEST_FLASH)
    Serial.println("[lcbtest] flash-write stressor enabled: a small NVS commit every 2 s");
#endif
#if defined(LCDTEST_LVGL)
    Serial.println("[lcbtest] a label is updated every 2 s, like an MQTT status push does.");
#else
    Serial.println("[lcbtest] drawing. Watch the moving white bar: it must sweep smoothly and evenly.");
#endif
}

void loop()
{
    pumpDisplay();

    static uint32_t lastMs = 0;
    static uint32_t lastFrames = 0;
    uint32_t now = millis();
    if (now - lastMs >= 1000)
    {
        uint32_t fps = s_frames - lastFrames;
        lastFrames = s_frames;
        lastMs = now;
        Serial.printf("[lcbtest] frames=%u fps=%u heap=%u psram=%u\n",
                      (unsigned)s_frames, (unsigned)fps,
                      (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram());
    }

#if defined(LCDTEST_LVGL)
    // Repaint traffic. Normally one burst every 2 s; under LCDTEST_LVGL_TORTURE
    // every loop iteration, which sustains full-frame repaint traffic the way a
    // continuously animating UI does.
    //
    // This matters: one full-screen repaint per 2 s is 768 KB, i.e. ~0.4 MB/s,
    // while the panel's EDMA alone reads ~32 MB/s. The earlier "contention is
    // tolerated" result was measured at that 1%-scale load and meant nothing.
    static uint32_t lastLabel = 0;
    static uint32_t burstCount = 0;
#if defined(LCDTEST_LVGL_TORTURE)
    bool doBurst = true;
#else
    bool doBurst = (now - lastLabel >= 2000);
#endif
    if (doBurst)
    {
        lastLabel = now;
        burstCount++;
        for (int i = 0; i < LVGL_LABELS; i++)
        {
            lv_label_set_text_fmt(s_lvLabels[i], "field %d: %u", i,
                                  (unsigned)(burstCount * 7 + i));
        }
        lv_bar_set_value(s_lvBar, (int)(burstCount % 100), LV_ANIM_OFF);
#if defined(LCDTEST_LVGL_FULL) || defined(LCDTEST_LVGL_TORTURE)
        // Force LVGL to redraw the entire screen, the way a status push
        // touching many components does. With a 10-line draw buffer that is
        // ~48 flushes of 800x10 into the framebuffer.
        lv_obj_invalidate(lv_scr_act());
#endif
        // Only log occasionally under torture, or the UART becomes the load.
#if defined(LCDTEST_LVGL_TORTURE)
        if (burstCount % 50 == 1)
#else
        if (true)
#endif
        {
            Serial.printf("[lcbtest] LVGL repaint #%u - did the panel glitch just now?\n",
                          (unsigned)burstCount);
        }
    }
#endif

#if defined(LCDTEST_LOAD)
    static uint32_t lastBurst = 0;
    if (now - lastBurst >= 2000)
    {
        lastBurst = now;
        uint32_t t0 = micros();
        loadBurst();
        uint32_t dt = micros() - t0;
        Serial.printf("[lcbtest] LOAD BURST %u us - did the panel glitch just now?\n", (unsigned)dt);
    }
#endif

#if defined(LCDTEST_FLASH)
    static uint32_t lastFlash = 0;
    static uint32_t flashCount = 0;
    if (now - lastFlash >= 2000)
    {
        lastFlash = now;
        uint32_t t0 = micros();
        flashBurst(flashCount++);
        Serial.printf("[lcbtest]   ^ FLASH WRITE took %u us - did the panel shift/flash black just now?\n",
                      (unsigned)(micros() - t0));
    }
#endif

#if defined(LCDTEST_WIFI)
    static uint32_t lastTls = 0;
    static uint32_t tlsCount = 0;
    if (WiFi.status() == WL_CONNECTED && now - lastTls >= 2000)
    {
        lastTls = now;
        tlsGet(tlsCount++);
    }
#endif
}
