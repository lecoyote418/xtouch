#ifndef _XLCD_SENSORS_HA_MQTT
#define _XLCD_SENSORS_HA_MQTT

#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

// Separate plain (non-TLS) connection to the user's own local broker —
// independent of xtouch_pubSubClient, which is the TLS connection to the
// printer itself. Home Assistant's native MQTT integration listens on
// this broker for the discovery topic below.
WiFiClient xtouch_ha_wifiClient;
PubSubClient xtouch_ha_pubSubClient(xtouch_ha_wifiClient);

String xtouch_ha_discovery_topic;
String xtouch_ha_state_topic;
unsigned long xtouch_ha_lastReconnectAttempt = 0;

void xtouch_ha_mqtt_publishDiscovery()
{
    DynamicJsonDocument doc(768);
    doc["name"] = "Chamber Temperature";
    doc["unique_id"] = String("xtouch_") + xTouchConfig.xTouchSerialNumber + "_chamber_temp";
    doc["state_topic"] = xtouch_ha_state_topic;
    doc["unit_of_measurement"] = "°C";
    doc["device_class"] = "temperature";
    doc["state_class"] = "measurement";

    JsonObject device = doc.createNestedObject("device");
    JsonArray identifiers = device.createNestedArray("identifiers");
    identifiers.add(String("xtouch_") + xTouchConfig.xTouchSerialNumber);
    device["name"] = String(xTouchConfig.xTouchPrinterName) + " Chamber Sensor";
    device["manufacturer"] = "xTouch";
    device["model"] = "DHT22";

    String payload;
    serializeJson(doc, payload);

    // retained so HA picks up the entity on restart without waiting for a new reading
    bool published = xtouch_ha_pubSubClient.publish(xtouch_ha_discovery_topic.c_str(), payload.c_str(), true);
    if (!published)
    {
        ConsoleError.printf("[XTouch][HA-MQTT] Discovery publish FAILED (payload %d bytes, buffer %d bytes)\n",
                             payload.length(), xtouch_ha_pubSubClient.getBufferSize());
    }
}

bool xtouch_ha_mqtt_connect()
{
    String clientId = String("xtouch-") + xTouchConfig.xTouchSerialNumber;
    bool connected;

    if (strlen(xTouchConfig.xTouchHAUser) > 0)
    {
        connected = xtouch_ha_pubSubClient.connect(clientId.c_str(), xTouchConfig.xTouchHAUser, xTouchConfig.xTouchHAPassword);
    }
    else
    {
        connected = xtouch_ha_pubSubClient.connect(clientId.c_str());
    }

    if (connected)
    {
        ConsoleInfo.println(F("[XTouch][HA-MQTT] ---- CONNECTED ----"));
        xtouch_ha_mqtt_publishDiscovery();
    }
    else
    {
        ConsoleError.printf("[XTouch][HA-MQTT] ---- CONNECTION FAIL ----: %d\n", xtouch_ha_pubSubClient.state());
    }

    return connected;
}

void xtouch_ha_mqtt_setup()
{
    if (!xTouchConfig.xTouchHAEnabled || strlen(xTouchConfig.xTouchHAHost) == 0)
    {
        ConsoleInfo.println(F("[XTouch][HA-MQTT] Disabled or not configured, skipping"));
        return;
    }

    xtouch_ha_discovery_topic = String("homeassistant/sensor/xtouch_") + xTouchConfig.xTouchSerialNumber + "_chamber/config";
    xtouch_ha_state_topic = String("xtouch/") + xTouchConfig.xTouchSerialNumber + "/chamber/temperature";

    // Default PubSubClient buffer is 256 bytes — too small for the discovery
    // JSON payload (name + unique_id + device block), so publish() would
    // otherwise fail silently with no error and no entity ever appearing in HA
    xtouch_ha_pubSubClient.setBufferSize(512);

    xtouch_ha_pubSubClient.setServer(xTouchConfig.xTouchHAHost, xTouchConfig.xTouchHAPort);
    xtouch_ha_mqtt_connect();
}

// Call from the main loop — non-blocking, unlike the printer MQTT reconnect,
// so an unreachable home broker never freezes the touchscreen UI
void xtouch_ha_mqtt_loop()
{
    if (!xTouchConfig.xTouchHAEnabled || strlen(xTouchConfig.xTouchHAHost) == 0)
    {
        return;
    }

    if (!xtouch_ha_pubSubClient.connected())
    {
        unsigned long now = millis();
        if (now - xtouch_ha_lastReconnectAttempt > 5000)
        {
            xtouch_ha_lastReconnectAttempt = now;
            xtouch_ha_mqtt_connect();
        }
        return;
    }

    xtouch_ha_pubSubClient.loop();
}

void xtouch_ha_publishChamberTemp(float temperatureC)
{
    if (!xTouchConfig.xTouchHAEnabled || strlen(xTouchConfig.xTouchHAHost) == 0)
    {
        return;
    }

    if (!xtouch_ha_pubSubClient.connected())
    {
        return;
    }

    char payload[8];
    snprintf(payload, sizeof(payload), "%.1f", temperatureC);
    xtouch_ha_pubSubClient.publish(xtouch_ha_state_topic.c_str(), payload);
}

#endif
