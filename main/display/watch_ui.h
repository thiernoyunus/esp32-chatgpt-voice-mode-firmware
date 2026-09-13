#pragma once
#include "lvgl_display/lvgl_font.h"
#include <lvgl.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class WatchUi {
public:
    enum class Page { Home, Voice, Settings, Brightness, Volume, Wifi, Clock, About,
                      CodexSettings, Models, Keyboard, WifiSetup, Sleep, Reasoning,
                      Chats, Voices, Shapes, Colours };
    enum class Action { Refresh, OpenVoice, Mute, EndCall, Brightness, Volume,
                        ScanWifi, JoinWifi, SetupWifi, Models, SelectModel, Sleep, SelectReasoning,
                        SelectChat, TemporaryChat, SelectVoice, SelectShape, SelectColour, Captions };
    struct Info {
        int brightness = 75, volume = 65, battery = -1;
        // Seconds until the display sleeps. 0 means always on.
        int sleep_seconds = 60;
        bool charging = false, connected = false;
        // Chats are saved to Codex unless the user opts into temporary chats.
        bool temporary_chat = false;
        // The transcript strip on the call screen. On unless turned off.
        bool captions = true;
        // Which app slot is running, and whether the other one failed to start.
        // Shown under About so "what is it running?" can be answered by someone
        // holding the watch rather than reading a serial log.
        bool rolled_back = false;
        std::string slot;
        std::string network, version, model = "Default", reasoning = "Default", wifi_status, notice;
        // chat is the label of the chat the next call continues.
        std::string chat = "New chat";
        // Spoken voice for the next call. Empty means the ChatGPT default.
        std::string voice;
        int shape = 0, colour = 0;
        std::vector<std::string> networks, saved_networks, models, chats;
    };
    using Callback = std::function<void(Action, int, const std::string&, const std::string&)>;
    WatchUi(lv_obj_t* voice, std::shared_ptr<LvglFont> font, Callback callback);
    ~WatchUi();
    void Show(Page page);
    void SetInfo(const Info& info);
    void SetFont(std::shared_ptr<LvglFont> font);
    void Tick(const char* clock, const char* date);
    void SetCallActive(bool active);
    Page page() const { return page_; }
    // Reusable text input. FieldKind chooses submit label, max length, secret mode,
    // and the validator. For arbitrary future fields, use FieldKind::Generic.
    enum class FieldKind { Generic, Ssid, Password };
    void OpenKeyboard(FieldKind kind, const std::string& title, const std::string& initial,
                      std::function<void(const std::string&)> done);
private:
    lv_obj_t *voice_, *shell_, *column_ = nullptr, *clock_ = nullptr, *date_ = nullptr;
    lv_obj_t *value_ = nullptr, *field_ = nullptr, *keys_ = nullptr, *wifi_status_ = nullptr,
             *notice_ = nullptr;
    lv_obj_t *error_ = nullptr;
    std::shared_ptr<LvglFont> font_owner_;
    const lv_font_t* font_ = nullptr;
    Callback callback_;
    Info info_;
    Page page_ = Page::Home, model_return_ = Page::Settings, keyboard_return_ = Page::Wifi;
    bool call_active_ = false, uppercase_ = false;
    // The ChatGPT pages use the app-pixels language; everything else still uses
    // the navy watch look, so the two styles have to coexist while they land.
    bool dot_style_ = false;
    int key_page_ = 0;
    std::string time_ = "--:--", date_text_, join_ssid_;
    std::function<void(const std::string&)> keyboard_done_;
    FieldKind keyboard_kind_ = FieldKind::Generic;
    lv_obj_t* Box(lv_obj_t*, int x, int y, int w, int h, uint32_t color, int radius = 16);
    lv_obj_t* Label(lv_obj_t*, const char*, int width = 0);
    lv_obj_t* Button(lv_obj_t*, int x, int y, int w, int h, const char*,
                     const lv_image_dsc_t*, std::function<void()>, uint32_t color = 0x1A1A1A);
    void Header(const char*, Page back);
    lv_obj_t* Column();
    // `selected` marks the row as the current choice: accent rule, filled
    // background, accent value. It used to be inferred from the value reading
    // "On", which meant a switch whose value was "On" looked like a chosen
    // list item, and a picker that passed no value showed no choice at all.
    void Row(const char*, const char*, std::function<void()>, bool selected = false);
    void Slider(bool brightness);
    // The dot-matrix drawer paints into a canvas, so a value that changes is
    // redrawn rather than re-lettered the way an LVGL label would be.
    void ShowSliderValue(int percent);
    void DrawClockFace();
    void KeyboardKeys();
    void ChooseNetwork(const std::string&);
    void ShowKeyboardError(const char* text);
    void ClearKeyboardError();
    void UpdateNotice();
    void Emit(Action action, int value = 0, const std::string& text = {}, const std::string& secret = {});
};
