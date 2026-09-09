#ifndef _XLCD_SENSORS_CHAMBER_TEMP
#define _XLCD_SENSORS_CHAMBER_TEMP

#include "ha_mqtt.h"

// Default chamber sensor GPIO per board — override with a build flag
// (-DXTOUCH_CHAMBER_TEMP_PIN=n) if your wiring differs.
// GPIO17 is free on the ESP32-S3 5" board; GPIO22 (the original default)
// works fine on the plain ESP32 used by the 2.8" board, but not on the S3
// (GPIO22-25 don't exist on that chip).
#ifndef XTOUCH_CHAMBER_TEMP_PIN
#if defined(__XTOUCH_SCREEN_50__)
#define XTOUCH_CHAMBER_TEMP_PIN 17
#else
#define XTOUCH_CHAMBER_TEMP_PIN 22
#endif
#endif

// ---- Pick your physical chamber sensor here, or override via build flag ----
// Set XTOUCH_CHAMBER_SENSOR_TYPE to 1 (DHT22) or 2 (DS18B20), e.g.
// -DXTOUCH_CHAMBER_SENSOR_TYPE=2 in platformio.ini build_flags.
// Only one can be active per build.
#define XTOUCH_CHAMBER_SENSOR_DHT22 1
#define XTOUCH_CHAMBER_SENSOR_DS18B20 2
#ifndef XTOUCH_CHAMBER_SENSOR_TYPE
#define XTOUCH_CHAMBER_SENSOR_TYPE XTOUCH_CHAMBER_SENSOR_DHT22
#endif
// --------------------------------------------------

#if XTOUCH_CHAMBER_SENSOR_TYPE == XTOUCH_CHAMBER_SENSOR_DHT22

#include <DHT.h>
DHT xtouch_chamber_sensor(XTOUCH_CHAMBER_TEMP_PIN, DHT22);

#elif XTOUCH_CHAMBER_SENSOR_TYPE == XTOUCH_CHAMBER_SENSOR_DS18B20

#include <OneWire.h>
#include <DallasTemperature.h>
OneWire temperatureSensorsOneWire(XTOUCH_CHAMBER_TEMP_PIN);
DallasTemperature xtouch_chamber_sensors(&temperatureSensorsOneWire);

#else
#error "XTOUCH_CHAMBER_SENSOR_TYPE must be XTOUCH_CHAMBER_SENSOR_DHT22 or XTOUCH_CHAMBER_SENSOR_DS18B20"
#endif

lv_timer_t *xtouch_chambertemp_requestTemperaturesTimer;

void xtouch_chamber_requestTemperatures(lv_timer_t *timer);

void xtouch_chamber_timer_create()
{
    // Neither sensor should be sampled faster than every ~2s (DHT22's own minimum,
    // and DS18B20's default 12-bit conversion time); 2500ms works for both.
    xtouch_chambertemp_requestTemperaturesTimer = lv_timer_create(xtouch_chamber_requestTemperatures, 2500, NULL);
    lv_timer_set_repeat_count(xtouch_chambertemp_requestTemperaturesTimer, 1);
}

void xtouch_chamber_requestTemperatures(lv_timer_t *timer)
{
#if XTOUCH_CHAMBER_SENSOR_TYPE == XTOUCH_CHAMBER_SENSOR_DHT22

    float rawTemperatureC = xtouch_chamber_sensor.readTemperature();
    bool validReading = !isnan(rawTemperatureC);

#elif XTOUCH_CHAMBER_SENSOR_TYPE == XTOUCH_CHAMBER_SENSOR_DS18B20

    float rawTemperatureC = xtouch_chamber_sensors.getTempCByIndex(0);
    bool validReading = (rawTemperatureC != DEVICE_DISCONNECTED_C);
    xtouch_chamber_sensors.requestTemperatures(); // kick off the next async conversion

#endif

    // Skip publishing on a failed read rather than sending a bogus value —
    // just try again on the next tick
    if (validReading)
    {
        int temperatureC = (int)rawTemperatureC + xTouchConfig.xTouchChamberSensorReadingDiff;
        bambuStatus.chamber_temper = temperatureC;
        xtouch_mqtt_sendMsg(XTOUCH_ON_CHAMBER_TEMP, temperatureC);
        xtouch_ha_publishChamberTemp(temperatureC);
    }

    xtouch_chamber_timer_create();
}

bool xtouch_chamber_started = false;
void xtouch_chamber_timer_start()
{
    if (!xtouch_chamber_started)
    {
#if XTOUCH_CHAMBER_SENSOR_TYPE == XTOUCH_CHAMBER_SENSOR_DHT22
        xtouch_chamber_sensor.begin();
#elif XTOUCH_CHAMBER_SENSOR_TYPE == XTOUCH_CHAMBER_SENSOR_DS18B20
        xtouch_chamber_sensors.begin();
        xtouch_chamber_sensors.setWaitForConversion(false);
#endif
        xtouch_chamber_started = true;
    }
    xtouch_chamber_timer_create();
}

void xtouch_chamber_timer_stop()
{
    lv_timer_pause(xtouch_chambertemp_requestTemperaturesTimer);
}

void xtouch_chamber_timer_init()
{
    if (!xtouch_bblp_is_p1Series())
    {
        return;
    }

    if (xTouchConfig.xTouchChamberSensorEnabled)
    {
        xtouch_chamber_timer_start();
    }
    else
    {
        if (xtouch_chamber_started)
        {

            xtouch_chamber_timer_stop();
        }
    }
}

#endif
