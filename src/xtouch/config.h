#ifndef _XLCD_JSONCONFIG
#define _XLCD_JSONCONFIG

#include <ArduinoJson.h>
#include "filesystem.h"
#include "paths.h"

/*
{
  "ssid": "xperiments-2.4G",
  "pwd": "viernes13",
  "timeout": "3000",
  "coldboot": "5000",
  "mqtt": {
    "host": "192.168.0.18",
    "accessCode": "24660910",
    "serialNumber": "01S00C342600288",
    "printerModel": "P1P"
  },
  "homeassistant": {
    "enabled": true,
    "host": "192.168.0.50",
    "port": 1883,
    "user": "mqtt_user",
    "password": "mqtt_pass"
  }
}
*/
DynamicJsonDocument xtouch_load_config()
{
    DynamicJsonDocument config = xtouch_filesystem_readJson(SD, xtouch_paths_config);

    strcpy(xTouchConfig.xTouchAccessCode, config["mqtt"]["accessCode"].as<const char *>());
    strcpy(xTouchConfig.xTouchSerialNumber, config["mqtt"]["serialNumber"].as<const char *>());
    strcpy(xTouchConfig.xTouchHost, config["mqtt"]["host"].as<const char *>());
    strcpy(xTouchConfig.xTouchPrinterModel, config["mqtt"]["printerModel"].as<const char *>());

    // Home Assistant local broker (optional block — chamber sensor publish is skipped if absent)
    xTouchConfig.xTouchHAEnabled = config["homeassistant"]["enabled"] | false;
    strcpy(xTouchConfig.xTouchHAHost, config["homeassistant"]["host"] | "");
    xTouchConfig.xTouchHAPort = config["homeassistant"]["port"] | 1883;
    strcpy(xTouchConfig.xTouchHAUser, config["homeassistant"]["user"] | "");
    strcpy(xTouchConfig.xTouchHAPassword, config["homeassistant"]["password"] | "");

    return xtouch_filesystem_readJson(SD, xtouch_paths_config);
}

#endif