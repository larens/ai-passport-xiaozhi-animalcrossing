#include "folo_display.h"

#include "application.h"
#include "assets/lang_config.h"
#include "audio_codec.h"
#include "board.h"
#include "folo_images.h"
#include "lvgl_theme.h"

#include <material_symbols.h>
#include <algorithm>
#include <cstring>
#include <ctime>
#include <string>

namespace {
lv_obj_t* Label(lv_obj_t* parent, int x, int y, int w, int h) {
    auto label = lv_label_create(parent);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_size(label, w, h);
    lv_label_set_text(label, "");
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    return label;
}
bool FullTextState(DeviceState state) {
    return state == kDeviceStateWifiConfiguring || state == kDeviceStateActivating ||
           state == kDeviceStateUpgrading || state == kDeviceStateFatalError;
}

// Decode one UTF-8 code point starting at s[i]. Returns the byte length of the
// sequence (1..4) and advances nothing; callers add the returned length to i.
// Malformed leading bytes are treated as a single byte so we never loop forever.
int Utf8SequenceLen(const char* s, size_t i, size_t n) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    int len = 1;
    if ((c & 0x80) == 0x00) len = 1;        // 0xxxxxxx  ASCII
    else if ((c & 0xE0) == 0xC0) len = 2;   // 110xxxxx
    else if ((c & 0xF0) == 0xE0) len = 3;   // 1110xxxx  (most CJK here)
    else if ((c & 0xF8) == 0xF0) len = 4;   // 11110xxx
    // Clamp to the remaining bytes so a truncated tail never overruns.
    return static_cast<int>(std::min<size_t>(len, n - i));
}

// Hard-wrap text so every visual line holds at most `max_units` character
// widths, counting an ASCII/half-width byte as 1 and any multi-byte (CJK /
// full-width) code point as 2. Existing '\n' in the source are preserved and
// reset the per-line width counter. This gives deterministic wrapping (e.g.
// 10 Chinese characters per line at width 20) instead of relying on LVGL's
// pixel-based auto wrap, which drifts with mixed-width and punctuation runs.
std::string WrapByCharWidth(const char* text, int max_units) {
    std::string out;
    if (text == nullptr) return out;
    const size_t n = std::strlen(text);
    out.reserve(n + n / 8 + 4);  // room for inserted newlines
    int units = 0;
    for (size_t i = 0; i < n;) {
        if (text[i] == '\n') {
            out.push_back('\n');
            units = 0;
            ++i;
            continue;
        }
        const int seq = Utf8SequenceLen(text, i, n);
        const int width = seq == 1 ? 1 : 2;  // ASCII=1, multi-byte(CJK)=2
        if (units + width > max_units) {
            out.push_back('\n');
            units = 0;
        }
        out.append(text + i, seq);
        units += width;
        i += seq;
    }
    return out;
}
}

FoloDisplay::~FoloDisplay() {
    DisplayLockGuard lock(this);
    esp_timer_stop(notification_timer_);
    esp_timer_stop(preview_timer_);
    if (display_) lv_obj_clean(lv_display_get_screen_active(display_));
    // Base destructors must not delete children after deleting their display.
    network_label_ = status_label_ = notification_label_ = mute_label_ = battery_label_ = nullptr;
    chat_message_label_ = nullptr;
    preview_image_ = nullptr;
    preview_image_cached_.reset();
}

void FoloDisplay::SetupUI() {
    DisplayLockGuard lock(this);
    if (setup_ui_called_ || !display_) return;
    auto screen = lv_display_get_screen_active(display_);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(screen, 0, 0);

    // Full-screen background image (Isabelle / 西施惠). Created first and pushed
    // to the bottom so every label and the bubble render on top of it.
    background_image_ = lv_image_create(screen);
    lv_image_set_src(background_image_, &bg_shizue);
    lv_obj_set_pos(background_image_, 0, 0);
    lv_obj_move_background(background_image_);

    network_label_ = Label(screen, 8, 4, 24, 24);
    mute_label_ = Label(screen, 36, 4, 24, 24);
    clock_label_ = Label(screen, 65, 4, 84, 24);
    lv_obj_set_style_text_align(clock_label_, LV_TEXT_ALIGN_CENTER, 0);
    percentage_label_ = Label(screen, 153, 4, 55, 24);
    lv_obj_set_style_text_align(percentage_label_, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(percentage_label_, "--%");
    battery_label_ = Label(screen, 212, 4, 24, 24);
    status_label_ = Label(screen, 8, 30, 224, 24);
    notification_label_ = Label(screen, 8, 30, 224, 24);
    lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_align(notification_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    preview_image_ = lv_image_create(screen);
    lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);

    // Always-on speech bubble background behind the chat text.
    bubble_image_ = lv_image_create(screen);
    lv_image_set_src(bubble_image_, &bubble_bg);
    lv_obj_set_pos(bubble_image_, 4, 224);

    // Clipping window for the chat text. The label lives inside it and is
    // bottom-anchored so the newest lines stay visible while older lines
    // scroll up out of the window.
    text_clip_ = lv_obj_create(screen);
    lv_obj_remove_style_all(text_clip_);
    lv_obj_remove_flag(text_clip_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(text_clip_, 16, 238);
    lv_obj_set_size(text_clip_, 208, 74);

    chat_message_label_ = lv_label_create(text_clip_);
    lv_obj_set_pos(chat_message_label_, 0, 0);
    lv_obj_set_width(chat_message_label_, 208);
    lv_obj_set_height(chat_message_label_, LV_SIZE_CONTENT);
    lv_label_set_text(chat_message_label_, "");
    lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(chat_message_label_, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_line_space(chat_message_label_, 2, 0);

    menu_label_ = Label(screen, 12, 64, 216, 248);
    lv_label_set_long_mode(menu_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(menu_label_, 8, 0);
    lv_obj_add_flag(menu_label_, LV_OBJ_FLAG_HIDDEN);
    volume_bar_ = lv_bar_create(screen);
    lv_obj_set_pos(volume_bar_, 24, 240);
    lv_obj_set_size(volume_bar_, 192, 4);
    lv_bar_set_range(volume_bar_, 0, 100);
    lv_obj_set_style_bg_color(volume_bar_, lv_color_hex(0xDE6E91), LV_PART_INDICATOR);
    lv_obj_add_flag(volume_bar_, LV_OBJ_FLAG_HIDDEN);

    Display::SetupUI();
    SetTheme(current_theme_);
    idle_.Activity(esp_timer_get_time());
    RefreshContent();
}

void FoloDisplay::SetTheme(Theme* theme) {
    auto typed = dynamic_cast<LvglTheme*>(theme);
    if (!typed) return;
    DisplayLockGuard lock(this);
    Display::SetTheme(theme);
    if (!setup_ui_called_) return;
    const bool dark = theme->name() == "dark";
    auto screen = lv_display_get_screen_active(display_);
    lv_obj_set_style_bg_color(screen, lv_color_hex(dark ? 0x202027 : 0xF3F5F5), 0);
    lv_obj_set_style_text_color(screen, lv_color_hex(dark ? 0xF6F2F5 : 0x30323A), 0);
    lv_obj_set_style_text_font(screen, typed->text_font()->font(), 0);
    for (auto label : {network_label_, mute_label_, battery_label_}) {
        lv_obj_set_style_text_font(label, typed->icon_font()->font(), 0);
    }
    lv_obj_set_style_text_color(status_label_, lv_color_hex(dark ? 0x8AD4C8 : 0x28776D), 0);
    lv_obj_set_style_text_color(notification_label_, lv_color_hex(dark ? 0xF3A5BC : 0xA33F63), 0);
    // Chat text sits on the light speech bubble, so force near-black for
    // maximum contrast regardless of the background image behind it.
    if (chat_message_label_) {
        lv_obj_set_style_text_color(chat_message_label_, lv_color_hex(0x1A1A1A), 0);
    }
}

void FoloDisplay::Activity() {
    idle_.Activity(esp_timer_get_time());
    if (panel_asleep_) {
        ApplySleep(false);
    }
}

void FoloDisplay::ApplySleep(bool asleep) {
    if (asleep == panel_asleep_) return;
    panel_asleep_ = asleep;
    auto backlight = Board::GetInstance().GetBacklight();
    if (asleep) {
        menu_open_ = false;
        lv_obj_add_flag(menu_label_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(volume_bar_, LV_OBJ_FLAG_HIDDEN);
        volume_until_ = 0;
        backlight->SetBrightness(0);  // Do not overwrite saved brightness.
    } else {
        backlight->RestoreBrightness();
    }
    RefreshContent();
}

bool FoloDisplay::WakeFromKey() {
    DisplayLockGuard lock(this);
    if (!setup_ui_called_) return true;
    bool consumed = panel_asleep_;
    Activity();
    return consumed;
}

void FoloDisplay::SetStatus(const char* status) {
    if (!setup_ui_called_) return;
    DisplayLockGuard lock(this);
    const auto state = Application::GetInstance().GetDeviceState();
    if (state != state_) {
        state_ = state;
        thinking_ = false;
        menu_open_ = false;
        lv_obj_add_flag(menu_label_, LV_OBJ_FLAG_HIDDEN);
        Activity();
    }
    LvglDisplay::SetStatus(status);
    RefreshContent();
}

void FoloDisplay::SetEmotion(const char* emotion) {
    DisplayLockGuard lock(this);
    if (!setup_ui_called_) return;
    // The static Isabelle background replaces the animated portrait, so emotions
    // no longer drive a sprite. Kept as a no-op to satisfy the Display interface.
    (void)emotion;
}

void FoloDisplay::SetChatMessage(const char* role, const char* content) {
    DisplayLockGuard lock(this);
    if (!setup_ui_called_) return;
    message_ = content ? content : "";
    if (!message_.empty()) {
        Activity();
        if (role && !strcmp(role, "user")) {
            thinking_ = true;
            LvglDisplay::SetStatus("思考中");
        }
        else if (role && !strcmp(role, "assistant")) thinking_ = false;
    }
    RefreshContent();
}

void FoloDisplay::ClearChatMessages() {
    DisplayLockGuard lock(this);
    message_.clear();
    thinking_ = false;
    if (setup_ui_called_) RefreshContent();
}

void FoloDisplay::RefreshContent() {
    const bool full_text = FullTextState(state_);
    if (full_text || menu_open_ || !preview_image_cached_)
        lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);

    // The bubble is shown behind ordinary chat text only. Full-screen text
    // states (provisioning/activation/upgrade), the menu and image preview all
    // hide it so the text can use the whole screen.
    const bool hide_bubble = full_text || menu_open_ || preview_image_cached_ != nullptr;
    if (hide_bubble) lv_obj_add_flag(bubble_image_, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(bubble_image_, LV_OBJ_FLAG_HIDDEN);

    const bool hide_text = menu_open_ || (hide_subtitle_ && !full_text);
    if (hide_text) lv_obj_add_flag(text_clip_, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(text_clip_, LV_OBJ_FLAG_HIDDEN);

    // Full-screen text uses a large clip window; ordinary chat text stays inside
    // the bubble.
    if (full_text) {
        lv_obj_set_pos(text_clip_, 12, 64);
        lv_obj_set_size(text_clip_, 216, 248);
        lv_obj_set_width(chat_message_label_, 216);
    } else {
        lv_obj_set_pos(text_clip_, 16, 238);
        lv_obj_set_size(text_clip_, 208, 74);
        lv_obj_set_width(chat_message_label_, 208);
    }

    const char* content = message_.c_str();
    if (message_.empty()) {
        if (state_ == kDeviceStateListening) content = "我在听，你慢慢说";
        else if (state_ == kDeviceStateConnecting) content = "正在连接，请稍等";
        else if (state_ == kDeviceStateSpeaking) content = "我正在回答";
        else if (low_battery_) content = "电量较低，请及时充电";
        else content = "你好，我是西施惠\n今天想聊点什么？";
    }
    // Ordinary chat text is hard-wrapped to a fixed character width (20 units =
    // 10 Chinese chars per line) for tidy, deterministic line breaks inside the
    // narrow bubble. Full-screen text (provisioning/activation/upgrade) keeps
    // LVGL's pixel wrap so it can use the whole width.
    std::string wrapped;
    const char* display_text = content;
    if (!full_text) {
        wrapped = WrapByCharWidth(content, 20);
        display_text = wrapped.c_str();
    }
    if (strcmp(lv_label_get_text(chat_message_label_), display_text))
        lv_label_set_text(chat_message_label_, display_text);
    LayoutChatText(full_text);
}

void FoloDisplay::LayoutChatText(bool full_text) {
    // Force the label to recompute its content height for the current width,
    // then bottom-anchor it inside the clip window. When the text is taller
    // than the window the top lines scroll up out of view; when it is shorter
    // it is vertically centered for a tidy look.
    lv_obj_update_layout(chat_message_label_);
    const int32_t window_h = lv_obj_get_height(text_clip_);
    const int32_t text_h = lv_obj_get_height(chat_message_label_);
    int32_t y;
    if (text_h >= window_h) {
        y = window_h - text_h;  // negative: oldest lines clipped at the top
    } else {
        y = (window_h - text_h) / 2;  // fits: center vertically
    }
    lv_obj_set_y(chat_message_label_, y);
}

void FoloDisplay::UpdateStatusBar(bool update_all) {
    if (!setup_ui_called_) return;
    auto& board = Board::GetInstance();
    auto& app = Application::GetInstance();
    int level = 0;
    bool charging = false, discharging = false;
    const bool valid = board.GetBatteryLevel(level, charging, discharging);
    const int volume = board.GetAudioCodec()->output_volume();
    const auto state = app.GetDeviceState();
    const bool voice = state == kDeviceStateListening && app.IsVoiceDetected();
    const bool update_network = update_all || status_ticks_++ % 10 == 0;
    const char* network = update_network ? board.GetNetworkStateIcon() : nullptr;
    DisplayLockGuard lock(this);
    if (state != state_) {
        state_ = state;
        thinking_ = false;
        menu_open_ = false;
        lv_obj_add_flag(menu_label_, LV_OBJ_FLAG_HIDDEN);
        Activity();
    }
    const auto now = esp_timer_get_time();
    ApplySleep(idle_.Tick(now, state, voice));
    if (panel_asleep_) return;
    lv_label_set_text(mute_label_, volume == 0 ? MATERIAL_SYMBOLS_VOLUME_OFF : "");
    if (network) lv_label_set_text(network_label_, network);
    time_t seconds = time(nullptr);
    struct tm local = {};
    localtime_r(&seconds, &local);
    char clock[8] = "--:--";
    if (local.tm_year >= 125) strftime(clock, sizeof(clock), "%H:%M", &local);
    lv_label_set_text(clock_label_, clock);
    if (valid) {
        lv_label_set_text_fmt(percentage_label_, "%d%%", level);
        lv_label_set_text(battery_label_, level <= 15 ? MATERIAL_SYMBOLS_BATTERY_ANDROID_0 :
                          level < 50 ? MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_3 :
                          MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_FULL);
    } else {
        lv_label_set_text(percentage_label_, "--%");
        lv_label_set_text(battery_label_, "");
    }
    // Hysteresis avoids repeated alerts when the gauge oscillates near 15%.
    if (valid && level <= 15 && !low_battery_) {
        low_battery_ = true;
        if (!panel_asleep_ && !menu_open_) ShowNotification("电量较低，请及时充电", 5000);
    } else if (!valid || level >= 20) low_battery_ = false;
    lv_obj_set_style_text_color(percentage_label_,
        lv_color_hex(!valid ? 0x888888 : low_battery_ ? 0xD95270 : 0x379B87), 0);
    if (volume_until_ && now >= volume_until_) {
        volume_until_ = 0;
        lv_obj_add_flag(volume_bar_, LV_OBJ_FLAG_HIDDEN);
        RefreshContent();
    }
    if (!volume_until_) RefreshContent();
}

void FoloDisplay::ShowVolume(int volume) {
    DisplayLockGuard lock(this);
    if (!setup_ui_called_) return;
    Activity();
    ShowNotification((Lang::Strings::VOLUME + std::to_string(volume) + "%").c_str(), 2500);
    if (FullTextState(state_)) return;
    volume_until_ = esp_timer_get_time() + 2500000;
    lv_bar_set_value(volume_bar_, std::clamp(volume, 0, 100), LV_ANIM_OFF);
    lv_obj_remove_flag(volume_bar_, LV_OBJ_FLAG_HIDDEN);
}

void FoloDisplay::ShowMenu(const char* text) {
    DisplayLockGuard lock(this);
    if (!setup_ui_called_) return;
    Activity();
    menu_open_ = true;
    volume_until_ = 0;
    lv_obj_add_flag(volume_bar_, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(menu_label_, text);
    lv_obj_remove_flag(menu_label_, LV_OBJ_FLAG_HIDDEN);
    RefreshContent();
}

void FoloDisplay::CloseMenu() {
    DisplayLockGuard lock(this);
    if (!setup_ui_called_) return;
    menu_open_ = false;
    lv_obj_add_flag(menu_label_, LV_OBJ_FLAG_HIDDEN);
    RefreshContent();
}

void FoloDisplay::SetPowerSaveMode(bool on) {
    DisplayLockGuard lock(this);
    if (!setup_ui_called_) return;
    if (!on) Activity();
    // Only the inactivity policy turns off the screen; network power saving must not.
}

void FoloDisplay::NotifyUserActivity() {
    DisplayLockGuard lock(this);
    if (setup_ui_called_) Activity();
}

void FoloDisplay::SetPreviewImage(std::unique_ptr<LvglImage> image) {
    DisplayLockGuard lock(this);
    if (!setup_ui_called_) return;
    if (image) {
        auto dsc = image->image_dsc();
        if (!dsc || !dsc->header.w || !dsc->header.h) return;
        esp_timer_stop(preview_timer_);
        const int scale = std::max(1, std::min(192 * 256 / int(dsc->header.w),
                                              180 * 256 / int(dsc->header.h)));
        lv_image_set_src(preview_image_, dsc);
        lv_image_set_pivot(preview_image_, 0, 0);
        lv_image_set_scale(preview_image_, scale);
        lv_obj_set_pos(preview_image_, (240 - int(dsc->header.w) * scale / 256) / 2,
                       54 + (188 - int(dsc->header.h) * scale / 256) / 2);
        preview_image_cached_ = std::move(image);
        Activity();
        ESP_ERROR_CHECK(esp_timer_start_once(preview_timer_, PREVIEW_IMAGE_DURATION_MS * 1000));
    } else {
        esp_timer_stop(preview_timer_);
        lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
        lv_image_set_src(preview_image_, nullptr);
        preview_image_cached_.reset();
    }
    RefreshContent();
}
