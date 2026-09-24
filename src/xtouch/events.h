#ifndef _XLCD_EVENTS
#define _XLCD_EVENTS

void xtouch_events_onResetDevice(lv_msg_t *m)
{
    ESP.restart();
}

void xtouch_events_onCloudSelect(lv_msg_t *m)
{
    // xtouch_cloud_pair_loop_exit = true;
}

void xtouch_events_onBackLight(lv_msg_t *m)
{
    int32_t value = lv_slider_get_value(ui_settingsBackLightPanelSlider);
    xTouchConfig.xTouchBacklightLevel = value;
    xtouch_screen_setBrightness(value);
}

void xtouch_events_onBackLightSet(lv_msg_t *m)
{
    int32_t value = lv_slider_get_value(ui_settingsBackLightPanelSlider);
    DynamicJsonDocument settings = xtouch_filesystem_readJson(SD, xtouch_paths_settings);
    settings["backlight"] = value;
    xTouchConfig.xTouchBacklightLevel = value;
    xtouch_filesystem_writeJson(SD, xtouch_paths_settings, settings);
    xtouch_screen_setBrightness(value);
}

void xtouch_events_onTFTTimerSet(lv_msg_t *m)
{
    int32_t value = lv_slider_get_value(ui_settingsTFTOFFSlider);
    xtouch_screen_setScreenTimer(value * 1000 * 60);

    DynamicJsonDocument settings = xtouch_filesystem_readJson(SD, xtouch_paths_settings);
    settings["tftOff"] = value;
    xTouchConfig.xTouchTFTOFFValue = value;
    xtouch_filesystem_writeJson(SD, xtouch_paths_settings, settings);
}

void xtouch_events_onTFTInvert(lv_msg_t *m)
{
    bool value = lv_obj_has_state(ui_settingsTFTInvertSwitch, LV_STATE_CHECKED);
    DynamicJsonDocument settings = xtouch_filesystem_readJson(SD, xtouch_paths_settings);
    settings["tftInvert"] = value ? true : false;
    xtouch_filesystem_writeJson(SD, xtouch_paths_settings, settings);
    xTouchConfig.xTouchTFTInvert = value;
    // 5-inch RGB panel has no TFT_eSPI invertColors() path.
}

void xtouch_events_onSettingsSave(lv_msg_t *m)
{
    xtouch_settings_save();
}

void xtouch_events_onTFTFlip(lv_msg_t *m)
{
    xtouch_screen_toggleTFTFlip();
}

void xtouch_events_onResetTouch(lv_msg_t *m)
{
    xtouch_resetTouchConfig();
}

void xtouch_events_onChamberTempSwitch(lv_msg_t *m)
{
    if (xTouchConfig.xTouchChamberSensorEnabled)
    {
        xtouch_chamber_timer_start();
    }
    else
    {
        xtouch_chamber_timer_stop();
    }
}

// Home Assistant switch from the settings screen. The link is started/stopped
// right away (no reboot), and the flag is persisted in config.json.
void xtouch_events_onHASwitch(lv_msg_t *m)
{
    // The "homeassistant" block lives in config.json - settings.json is written
    // wholesale by XTOUCH_SETTINGS_SAVE and does not carry it - so this is a
    // read-modify-write of the full config document to keep every other key.
    DynamicJsonDocument config = xtouch_filesystem_readJson(SD, xtouch_paths_config);
    config["homeassistant"]["enabled"] = xTouchConfig.xTouchHAEnabled;
    xtouch_filesystem_writeJson(SD, xtouch_paths_config, config);

    if (xTouchConfig.xTouchHAEnabled)
    {
        xtouch_ha_mqtt_start();
    }
    else
    {
        xtouch_ha_mqtt_stop();
    }
}

void xtouch_setupGlobalEvents()
{
    lv_msg_subscribe(XTOUCH_SETTINGS_RESET_DEVICE, (lv_msg_subscribe_cb_t)xtouch_events_onResetDevice, NULL);
    lv_msg_subscribe(XTOUCH_SETTINGS_BACKLIGHT, (lv_msg_subscribe_cb_t)xtouch_events_onBackLight, NULL);
    lv_msg_subscribe(XTOUCH_SETTINGS_BACKLIGHT_SET, (lv_msg_subscribe_cb_t)xtouch_events_onBackLightSet, NULL);
    lv_msg_subscribe(XTOUCH_SETTINGS_TFTOFF_SET, (lv_msg_subscribe_cb_t)xtouch_events_onTFTTimerSet, NULL);
    lv_msg_subscribe(XTOUCH_SETTINGS_TFT_INVERT, (lv_msg_subscribe_cb_t)xtouch_events_onTFTInvert, NULL);
    lv_msg_subscribe(XTOUCH_SETTINGS_SAVE, (lv_msg_subscribe_cb_t)xtouch_events_onSettingsSave, NULL);
    lv_msg_subscribe(XTOUCH_SETTINGS_CHAMBER_TEMP, (lv_msg_subscribe_cb_t)xtouch_events_onChamberTempSwitch, NULL);
#if defined(__XTOUCH_SCREEN_50__)
    // The Home Assistant switch only exists in the 5.0 settings screen.
    lv_msg_subscribe(XTOUCH_SETTINGS_HA, (lv_msg_subscribe_cb_t)xtouch_events_onHASwitch, NULL);
#endif
    lv_msg_subscribe(XTOUCH_SETTINGS_TFT_FLIP, (lv_msg_subscribe_cb_t)xtouch_events_onTFTFlip, NULL);
}

#endif