#ifndef _XLCD_SENSORS_CHAMBER_TEMP
#define _XLCD_SENSORS_CHAMBER_TEMP

#include "ha_mqtt.h"

#define XTOUCH_CHAMBER_TEMP_PIN 22

// ---- Pick your physical chamber sensor here ----
// Set to DHT22 or DS18B20. Only one can be active per build.
#define XTOUCH_CHAMBER_SENSOR_DHT22 1
#define XTOUCH_CHAMBER_SENSOR_DS18B20 2
#define XTOUCH_CHAMBER_SENSOR_TYPE XTOUCH_CHAMBER_SENSOR_DHT22
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
