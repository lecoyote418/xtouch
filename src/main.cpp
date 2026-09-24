#include <driver/i2s.h>
#include <Arduino.h>
#include <ArduinoJson.h>
#include "xtouch/debug.h"
#include "xtouch/paths.h"
#include "xtouch/eeprom.h"
#include "xtouch/types.h"
#include "xtouch/bblp.h"
#include "xtouch/globals.h"
#include "xtouch/filesystem.h"
#if defined(__XTOUCH_SCREEN_50__)
#include "ui/5.0/ui.h"
#elif defined(__XTOUCH_SCREEN_28__)
#include "ui/2.8/ui.h"
#endif
#include "xtouch/sdcard.h"
#include "xtouch/hms.h"

#if defined(__XTOUCH_SCREEN_50__)
#include "devices/5.0/screen.h"
#elif defined(__XTOUCH_SCREEN_28__)
#include "devices/2.8/screen.h"
#else
#error "Define __XTOUCH_SCREEN_50__ or __XTOUCH_SCREEN_28__ (set automatically by the platformio.ini environment you build)"
#endif

#include "xtouch/settings.h"
#include "xtouch/net.h"
#include "xtouch/firmware.h"
#include "xtouch/mqtt.h"
#include "xtouch/sensors/chamber.h"
#include "xtouch/events.h"
#include "xtouch/connection.h"
#include "xtouch/coldboot.h"

void xtouch_intro_show(void)
{
  ui_introScreen_screen_init();
  lv_disp_load_scr(introScreen);
  lv_timer_handler();
}

void setup()
{

#if XTOUCH_USE_SERIAL == true || XTOUCH_DEBUG_ERROR == true || XTOUCH_DEBUG_DEBUG == true || XTOUCH_DEBUG_INFO == true
  Serial.begin(115200);
#endif

  xtouch_eeprom_setup();
  xtouch_globals_init();
  xtouch_screen_setup();
  xtouch_intro_show();
  while (!xtouch_sdcard_setup())
    ;

  xtouch_coldboot_check();

  xtouch_settings_loadSettings();

  xtouch_firmware_checkFirmwareUpdate();

  xtouch_touch_setup();

  while (!xtouch_wifi_setup())
    ;

  xtouch_firmware_checkOnlineFirmwareUpdate();

  xtouch_screen_setupScreenTimer();
  xtouch_setupGlobalEvents();

  // Build with -DXTOUCH_DISABLE_MQTT=1 to run with no MQTT/HA network traffic
  // at all (the display and UI still work). Mainly a diagnostic: it separates
  // glitches caused by network activity from glitches caused by UI updates.
#if !defined(XTOUCH_DISABLE_MQTT)
  xtouch_mqtt_setup();
  xtouch_ha_mqtt_setup();
#else
  ConsoleInfo.println(F("[XTouch] MQTT disabled (XTOUCH_DISABLE_MQTT)"));
  // Normally the first MQTT connection loads the home screen; without MQTT do
  // it here so the UI is still exercised.
  loadScreen(0);
#endif
  xtouch_chamber_timer_init();
}

void loop()
{
  lv_timer_handler();
  lv_task_handler();
#if !defined(XTOUCH_DISABLE_MQTT)
  xtouch_mqtt_loop();
  xtouch_ha_mqtt_loop();
#endif
}
