#ifndef _XLCD_TOUCH
#define _XLCD_TOUCH

XTouchPanelConfig x_touch_touchConfig;

class ScreenPoint
{
public:
    int16_t x;
    int16_t y;

    // default constructor
    ScreenPoint()
    {
    }

    ScreenPoint(int16_t xIn, int16_t yIn)
    {
        x = xIn;
        y = yIn;
    }
};

ScreenPoint getScreenCoords(int16_t x, int16_t y)
{
    int16_t xCoord = round((x * x_touch_touchConfig.xCalM) + x_touch_touchConfig.xCalC);
    int16_t yCoord = round((y * x_touch_touchConfig.yCalM) + x_touch_touchConfig.yCalC);
    if (xCoord < 0)
        xCoord = 0;
    if (xCoord >= 800)
        xCoord = 800 - 1;
    if (yCoord < 0)
        yCoord = 0;
    if (yCoord >= 480)
        yCoord = 480 - 1;
    return (ScreenPoint(xCoord, yCoord));
}

void xtouch_loadTouchConfig(XTouchPanelConfig &config)
{
    // Open file for reading
    File file = xtouch_filesystem_open(SD, xtouch_paths_touch);

    // Allocate a temporary JsonDocument
    // Don't forget to change the capacity to match your requirements.
    // Use arduinojson.org/v6/assistant to compute the capacity.
    DynamicJsonDocument doc(512);

    // Deserialize the JSON document
    DeserializationError error = deserializeJson(doc, file);
    if (error)
        ConsoleError.println(F("[XTouch][Touch] Failed to read touch config"));

    config.xCalM = doc["xCalM"].as<float>();
    config.yCalM = doc["yCalM"].as<float>();
    config.xCalC = doc["xCalC"].as<float>();
    config.yCalC = doc["yCalC"].as<float>();

    file.close();
}

void xtouch_saveTouchConfig(XTouchPanelConfig &config)
{
    DynamicJsonDocument doc(512); // Specify the size of the document
    doc["xCalM"] = config.xCalM;
    doc["yCalM"] = config.yCalM;
    doc["xCalC"] = config.xCalC;
    doc["yCalC"] = config.yCalC;
    xtouch_filesystem_writeJson(SD, xtouch_paths_touch, doc);
}

void xtouch_resetTouchConfig()
{
    ConsoleInfo.println(F("[XTouch][FS] Resetting touch config"));
    xtouch_filesystem_deleteFile(SD, xtouch_paths_touch);
    delay(500);
    ESP.restart();
}

bool hasTouchConfig()
{
    ConsoleInfo.println(F("[XTouch][FS] Checking for touch config"));
    return xtouch_filesystem_exist(SD, xtouch_paths_touch);
}

void xtouch_touch_setup()
{
    // The GT911 on this panel reports coordinates directly in the panel's own
    // 800x480 space (the controller is factory-configured for that resolution,
    // as the vendor board definition also assumes), so touch needs no
    // calibration. The interactive two-crosshair calibration that this board
    // inherited from the 2.8" resistive panel is deliberately NOT run:
    //   - it blocks the boot waiting for two touch points, and
    //   - if it is interrupted it leaves a half-written /xtouch/touch.json
    //     behind, which then maps every touch to the wrong place. That is
    //     exactly the failure that made the UI look unresponsive.
    // The mapping helpers below are kept only so the rest of the code (and a
    // future calibration option) still compiles.
    ConsoleInfo.println(F("[XTouch][TOUCH] GT911 native 800x480 resolution - no calibration"));
}

#endif