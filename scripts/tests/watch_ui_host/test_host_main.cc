#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <functional>
#include <memory>

extern "C" {
#include "display_driver.h"
#include "screenshot.h"
#include "display/lv_display_private.h"
}

#include "watch_ui.h"
#include "watch_icons.h"
#include "lvgl_display/lvgl_font.h"

static int g_pass = 0, g_fail = 0, g_shot = 0;
static std::vector<std::string> g_log;
static WatchUi::Action g_last_action = WatchUi::Action::Refresh;
static int g_last_action_value = -1;
static std::string g_last_action_text;
static std::string g_last_action_secret;

static void log_action(const char* msg) {
    g_log.push_back(msg);
    printf("  %s\n", msg);
}

static void check(bool cond, const char* label) {
    if (cond) { g_pass++; printf("  PASS: %s\n", label); }
    else      { g_fail++; printf("  FAIL: %s\n", label); }
}

static void snap(const char* tag) {
    char path[256];
    snprintf(path, sizeof(path), "/tmp/watch-%02d-%s.png", g_shot++, tag);
    uint8_t* fb = headless_display_get_framebuffer();
    uint32_t w = headless_display_get_width(), h = headless_display_get_height();
    screenshot_save_png(path, fb, w, h);
    printf("  [screenshot] %s\n", path);
}

static void send_click(lv_obj_t* obj) {
    if (obj) lv_obj_send_event(obj, LV_EVENT_CLICKED, NULL);
}

static void tick_lv(int ms = 50) {
    lv_timer_handler();
    lv_tick_inc(ms);
    lv_timer_handler();
}

static int count_children_recursive(lv_obj_t* obj) {
    if (!obj) return 0;
    int count = 1;
    int32_t n = lv_obj_get_child_count(obj);
    for (int32_t i = 0; i < n; i++)
        count += count_children_recursive(lv_obj_get_child(obj, i));
    return count;
}

static lv_obj_t* find_label(lv_obj_t* parent, const char* text) {
    if (!parent) return nullptr;
    if (lv_obj_check_type(parent, &lv_label_class)) {
        const char* t = lv_label_get_text(parent);
        if (t && strcmp(t, text) == 0) return parent;
    }
    int32_t n = lv_obj_get_child_count(parent);
    for (int32_t i = 0; i < n; i++) {
        lv_obj_t* found = find_label(lv_obj_get_child(parent, i), text);
        if (found) return found;
    }
    return nullptr;
}

static lv_obj_t* find_slider(lv_obj_t* parent) {
    if (!parent) return nullptr;
    if (lv_obj_check_type(parent, &lv_slider_class)) return parent;
    int32_t n = lv_obj_get_child_count(parent);
    for (int32_t i = 0; i < n; i++) {
        lv_obj_t* found = find_slider(lv_obj_get_child(parent, i));
        if (found) return found;
    }
    return nullptr;
}

static lv_obj_t* find_textarea(lv_obj_t* parent) {
    if (!parent) return nullptr;
    if (lv_obj_check_type(parent, &lv_textarea_class)) return parent;
    int32_t n = lv_obj_get_child_count(parent);
    for (int32_t i = 0; i < n; i++) {
        lv_obj_t* found = find_textarea(lv_obj_get_child(parent, i));
        if (found) return found;
    }
    return nullptr;
}

static bool click_label(lv_obj_t* parent, const char* text) {
    lv_obj_t* label = find_label(parent, text);
    if (!label) return false;
    send_click(lv_obj_get_parent(label));
    return true;
}

static uint32_t screen_count() {
    lv_display_t* display = lv_display_get_default();
    return display ? display->screen_cnt : 0;
}

static lv_obj_t* make_voice_placeholder(lv_obj_t* parent) {
    lv_obj_t* obj = lv_obj_create(parent);
    lv_obj_set_size(obj, 1, 1);
    return obj;
}

int main() {
    printf("=== Watch UI Host Test ===\n");
    lv_init();
    headless_display_init(360, 360);
    lv_tick_inc(100);
    lv_timer_handler();

    lv_obj_t* scr = lv_screen_active();
    lv_obj_t* voice = make_voice_placeholder(scr);

    auto cb = [](WatchUi::Action a, int v, const std::string& t, const std::string& s) {
        g_last_action = a;
        g_last_action_value = v;
        g_last_action_text = t;
        g_last_action_secret = s;
        const char* name = "Other";
        switch(a) {
            case WatchUi::Action::Refresh: name="Refresh"; break;
            case WatchUi::Action::Brightness: name="Brightness"; break;
            case WatchUi::Action::Volume: name="Volume"; break;
            case WatchUi::Action::ScanWifi: name="ScanWifi"; break;
            case WatchUi::Action::JoinWifi: name="JoinWifi"; break;
            case WatchUi::Action::Models: name="Models"; break;
            case WatchUi::Action::SelectModel: name="SelectModel"; break;
            case WatchUi::Action::Sleep: name="Sleep"; break;
            default: break;
        }
        printf("  ACTION: %s val=%d\n", name, v);
    };

    {
    WatchUi ui(voice, std::make_shared<LvglBuiltInFont>(LV_FONT_DEFAULT), cb);
    WatchUi::Info info;
    info.brightness = 75; info.volume = 65; info.battery = 85;
    info.charging = false; info.connected = true;
    info.network = "HomeWifi"; info.version = "1.0.0";
    info.model = "gpt-4o"; info.sleep_seconds = 60;
    info.networks = {"HomeWifi", "OfficeNet", "GuestNet"};
    info.saved_networks = {"HomeWifi"};
    info.models = {"gpt-4o", "gpt-4o-mini", "o1-mini"};
    // Reasoning and Chats render a picker only when there is something to pick.
    info.reasoning = "Medium";
    // The recovery case: the update failed and the watch is on the old one.
    info.rolled_back = true;
    info.slot = "ota_1";
    info.chats = {"Trip planning", "Firmware notes"};
    ui.SetInfo(info);
    const uint32_t base_screen_count = screen_count();

    // Render all pages
    struct PT { WatchUi::Page p; const char* n; };
    PT pages[] = {
        {WatchUi::Page::Home, "home"},
        {WatchUi::Page::Settings, "settings"},
        {WatchUi::Page::Brightness, "brightness"},
        {WatchUi::Page::Volume, "volume"},
        {WatchUi::Page::Clock, "clock"},
        {WatchUi::Page::Wifi, "wifi"},
        {WatchUi::Page::Models, "models"},
        {WatchUi::Page::About, "about"},
        {WatchUi::Page::CodexSettings, "chatgpt"},
        {WatchUi::Page::Shapes, "shapes"},
        {WatchUi::Page::Colours, "colours"},
        {WatchUi::Page::Sleep, "sleep"},
        {WatchUi::Page::Reasoning, "reasoning"},
        {WatchUi::Page::Chats, "chats"},
        {WatchUi::Page::WifiSetup, "wifisetup"},
    };
    for (auto& pt : pages) { ui.Show(pt.p); tick_lv(); snap(pt.n); }
    check(screen_count() == base_screen_count, "Pages keep LVGL screen count");

    // The notice banner floats over whichever page is open rather than being a
    // page of its own, so it is rendered here over Settings and then cleared -
    // anything left on screen would show up in the nav-stability count below.
    {
        const char* kNotice = "Update didn't finish. Running your last working version.";
        ui.Show(WatchUi::Page::Settings); tick_lv();
        const int bare = count_children_recursive(lv_screen_active());
        info.notice = kNotice; ui.SetInfo(info); tick_lv();
        check(count_children_recursive(lv_screen_active()) > bare, "Notice banner appears");
        snap("notice");
        info.notice.clear(); ui.SetInfo(info); tick_lv();
        check(count_children_recursive(lv_screen_active()) == bare, "Notice banner clears");
    }

    ui.Show(WatchUi::Page::Brightness); tick_lv();
    // Rows in the app-pixels pages are canvases, not labels, so they cannot be
    // found by their text any more. Click them by position instead: from the
    // start for the fixed lists, from the end for Wi-Fi, where the number of
    // networks above "Other network" depends on what this harness set up.
    auto row_at = [&ui](int index, bool from_end) -> lv_obj_t* {
        lv_obj_t* screen = lv_screen_active();
        lv_obj_t* shell = nullptr;
        for (uint32_t i = 0; i < lv_obj_get_child_cnt(screen); ++i) {
            lv_obj_t* child = lv_obj_get_child(screen, i);
            if (lv_obj_get_width(child) == 360 && lv_obj_get_height(child) == 360) shell = child;
        }
        if (shell == nullptr) return nullptr;
        const int y = ui.page() == WatchUi::Page::Brightness ? 244
                    : 100;
        lv_obj_t* container = nullptr;
        for (uint32_t i = 0; i < lv_obj_get_child_cnt(shell); ++i) {
            lv_obj_t* child = lv_obj_get_child(shell, i);
            if (lv_obj_get_x(child) == 62 && lv_obj_get_y(child) == y
                    && lv_obj_get_width(child) == 236) {
                container = child;
                break;
            }
        }
        if (container == nullptr) return nullptr;
        const uint32_t n = lv_obj_get_child_cnt(container);
        if (n == 0) return nullptr;
        const uint32_t at = from_end ? n - 1 - static_cast<uint32_t>(index)
                                     : static_cast<uint32_t>(index);
        return at < n ? lv_obj_get_child(container, at) : nullptr;
    };
    auto click_row = [&](int index, bool from_end) {
        lv_obj_t* row = row_at(index, from_end);
        if (row == nullptr) return false;
        send_click(row);
        return true;
    };
    if (click_row(0, false)) {
        tick_lv();
        check(ui.page() == WatchUi::Page::Sleep, "Sleep timeout row opens Sleep");
        if (click_row(1, false)) {
            tick_lv();
            check(ui.page() == WatchUi::Page::Brightness
                      && g_last_action == WatchUi::Action::Sleep
                      && g_last_action_value == 30,
                  "Sleep selection emits 30 seconds");
        } else {
            check(false, "Sleep 30 seconds row found");
        }
    } else {
        check(false, "Brightness Sleep timeout row found");
    }

    // The ChatGPT pages wear the app-pixels language: black, one accent, the
    // title on the same line as the back arrow, and only the current row filled.
    ui.Show(WatchUi::Page::CodexSettings); tick_lv(); snap("chatgpt");
    ui.Show(WatchUi::Page::Voices); tick_lv(); snap("chatgpt_voices");
    // A read-only About row still receives a real LVGL click. It must not
    ui.Show(WatchUi::Page::About); tick_lv();
   // call an empty std::function or crash with std::bad_function_call.
    lv_obj_t* firmware = row_at(0, false);
    if (firmware) {
        send_click(firmware); tick_lv();
        check(ui.page() == WatchUi::Page::About, "Readonly About row click is safe");
    } else {
        check(false, "Readonly About row found");
    }

    lv_obj_t* about_shell = firmware ? lv_obj_get_parent(lv_obj_get_parent(firmware)) : nullptr;
    lv_obj_t* about_back = nullptr;
    for (uint32_t i = 0; about_shell && i < lv_obj_get_child_cnt(about_shell); ++i) {
        lv_obj_t* child = lv_obj_get_child(about_shell, i);
        if (lv_obj_get_width(child) == 44 && lv_obj_get_height(child) == 44
                && lv_obj_has_flag(child, LV_OBJ_FLAG_CLICKABLE)) {
            about_back = child;
            break;
        }
    }
    if (about_back) {
        send_click(about_back); tick_lv();
        check(ui.page() == WatchUi::Page::Settings, "About Back returns to Settings");
    } else {
        check(false, "About Back button found");
    }

    // Keyboard test
    printf("\nKeyboard tests...\n");
    ui.Show(WatchUi::Page::Wifi); tick_lv();
    ui.OpenKeyboard(WatchUi::FieldKind::Ssid, "WiFi Name", "", nullptr); tick_lv();
    snap("keyboard_open");
    lv_obj_t* field = find_textarea(lv_screen_active());
    check(field != nullptr, "Keyboard textarea found");
    const int keyboard_children = count_children_recursive(lv_screen_active());
    WatchUi::Info notice_info = info;
    notice_info.notice = "Saved";
    ui.SetInfo(notice_info); tick_lv();
    check(count_children_recursive(lv_screen_active()) == keyboard_children,
          "Notice stays out of the keyboard action area");
    notice_info.notice.clear();
    ui.SetInfo(notice_info); tick_lv();
    check(ui.page() == WatchUi::Page::Keyboard && find_textarea(lv_screen_active()) == field,
          "Notice expiry preserves keyboard");

    lv_obj_t* ka = find_label(lv_screen_active(), "a");
    if (ka) { send_click(lv_obj_get_parent(ka)); tick_lv(); log_action("typed a"); }
    else { check(false, "Keyboard a key found"); }

    lv_obj_t* kb = find_label(lv_screen_active(), "b");
    if (kb) { send_click(lv_obj_get_parent(kb)); tick_lv(); log_action("typed b"); }
    else { check(false, "Keyboard b key found"); }
    check(field && strcmp(lv_textarea_get_text(field), "ab") == 0, "Keyboard chars entered");
    snap("keyboard_ab");

    // Uppercase
    lv_obj_t* aa = find_label(lv_screen_active(), "Aa");
    if (aa) {
        send_click(lv_obj_get_parent(aa)); tick_lv(); snap("keyboard_upper"); log_action("uppercase toggle");
        lv_obj_t* kC = find_label(lv_screen_active(), "C");
        if (kC) { send_click(lv_obj_get_parent(kC)); tick_lv(); log_action("typed C"); }
        else { check(false, "Uppercase C key found"); }
        check(field && strcmp(lv_textarea_get_text(field), "abC") == 0, "Caps key enters uppercase");
    } else { check(false, "Caps key found"); }

    // Symbols
    lv_obj_t* k123 = find_label(lv_screen_active(), "123");
    if (k123) {
        send_click(lv_obj_get_parent(k123)); tick_lv(); snap("keyboard_sym"); log_action("symbols toggle");
        lv_obj_t* kat = find_label(lv_screen_active(), "@");
        if (kat) { send_click(lv_obj_get_parent(kat)); tick_lv(); log_action("typed @"); }
        else { check(false, "Symbol @ key found"); }
        check(field && strcmp(lv_textarea_get_text(field), "abC@") == 0, "Symbol key enters @");
        lv_obj_t* kcycle = find_label(lv_screen_active(), "#+=");
        if (kcycle) { send_click(lv_obj_get_parent(kcycle)); tick_lv(); }
        lv_obj_t* kabc = find_label(lv_screen_active(), "abc");
        if (kabc) { send_click(lv_obj_get_parent(kabc)); tick_lv(); log_action("back to abc"); }
        else if (!kcycle) { check(false, "Keyboard abc key found"); }
    } else { check(false, "Keyboard symbols key found"); }

    // Space
    lv_obj_t* ks = find_label(lv_screen_active(), "space");
    if (!ks) ks = find_label(lv_screen_active(), " ");
    if (ks) { send_click(lv_obj_get_parent(ks)); tick_lv(); log_action("space"); }
    else { check(false, "Keyboard space key found"); }
    check(field && strcmp(lv_textarea_get_text(field), "abC@ ") == 0, "Space key enters space");

    // Delete
    lv_obj_t* kd = find_label(lv_screen_active(), "Del");
    if (!kd) kd = find_label(lv_screen_active(), LV_SYMBOL_BACKSPACE);
    if (kd) { send_click(lv_obj_get_parent(kd)); tick_lv(); log_action("delete"); }
    else { check(false, "Keyboard delete key found"); }
    check(field && strcmp(lv_textarea_get_text(field), "abC@") == 0, "Delete removes one character");

    snap("keyboard_typed");

    // Cancel
    lv_obj_t* kc = find_label(lv_screen_active(), "Cancel");
    if (kc) {
        send_click(lv_obj_get_parent(kc)); tick_lv();
        check(ui.page() == WatchUi::Page::Wifi, "Cancel returns to Wifi");
        snap("keyboard_cancel");
    } else { log_action("WARN: no Cancel"); }

    // Password submit
    bool submitted = false;
    ui.OpenKeyboard(WatchUi::FieldKind::Password, "Password", "", [&](const std::string& val) {
        submitted = true;
        printf("  keyboard submitted: %s\n", val.c_str());
    }); tick_lv();
    snap("keyboard_pass");
    lv_obj_t* kj = find_label(lv_screen_active(), "Join");
    if (!kj) kj = find_label(lv_screen_active(), "Submit");
    if (!kj) kj = find_label(lv_screen_active(), "OK");
    if (kj) {
        send_click(lv_obj_get_parent(kj)); tick_lv();
        check(submitted, "Submit callback fired");
        log_action("submit clicked");
    } else { check(false, "Submit button found"); }

    printf("\nManual Wi-Fi flow...\n");
    auto open_manual_ssid = [&]() {
        ui.Show(WatchUi::Page::Wifi); tick_lv();
        return click_row(2, true);
    };
    if (open_manual_ssid()) {
        tick_lv();
        check(ui.page() == WatchUi::Page::Keyboard, "Other network opens SSID keyboard");
        lv_obj_t* manual_field = find_textarea(lv_screen_active());
        check(manual_field != nullptr, "Manual SSID textarea found");
        check(click_label(lv_screen_active(), "a"), "Manual SSID key found");
        tick_lv();
        check(manual_field && strcmp(lv_textarea_get_text(manual_field), "a") == 0,
              "Manual SSID entered");
        check(click_label(lv_screen_active(), "Next"), "SSID Next button found");
        tick_lv();
        check(ui.page() == WatchUi::Page::Keyboard
                  && find_label(lv_screen_active(), "Wi-Fi password") != nullptr,
              "SSID Next opens password keyboard");
        check(click_label(lv_screen_active(), "Cancel"), "Password Cancel button found");
        tick_lv();
        check(ui.page() == WatchUi::Page::Wifi, "Password Cancel returns to Wifi");
    } else {
        check(false, "Other network row found");
    }

    if (open_manual_ssid()) {
        tick_lv();
        check(click_label(lv_screen_active(), "a"), "Second manual SSID key found");
        tick_lv();
        check(click_label(lv_screen_active(), "Next"), "Second SSID Next button found");
        tick_lv();
        check(click_label(lv_screen_active(), "Join"), "Password Join button found");
        tick_lv();
        check(ui.page() == WatchUi::Page::Wifi
                  && g_last_action == WatchUi::Action::JoinWifi
                  && g_last_action_value == 0
                  && g_last_action_text == "a"
                  && g_last_action_secret.empty(),
              "Manual SSID join emits and returns to Wifi");
    } else {
        check(false, "Other network row found for submit");
    }

    // Slider
    printf("\nSlider test...\n");
    ui.Show(WatchUi::Page::Brightness); tick_lv();
    lv_obj_t* sl = find_slider(lv_screen_active());
    if (sl) {
        lv_slider_set_value(sl, 50, LV_ANIM_OFF);
        lv_obj_send_event(sl, LV_EVENT_VALUE_CHANGED, NULL);
        lv_obj_send_event(sl, LV_EVENT_RELEASED, NULL);
        tick_lv(); snap("slider_50");
        log_action("slider set 50");
        check(g_last_action == WatchUi::Action::Brightness && g_last_action_value == 50,
              "Slider callback reports 50");
    } else { check(false, "Slider found"); }

    // Nav stability
    printf("\nNav stability...\n");
    ui.Show(WatchUi::Page::Home); tick_lv();
    int c1 = count_children_recursive(lv_screen_active());
    uint32_t s1 = screen_count();
    for (int i = 0; i < 20; i++) {
        ui.Show(WatchUi::Page::Settings); lv_timer_handler();
        ui.Show(WatchUi::Page::Home); lv_timer_handler();
    }
    tick_lv();
    int c2 = count_children_recursive(lv_screen_active());
    check(c1 == c2, "Widget count stable 20 cycles");
    uint32_t s2 = screen_count();
    check(s1 == s2 && s2 == base_screen_count, "LVGL screen count stable 20 cycles");
    printf("  count: %d -> %d\n", c1, c2);
    printf("  screens: %u -> %u (baseline %u)\n", s1, s2, base_screen_count);
    snap("nav_stable");

    printf("\n=== Results ===\n");
    printf("  PASS: %d\n", g_pass);
    printf("  FAIL: %d\n", g_fail);
    printf("  Screenshots: %d\n", g_shot);
    }
    headless_display_deinit();
    return g_fail > 0 ? 1 : 0;
}
