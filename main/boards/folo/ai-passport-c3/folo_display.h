#pragma once

#include "display/lcd_display.h"
#include "device_state.h"
#include "screen_idle.h"

class FoloDisplay : public SpiLcdDisplay {
public:
    using SpiLcdDisplay::SpiLcdDisplay;
    ~FoloDisplay() override;
    void SetupUI() override;
    void SetStatus(const char* status) override;
    void SetEmotion(const char* emotion) override;
    void SetChatMessage(const char* role, const char* content) override;
    void ClearChatMessages() override;
    void SetTheme(Theme* theme) override;
    void UpdateStatusBar(bool update_all = false) override;
    void SetPowerSaveMode(bool on) override;
    void NotifyUserActivity() override;
    void SetPreviewImage(std::unique_ptr<LvglImage> image) override;

    // True means the key was consumed as a wake-only action.
    bool WakeFromKey();
    void ShowVolume(int volume);
    void ShowMenu(const char* text);
    void CloseMenu();
    bool IsMenuOpen() { DisplayLockGuard lock(this); return menu_open_; }

private:
    void Activity();
    void ApplySleep(bool asleep);
    void RefreshContent();
    // Bottom-anchored multi-line chat text: newest lines stay visible, older
    // lines scroll up out of the clipping window.
    void LayoutChatText(bool full_text);
    lv_obj_t* background_image_ = nullptr;
    lv_obj_t* bubble_image_ = nullptr;
    lv_obj_t* text_clip_ = nullptr;
    lv_obj_t* clock_label_ = nullptr;
    lv_obj_t* percentage_label_ = nullptr;
    lv_obj_t* menu_label_ = nullptr;
    lv_obj_t* volume_bar_ = nullptr;
    ScreenIdle idle_;
    DeviceState state_ = kDeviceStateUnknown;
    unsigned status_ticks_ = 0;
    bool panel_asleep_ = false;
    bool menu_open_ = false;
    bool thinking_ = false;
    bool low_battery_ = false;
    int64_t volume_until_ = 0;
    std::string message_;
};
