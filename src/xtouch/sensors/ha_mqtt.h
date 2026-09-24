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

// Reconnect pacing: the first retry is after HA_RECONNECT_MIN_MS, and the
// interval doubles up to HA_RECONNECT_MAX_MS while the broker stays down.
// Combined with the 1 s connection timeout set in xtouch_ha_mqtt_start(), the
// worst case is a 1 s stall once a minute instead of every 5 s.
#define HA_RECONNECT_MIN_MS 5000
#define HA_RECONNECT_MAX_MS 60000
unsigned long xtouch_ha_reconnectInterval = HA_RECONNECT_MIN_MS;

// True once the HA link is configured and running. Toggled at runtime by the
// "HOME ASSISTANT" switch in the settings screen, so the client can be started
// and stopped without a reboot.
bool xtouch_ha_running = false;

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

// Configure (or re-configure) and connect the Home Assistant link. Safe to call
// repeatedly: the first call does the setup, later calls are a no-op while the
// link is already running.
void xtouch_ha_mqtt_start()
{
    if (xtouch_ha_running)
    {
        return;
    }

    if (strlen(xTouchConfig.xTouchHAHost) == 0)
    {
        ConsoleInfo.println(F("[XTouch][HA-MQTT] No broker host configured, nothing to start"));
        return;
    }

    xtouch_ha_running = true;

    xtouch_ha_discovery_topic = String("homeassistant/sensor/xtouch_") + xTouchConfig.xTouchSerialNumber + "_chamber/config";
    xtouch_ha_state_topic = String("xtouch/") + xTouchConfig.xTouchSerialNumber + "/chamber/temperature";

    // Default PubSubClient buffer is 256 bytes — too small for the discovery
    // JSON payload (name + unique_id + device block), so publish() would
    // otherwise fail silently with no error and no entity ever appearing in HA
    xtouch_ha_pubSubClient.setBufferSize(512);

    // Bound how long a connection attempt may block the main loop.
    //
    // This is what made the UI lag when the broker was down: the loop retries
    // every HA_RECONNECT_MIN_MS, but PubSubClient::connect() blocks inside the
    // socket connect, and WiFiClient's default connection timeout is 30 s - so
    // LVGL (which runs from the same loop) stalled for seconds at a time. 1 s is
    // plenty for a broker on the local network.
    xtouch_ha_wifiClient.setConnectionTimeout(1000);
    xtouch_ha_pubSubClient.setSocketTimeout(1);

    xtouch_ha_pubSubClient.setServer(xTouchConfig.xTouchHAHost, xTouchConfig.xTouchHAPort);

    xtouch_ha_reconnectInterval = HA_RECONNECT_MIN_MS;
    xtouch_ha_lastReconnectAttempt = 0;
    xtouch_ha_mqtt_connect();
}

// Stop the Home Assistant link and drop any open socket. Called when the user
// turns the switch off: nothing is retried until it is turned back on.
void xtouch_ha_mqtt_stop()
{
    if (!xtouch_ha_running)
    {
        return;
    }

    xtouch_ha_running = false;

    if (xtouch_ha_pubSubClient.connected())
    {
        xtouch_ha_pubSubClient.disconnect();
    }
    xtouch_ha_wifiClient.stop();

    ConsoleInfo.println(F("[XTouch][HA-MQTT] Stopped"));
}

// Boot-time entry point: honour the persisted switch state from config.json.
void xtouch_ha_mqtt_setup()
{
    if (!xTouchConfig.xTouchHAEnabled || strlen(xTouchConfig.xTouchHAHost) == 0)
    {
        ConsoleInfo.println(F("[XTouch][HA-MQTT] Disabled or not configured, skipping"));
        return;
    }

    xtouch_ha_mqtt_start();
}

// Call from the main loop.
void xtouch_ha_mqtt_loop()
{
    if (!xtouch_ha_running || !xTouchConfig.xTouchHAEnabled || strlen(xTouchConfig.xTouchHAHost) == 0)
    {
        return;
    }

    if (!xtouch_ha_pubSubClient.connected())
    {
        unsigned long now = millis();
        if (now - xtouch_ha_lastReconnectAttempt >= xtouch_ha_reconnectInterval)
        {
            xtouch_ha_lastReconnectAttempt = now;
            if (xtouch_ha_mqtt_connect())
            {
                xtouch_ha_reconnectInterval = HA_RECONNECT_MIN_MS;
            }
            else
            {
                xtouch_ha_reconnectInterval *= 2;
                if (xtouch_ha_reconnectInterval > HA_RECONNECT_MAX_MS)
                {
                    xtouch_ha_reconnectInterval = HA_RECONNECT_MAX_MS;
                }
            }
            return;
        }
        return;
    }

    xtouch_ha_reconnectInterval = HA_RECONNECT_MIN_MS;
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
