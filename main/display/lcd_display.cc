#include "lcd_display.h"
#include "bloub/bloub_shapes.h"   /* the character */
#include "voice_character.h"        /* the palette and counts it is worn in */
#include "bloub/bloub_face.h"
#include "bloub/bloub_decor.h" /* the rings it connects inside */
#include "bloub/bloub_states.h" /* what it does while the agent works */
#include "assets/lang_config.h"
#include "gif/lvgl_gif.h"
#include "lvgl_theme.h"
#include "settings.h"
#include "voice_geometry.h"
#include "confirm_geometry.h"
#include "watch_icons.h"
#include "watch_dotmatrix.h"

#include <esp_err.h>
#include <esp_log.h>
#include <esp_lvgl_port.h>
#include <esp_psram.h>
#include <material_symbols.h>
#include <noto_emoji.h>
#include <src/misc/cache/lv_cache.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "board.h"
#include "application.h"
#include <esp_heap_caps.h>
#include <mbedtls/base64.h>

#define TAG "LcdDisplay"

LV_FONT_DECLARE(BUILTIN_TEXT_FONT);
LV_FONT_DECLARE(BUILTIN_ICON_FONT);
LV_FONT_DECLARE(font_material_symbols_30_4);
LV_FONT_DECLARE(font_noto_emoji_30_4);

namespace {
// Fluid shading ported from Rare UI's Fluid Orb: https://www.rareui.com/components/fluidorb
// 30fps. The face alone was fine at 15 - it only blinks and drifts - but
// orbit turns the whole body at 1.25 turns a second, which at 15 arrives in
// 30-degree steps and reads as stuttering rather than spinning.
constexpr uint32_t kFluidOrbFramePeriodMs = 33;

constexpr uint32_t kVoiceGreen = 0x30C46E;
constexpr uint32_t kVoiceCyan = 0x2FD8E8;
constexpr uint32_t kVoiceAmber = 0xF5A524;
constexpr uint32_t kVoiceRed = 0xE5484D;
constexpr uint32_t kVoiceGray = 0x8E8E93;
static_assert(voice_character::kShapeCount == static_cast<int>(SHAPE_COUNT),
              "the picker's shape count and bloub's silhouette table have drifted apart");

// The state word's own slot: top edge, and the size of the tool caption that
// shares it. 17 characters is what fits beside an icon at this size on a round
// 360px screen - "CHECKING CALENDAR" exactly, and anything longer is cut.
constexpr int kVoiceCaptionTop = 42;
constexpr int kVoiceToolIconSize = 20;
constexpr size_t kVoiceToolMaxChars = 17;
// How long a named tool keeps the slot before a plain "Thinking" may take it.
// The server announces a tool when the call STARTS and says "Thinking" again
// the moment it finishes, so a quick lookup would otherwise flash past unread
// - and its logo, which arrives in a later message once it has been fetched,
// would land after the caption it belongs to had already gone.
constexpr uint32_t kVoiceToolHoldMs = 1500;
// bloub hands orbit back over a 0.6s cross-fade. The character shrinks to
// 0.68 while the rings are up: they reach about 1.4 times its radius, which at
// resting size would run off this canvas - shrinking the character and the
// rings together keeps bloub's proportion and its motion exactly.
constexpr uint32_t kOrbitExitMs = 600;
constexpr float kOrbitScale = 0.68f;
// What he does while the agent is working. bloub plays its whole catalogue in
// order; this is the part of it that reads as activity rather than as a
// notification, a problem, or sleep - see bloub_states.h.
// Two are left out. PLAY's swoosh sweeps past 1.8 body radii, wider than this
// canvas. EGG and HEXAGON replaced the silhouette outright, so the shape
// picked in Settings vanished mid-sentence - they are gone entirely.
constexpr bloub_state_id_t kWorkingCycle[] = {
    BLOUB_STATE_THINKING, BLOUB_STATE_WINK,
    BLOUB_STATE_THINKING, BLOUB_STATE_WIDE,
};
constexpr int kWorkingCycleLength = sizeof(kWorkingCycle) / sizeof(kWorkingCycle[0]);
// The state being left, frozen at the moment it ended, for the cross-fade to
// read from. File scope rather than a member because bloub_shapes.h defines
// its profile tables inline: including it from lcd_display.h would copy them
// into every translation unit. There is one call screen, on one task.
bloub_pose_t s_pose_leaving{};

struct VoiceStateCaption {
    const char* text;
    uint32_t color;
};

VoiceStateCaption CaptionForDeviceState(DeviceState state, bool muted) {
    switch (state) {
        case kDeviceStateStarting:
        case kDeviceStateActivating: return {"WAKING", kVoiceGreen};
        case kDeviceStateWifiConfiguring:
        case kDeviceStateConnecting: return {"CONNECTING", kVoiceCyan};
        case kDeviceStateListening:
            return muted ? VoiceStateCaption{"MUTED", kVoiceGray}
                         : VoiceStateCaption{"LISTENING", kVoiceGreen};
        case kDeviceStateSpeaking: return {"SPEAKING", kVoiceCyan};
        case kDeviceStateUpgrading: return {"UPGRADING", 0x7465EB};
        case kDeviceStateFatalError: return {"ERROR", kVoiceRed};
        case kDeviceStateAudioTesting: return {"TESTING", kVoiceAmber};
        case kDeviceStateUnknown:
        case kDeviceStateIdle:
        // Idle here is not a status, it is the thing to do next: tapping the
        // character starts a call. 11 characters at this size is 195px, inside
        // the 240 the round screen gives at that height.
        default: return {"TAP TO WAKE", kVoiceGray};
    }
}

}  // namespace

void LcdDisplay::InitializeLcdThemes() {
    auto text_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_TEXT_FONT);
    auto icon_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_ICON_FONT);
    auto large_icon_font = std::make_shared<LvglBuiltInFont>(&font_material_symbols_30_4);
    auto emoji_font = std::make_shared<LvglBuiltInFont>(&font_noto_emoji_30_4);

    // light theme
    auto light_theme = new LvglTheme("light");
    light_theme->set_background_color(lv_color_hex(0xFFFFFF));
    light_theme->set_text_color(lv_color_hex(0x000000));
    light_theme->set_chat_background_color(lv_color_hex(0xE0E0E0));
    light_theme->set_user_bubble_color(lv_color_hex(0x00FF00));
    light_theme->set_assistant_bubble_color(lv_color_hex(0xDDDDDD));
    light_theme->set_system_bubble_color(lv_color_hex(0xFFFFFF));
    light_theme->set_system_text_color(lv_color_hex(0x000000));
    light_theme->set_border_color(lv_color_hex(0x000000));
    light_theme->set_low_battery_color(lv_color_hex(0x000000));
    light_theme->set_text_font(text_font);
    light_theme->set_icon_font(icon_font);
    light_theme->set_large_icon_font(large_icon_font);
    light_theme->set_emoji_font(emoji_font);

    // dark theme
    auto dark_theme = new LvglTheme("dark");
    dark_theme->set_background_color(lv_color_hex(0x000000));
    dark_theme->set_text_color(lv_color_hex(0xFFFFFF));
    dark_theme->set_chat_background_color(lv_color_hex(0x1F1F1F));
    dark_theme->set_user_bubble_color(lv_color_hex(0x00FF00));
    dark_theme->set_assistant_bubble_color(lv_color_hex(0x222222));
    dark_theme->set_system_bubble_color(lv_color_hex(0x000000));
    dark_theme->set_system_text_color(lv_color_hex(0xFFFFFF));
    dark_theme->set_border_color(lv_color_hex(0xFFFFFF));
    dark_theme->set_low_battery_color(lv_color_hex(0xFF0000));
    dark_theme->set_text_font(text_font);
    dark_theme->set_icon_font(icon_font);
    dark_theme->set_large_icon_font(large_icon_font);
    dark_theme->set_emoji_font(emoji_font);

    auto& theme_manager = LvglThemeManager::GetInstance();
    theme_manager.RegisterTheme("light", light_theme);
    theme_manager.RegisterTheme("dark", dark_theme);
}

LcdDisplay::LcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                       int height)
    : panel_io_(panel_io), panel_(panel) {
    width_ = width;
    height_ = height;

    // Initialize LCD themes
    InitializeLcdThemes();

    // Load theme from settings
    Settings settings("display", false);
    std::string theme_name = settings.GetString("theme", "light");
    theme_name = "dark";
    current_theme_ = LvglThemeManager::GetInstance().GetTheme(theme_name);

    // Create a timer to hide the preview image
    esp_timer_create_args_t preview_timer_args = {
        .callback =
            [](void* arg) {
                LcdDisplay* display = static_cast<LcdDisplay*>(arg);
                display->SetPreviewImage(nullptr);
            },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "preview_timer",
        .skip_unhandled_events = false,
    };
    esp_timer_create(&preview_timer_args, &preview_timer_);
}

SpiLcdDisplay::SpiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                             int width, int height, int offset_x, int offset_y, bool mirror_x,
                             bool mirror_y, bool swap_xy)
    : LcdDisplay(panel_io, panel, width, height) {
    // draw white
    std::vector<uint16_t> buffer(width_, 0xFFFF);
    for (int y = 0; y < height_; y++) {
        esp_lcd_panel_draw_bitmap(panel_, 0, y, width_, y + 1, buffer.data());
    }

    // Set the display to on
    ESP_LOGI(TAG, "Turning display on");
    {
        esp_err_t __err = esp_lcd_panel_disp_on_off(panel_, true);
        if (__err == ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGW(TAG, "Panel does not support disp_on_off; assuming ON");
        } else {
            ESP_ERROR_CHECK(__err);
        }
    }

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

#if CONFIG_SPIRAM
    // lv image cache, currently only PNG is supported
    size_t psram_size_mb = esp_psram_get_size() / 1024 / 1024;
    if (psram_size_mb >= 8) {
        lv_image_cache_resize(2 * 1024 * 1024, true);
        ESP_LOGI(TAG, "Use 2MB of PSRAM for image cache");
    } else if (psram_size_mb >= 2) {
        lv_image_cache_resize(512 * 1024, true);
        ESP_LOGI(TAG, "Use 512KB of PSRAM for image cache");
    }
#endif

    ESP_LOGI(TAG, "Initialize LVGL port");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 1;
#if CONFIG_SOC_CPU_CORES_NUM > 1
    port_cfg.task_affinity = 1;
#endif
    lvgl_port_init(&port_cfg);

    ESP_LOGI(TAG, "Adding LCD display");
    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = panel_io_,
        .panel_handle = panel_,
        .control_handle = nullptr,
        .buffer_size = static_cast<uint32_t>(width_ * 20),
        .double_buffer = false,
        .trans_size = 0,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .monochrome = false,
        .rotation =
            {
                .swap_xy = swap_xy,
                .mirror_x = mirror_x,
                .mirror_y = mirror_y,
            },
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags =
            {
                .buff_dma = 1,
                .buff_spiram = 0,
                .sw_rotate = 0,
                .swap_bytes = 1,
                .full_refresh = 0,
                .direct_mode = 0,
            },
    };

    display_ = lvgl_port_add_disp(&display_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add display");
        return;
    }

    if (offset_x != 0 || offset_y != 0) {
        lv_display_set_offset(display_, offset_x, offset_y);
    }
}

// RGB LCD implementation
RgbLcdDisplay::RgbLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                             int width, int height, int offset_x, int offset_y, bool mirror_x,
                             bool mirror_y, bool swap_xy)
    : LcdDisplay(panel_io, panel, width, height) {
    // draw white
    std::vector<uint16_t> buffer(width_, 0xFFFF);
    for (int y = 0; y < height_; y++) {
        esp_lcd_panel_draw_bitmap(panel_, 0, y, width_, y + 1, buffer.data());
    }

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

    ESP_LOGI(TAG, "Initialize LVGL port");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 1;
    port_cfg.timer_period_ms = 50;
    lvgl_port_init(&port_cfg);

    ESP_LOGI(TAG, "Adding LCD display");
    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = panel_io_,
        .panel_handle = panel_,
        .buffer_size = static_cast<uint32_t>(width_ * 20),
        .double_buffer = true,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .rotation =
            {
                .swap_xy = swap_xy,
                .mirror_x = mirror_x,
                .mirror_y = mirror_y,
            },
        .flags =
            {
                .buff_dma = 1,
                .swap_bytes = 0,
                .full_refresh = 1,
                .direct_mode = 1,
            },
    };

    const lvgl_port_display_rgb_cfg_t rgb_cfg = {.flags = {
                                                     .bb_mode = true,
                                                     .avoid_tearing = true,
                                                 }};

    display_ = lvgl_port_add_disp_rgb(&display_cfg, &rgb_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add RGB display");
        return;
    }

    if (offset_x != 0 || offset_y != 0) {
        lv_display_set_offset(display_, offset_x, offset_y);
    }
}

MipiLcdDisplay::MipiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                               int width, int height, int offset_x, int offset_y, bool mirror_x,
                               bool mirror_y, bool swap_xy)
    : LcdDisplay(panel_io, panel, width, height) {
    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

    ESP_LOGI(TAG, "Initialize LVGL port");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_port_init(&port_cfg);

    ESP_LOGI(TAG, "Adding LCD display");
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = panel_io,
        .panel_handle = panel,
        .control_handle = nullptr,
        .buffer_size = static_cast<uint32_t>(width_ * 50),
        .double_buffer = false,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .monochrome = false,
        /* Rotation values must be same as used in esp_lcd for initial settings of the screen */
        .rotation =
            {
                .swap_xy = swap_xy,
                .mirror_x = mirror_x,
                .mirror_y = mirror_y,
            },
        .flags =
            {
                .buff_dma = true,
                .buff_spiram = false,
                .sw_rotate = true,
            },
    };

    const lvgl_port_display_dsi_cfg_t dpi_cfg = {.flags = {
                                                     .avoid_tearing = false,
                                                 }};
    display_ = lvgl_port_add_disp_dsi(&disp_cfg, &dpi_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add display");
        return;
    }

    if (offset_x != 0 || offset_y != 0) {
        lv_display_set_offset(display_, offset_x, offset_y);
    }
}

LcdDisplay::~LcdDisplay() {
    SetPreviewImage(nullptr);

    if (touch_input_) lv_indev_delete(touch_input_);
    watch_ui_.reset();
    if (voice_orb_timer_ != nullptr) {
        lv_timer_delete(voice_orb_timer_);
        voice_orb_timer_ = nullptr;
    }

    // Clean up GIF controller
    if (gif_controller_) {
        gif_controller_->Stop();
        gif_controller_.reset();
    }

    if (preview_timer_ != nullptr) {
        esp_timer_stop(preview_timer_);
        esp_timer_delete(preview_timer_);
    }

    if (preview_image_ != nullptr) {
        lv_obj_del(preview_image_);
    }
    if (chat_message_label_ != nullptr) {
        lv_obj_del(chat_message_label_);
    }
    if (emoji_label_ != nullptr) {
        lv_obj_del(emoji_label_);
    }
    if (emoji_image_ != nullptr) {
        lv_obj_del(emoji_image_);
    }
    if (emoji_box_ != nullptr) {
        lv_obj_del(emoji_box_);
    }
    if (voice_orb_buffer_ != nullptr) {
        heap_caps_free(voice_orb_buffer_);
        voice_orb_buffer_ = nullptr;
    }
    if (content_ != nullptr) {
        lv_obj_del(content_);
    }
    if (bottom_bar_ != nullptr) {
        lv_obj_del(bottom_bar_);
    }
    if (status_bar_ != nullptr) {
        lv_obj_del(status_bar_);
    }
    if (top_bar_ != nullptr) {
        lv_obj_del(top_bar_);
    }
    if (side_bar_ != nullptr) {
        lv_obj_del(side_bar_);
    }
    if (container_ != nullptr) {
        lv_obj_del(container_);
    }
    if (display_ != nullptr) {
        lv_display_delete(display_);
    }

    if (panel_ != nullptr) {
        esp_lcd_panel_del(panel_);
    }
    if (panel_io_ != nullptr) {
        esp_lcd_panel_io_del(panel_io_);
    }
}

bool LcdDisplay::Lock(int timeout_ms) { return lvgl_port_lock(timeout_ms); }

void LcdDisplay::Unlock() { lvgl_port_unlock(); }

void LcdDisplay::SetupUI() {
    // Prevent duplicate calls - if already called, return early
    if (setup_ui_called_) {
        ESP_LOGW(TAG, "SetupUI() called multiple times, skipping duplicate call");
        return;
    }

    Display::SetupUI();  // Mark SetupUI as called
    DisplayLockGuard lock(this);
    LvglTheme* lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();
    auto large_icon_font = lvgl_theme->large_icon_font()->font();

    voice_root_ = lv_obj_create(lv_screen_active());
    {
        Settings character("display", false);
        voice_shape_ = std::clamp<int32_t>(character.GetInt("voice_shape", 0), 0,
                                          voice_character::kShapeCount - 1);
        voice_colour_ = std::clamp<int32_t>(character.GetInt("voice_colour", 0), 0,
                                           voice_character::kColorCount - 1);
    }
    {
        // The captions toggle has to be read back here too, or turning them
        // off would only last until the next boot.
        Settings voice("codex_voice", false);
        hide_subtitle_ = !voice.GetBool("captions", true);
    }
    /* Black, like every other screen the character appears on. The voice screen
     * used to be navy, which left the character sitting in a black square on a
     * dark blue page - two different darks, and the square was the seam. */
    lv_obj_set_style_bg_color(voice_root_, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_grad_color(voice_root_, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_grad_dir(voice_root_, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_bg_opa(voice_root_, LV_OPA_COVER, 0);
    lv_obj_set_size(voice_root_, 360, 360);
    lv_obj_set_pos(voice_root_, 0, 0);
    lv_obj_set_style_pad_all(voice_root_, 0, 0);
    lv_obj_set_style_border_width(voice_root_, 0, 0);
    lv_obj_set_style_radius(voice_root_, 0, 0);
    lv_obj_remove_flag(voice_root_, LV_OBJ_FLAG_SCROLLABLE);
    auto screen = voice_root_;
    lv_obj_set_style_text_font(screen, text_font, 0);
    lv_obj_set_style_text_color(screen, lvgl_theme->text_color(), 0);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), 0);

    /* Container - used as background */
    container_ = lv_obj_create(screen);
    lv_obj_set_size(container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_radius(container_, 0, 0);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_bg_color(container_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_border_color(container_, lvgl_theme->border_color(), 0);

    /* Bottom layer: emoji_box_ - centered display */
    emoji_box_ = lv_obj_create(screen);
    lv_obj_set_size(emoji_box_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(emoji_box_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(emoji_box_, 0, 0);
    lv_obj_set_style_border_width(emoji_box_, 0, 0);
    lv_obj_align(emoji_box_, LV_ALIGN_CENTER, 0, 0);

    emoji_label_ = lv_label_create(emoji_box_);
    lv_obj_set_style_text_font(emoji_label_, large_icon_font, 0);
    lv_obj_set_style_text_color(emoji_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(emoji_label_, MATERIAL_SYMBOLS_ROBOT_2);

    emoji_image_ = lv_img_create(emoji_box_);
    lv_obj_center(emoji_image_);
    lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);

    /* Middle layer: preview_image_ - centered display */
    preview_image_ = lv_image_create(screen);
    lv_obj_set_size(preview_image_, width_ / 2, height_ / 2);
    lv_obj_align(preview_image_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);

    /* Layer 1: Top bar - for status icons */
    top_bar_ = lv_obj_create(screen);
    lv_obj_set_size(top_bar_, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(top_bar_, 0, 0);
    lv_obj_set_style_bg_opa(top_bar_, LV_OPA_50, 0);  // 50% opacity background
    lv_obj_set_style_bg_color(top_bar_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_border_width(top_bar_, 0, 0);
    lv_obj_set_style_pad_all(top_bar_, 0, 0);
    lv_obj_set_style_pad_top(top_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_bottom(top_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_left(top_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_pad_right(top_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_flex_flow(top_bar_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top_bar_, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(top_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(top_bar_, LV_ALIGN_TOP_MID, 0, 0);

    // Left icon
    network_label_ = lv_label_create(top_bar_);
    lv_label_set_text(network_label_, "");
    lv_obj_set_style_text_font(network_label_, icon_font, 0);
    lv_obj_set_style_text_color(network_label_, lvgl_theme->text_color(), 0);

    // Right icons container
    lv_obj_t* right_icons = lv_obj_create(top_bar_);
    lv_obj_set_size(right_icons, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(right_icons, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right_icons, 0, 0);
    lv_obj_set_style_pad_all(right_icons, 0, 0);
    lv_obj_set_flex_flow(right_icons, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right_icons, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    mute_label_ = lv_label_create(right_icons);
    lv_label_set_text(mute_label_, "");
    lv_obj_set_style_text_font(mute_label_, icon_font, 0);
    lv_obj_set_style_text_color(mute_label_, lvgl_theme->text_color(), 0);

    battery_label_ = lv_label_create(right_icons);
    lv_label_set_text(battery_label_, "");
    lv_obj_set_style_text_font(battery_label_, icon_font, 0);
    lv_obj_set_style_text_color(battery_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_margin_left(battery_label_, lvgl_theme->spacing(2), 0);

    /* Layer 2: Status bar - for center text labels */
    status_bar_ = lv_obj_create(screen);
    lv_obj_set_size(status_bar_, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(status_bar_, 0, 0);
    lv_obj_set_style_bg_opa(status_bar_, LV_OPA_TRANSP, 0);  // Transparent background
    lv_obj_set_style_border_width(status_bar_, 0, 0);
    lv_obj_set_style_pad_all(status_bar_, 0, 0);
    lv_obj_set_style_pad_top(status_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_bottom(status_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_scrollbar_mode(status_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_layout(status_bar_, LV_LAYOUT_NONE, 0);  // Use absolute positioning
    lv_obj_align(status_bar_, LV_ALIGN_TOP_MID, 0, 0);        // Overlap with top_bar_

    notification_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(notification_label_, LV_HOR_RES * 0.75);
    lv_obj_set_style_text_align(notification_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(notification_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(notification_label_, "");
    lv_obj_align(notification_label_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    status_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(status_label_, LV_HOR_RES * 0.75);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(status_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(status_label_, Lang::Strings::INITIALIZING);
    lv_obj_align(status_label_, LV_ALIGN_CENTER, 0, 0);

#if CONFIG_USE_MULTILINE_CHAT_MESSAGE
    /* Bottom bar - auto height, grows upward with wrapped text */
    bottom_bar_ = lv_obj_create(screen);
    lv_obj_set_width(bottom_bar_, LV_HOR_RES);
    lv_obj_set_height(bottom_bar_, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(bottom_bar_, 0, 0);
    lv_obj_set_style_bg_color(bottom_bar_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_bg_opa(bottom_bar_, LV_OPA_50, 0);
    lv_obj_set_style_text_color(bottom_bar_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_pad_all(bottom_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_border_width(bottom_bar_, 0, 0);
    lv_obj_set_scrollbar_mode(bottom_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(bottom_bar_, LV_ALIGN_BOTTOM_MID, 0, 0);

    /* chat_message_label_ placed in bottom_bar_, multiline wrapped display */
    chat_message_label_ = lv_label_create(bottom_bar_);
    lv_label_set_text(chat_message_label_, "");
    lv_obj_set_width(chat_message_label_, LV_HOR_RES - lvgl_theme->spacing(8));
    lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(chat_message_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(chat_message_label_, lvgl_theme->text_color(), 0);
    lv_obj_align(chat_message_label_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);  // Hide until there is content
#else
    /* Top layer: Bottom bar - fixed height at bottom */
    bottom_bar_ = lv_obj_create(screen);
    lv_obj_set_size(bottom_bar_, LV_HOR_RES, text_font->line_height + lvgl_theme->spacing(8));
    lv_obj_set_style_radius(bottom_bar_, 0, 0);
    lv_obj_set_style_bg_color(bottom_bar_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_text_color(bottom_bar_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_pad_all(bottom_bar_, 0, 0);
    lv_obj_set_style_pad_left(bottom_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_pad_right(bottom_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_border_width(bottom_bar_, 0, 0);
    lv_obj_set_scrollbar_mode(bottom_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(bottom_bar_, LV_ALIGN_BOTTOM_MID, 0, 0);

    /* chat_message_label_ placed in bottom_bar_, single-line horizontal scroll */
    chat_message_label_ = lv_label_create(bottom_bar_);
    lv_label_set_text(chat_message_label_, "");
    lv_obj_set_width(chat_message_label_, LV_HOR_RES - lvgl_theme->spacing(8));
    lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(chat_message_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(chat_message_label_, lvgl_theme->text_color(), 0);
    lv_obj_align(chat_message_label_, LV_ALIGN_CENTER, 0, 0);

    // Start scrolling after a delay (short text won't scroll)
    static lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_delay(&a, 1000);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_obj_set_style_anim(chat_message_label_, &a, LV_PART_MAIN);
    lv_obj_set_style_anim_duration(chat_message_label_, lv_anim_speed_clamped(60, 300, 60000),
                                   LV_PART_MAIN);
    lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);  // Hide until there is content
#endif

    // Keep text inside the circle, away from the clipped top and bottom edges.
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_color(container_, lv_color_black(), 0);
    lv_obj_remove_flag(container_, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    lv_obj_set_style_text_color(status_label_, lv_color_white(), 0);
    lv_obj_set_style_text_color(notification_label_, lv_color_white(), 0);
    lv_obj_set_style_text_color(chat_message_label_, lv_color_white(), 0);
    /* The transcript: what was said, and what came back. The mockup layout
     * dropped it, but the words are still arriving - so it gets its strip back
     * below the call buttons, and SetChatMessage shows it when there is
     * something to show.
     *
     * 180 wide rather than the old 190: this sits low on a round screen, where
     * the usable width is only about 185, and the old strip's corners were
     * outside the glass. Long lines scroll sideways rather than wrap - there
     * is one line of room here, not two.
     *
     * The label's height is its text's, not a number picked to look right: a
     * label shorter than one line of its own font makes LVGL scroll the text
     * UP AND DOWN to show the rest of it, which is what the caption bouncing
     * was. Its own padding goes to zero for the same reason - padding comes
     * out of the height the text is measured against.
     *
     * Circular rather than plain SCROLL for the sideways travel: plain SCROLL
     * is LVGL's back-and-forth mode, and it is the mode that owns that
     * vertical animation at all. Circular only ever travels one way. */
    lv_obj_set_size(bottom_bar_, 180, 28);
    lv_obj_align(bottom_bar_, LV_ALIGN_BOTTOM_MID, 0, -28);
    lv_obj_set_style_bg_opa(bottom_bar_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(bottom_bar_, 0, 0);
    lv_obj_set_style_pad_all(chat_message_label_, 0, 0);
    lv_obj_set_size(chat_message_label_, 174, LV_SIZE_CONTENT);
    lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_align(chat_message_label_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);  // until there is something said
    lv_obj_add_flag(top_bar_, LV_OBJ_FLAG_HIDDEN);
    /* The pill that names what the agent is doing - "Searching email", with
     * that connector's own icon. The mockup layout dropped the old
     * "Listening..." pill and hid this bar with it, which took the tool
     * caption and the plugin icon down too. The bar stays gone - it was the
     * old rounded chip, in the old typeface, and the redesign was right about
     * it. The tool caption is rebuilt below out of the same dot-matrix text
     * the state word is made of, in the state word's own slot. */
    lv_obj_add_flag(status_bar_, LV_OBJ_FLAG_HIDDEN);
    const auto initial_caption = CaptionForDeviceState(Application::GetInstance().GetDeviceState(), false);
    UpdateVoiceStateCaption(initial_caption.text, initial_caption.color);

    voice_tool_active_ = false;

    lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(emoji_box_, voice_geometry::kOrbSize, voice_geometry::kOrbSize);
    lv_obj_align(emoji_box_, LV_ALIGN_CENTER, 0, 4);
    lv_obj_set_style_radius(emoji_box_, 0, 0);
    lv_obj_set_style_clip_corner(emoji_box_, false, 0);
    lv_obj_set_style_bg_opa(emoji_box_, LV_OPA_TRANSP, 0);
    /* The canvas and screen share one black field, so the character appears
     * directly on the page without the old orb silhouette behind it. */
    lv_obj_set_style_bg_color(emoji_box_, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_grad_color(emoji_box_, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_grad_dir(emoji_box_, LV_GRAD_DIR_NONE, 0);

    const size_t orb_buffer_size = static_cast<size_t>(voice_geometry::kOrbSize) *
                                   voice_geometry::kOrbSize * sizeof(lv_color16_t);
    voice_orb_buffer_ = static_cast<lv_color16_t*>(
        heap_caps_malloc(orb_buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (voice_orb_buffer_ == nullptr) {
        voice_orb_buffer_ = static_cast<lv_color16_t*>(
            heap_caps_malloc(orb_buffer_size, MALLOC_CAP_8BIT));
    }
    if (voice_orb_buffer_ != nullptr) {
        voice_orb_canvas_ = lv_canvas_create(emoji_box_);
        if (voice_orb_canvas_ != nullptr) {
            lv_canvas_set_buffer(voice_orb_canvas_, voice_orb_buffer_, voice_geometry::kOrbSize,
                                 voice_geometry::kOrbSize, LV_COLOR_FORMAT_RGB565);
            lv_obj_set_size(voice_orb_canvas_, voice_geometry::kOrbSize, voice_geometry::kOrbSize);
            lv_obj_align(voice_orb_canvas_, LV_ALIGN_CENTER, 0, 0);
            lv_obj_set_style_radius(voice_orb_canvas_, 0, 0);
            lv_obj_set_style_clip_corner(voice_orb_canvas_, false, 0);
            /* Opaque. The orb could afford to be half-transparent over its own
             * gradient; a face cannot - it was coming out washed out. */
            lv_obj_set_style_image_opa(voice_orb_canvas_, LV_OPA_COVER, 0);
            lv_obj_remove_flag(voice_orb_canvas_, LV_OBJ_FLAG_SCROLLABLE);
            RenderVoiceOrb(0.0f);
            voice_orb_timer_ = lv_timer_create(
                [](lv_timer_t* timer) {
                    auto display = static_cast<LcdDisplay*>(lv_timer_get_user_data(timer));
                    /* Runs whenever the call screen is up, in a call or not:
                     * the blink and the gaze drift are what stop him looking
                     * switched off while he waits. */
                    if (!lv_obj_has_flag(display->voice_root_, LV_OBJ_FLAG_HIDDEN)) {
                        display->RenderVoiceOrb(
                            static_cast<float>(lv_tick_elaps(display->voice_orb_started_at_)) /
                            1000.0f);
                    }
                },
                kFluidOrbFramePeriodMs, this);
            // Left running: it costs nothing while the call screen is hidden,
            // and it means he is already alive the first time it is opened,
            // without waiting for a status to arrive and start him.
        } else {
            heap_caps_free(voice_orb_buffer_);
            voice_orb_buffer_ = nullptr;
        }
    }

    for (int index = 0; index < 2; ++index) {
        auto button = lv_obj_create(screen);
        lv_obj_set_size(button, voice_geometry::kButtonSize, voice_geometry::kButtonSize);
        lv_obj_set_pos(button, index == 0 ? voice_geometry::kMuteLeft : voice_geometry::kEndLeft,
                       voice_geometry::kButtonTop);
        lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
        /* Mic and hang-up. The mic keeps the dark disc the user likes; hang-up
         * is red, because it is the one control that ends something. Both use
         * 0x1A1A1A rather than the old 0x292929 so they match the disc the
         * back arrow and the three dots sit in. */
        lv_obj_set_style_bg_color(button, lv_color_hex(index == 0 ? 0x1A1A1A : 0xE5484D), 0);
        lv_obj_set_style_border_width(button, 0, 0);
        lv_obj_set_style_pad_all(button, 0, 0);
        lv_obj_remove_flag(button, LV_OBJ_FLAG_SCROLLABLE);
        auto icon = lv_label_create(button);
        lv_obj_set_style_text_font(icon, large_icon_font, 0);
        lv_obj_set_style_text_color(icon, lv_color_white(), 0);
        lv_label_set_text(icon, index == 0 ? MATERIAL_SYMBOLS_MIC : MATERIAL_SYMBOLS_CLOSE);
        lv_obj_center(icon);
        lv_obj_add_event_cb(button, [](lv_event_t* e) {
            auto display = static_cast<LcdDisplay*>(lv_event_get_user_data(e));
            auto action = lv_event_get_target(e) == display->voice_mute_button_
                              ? WatchUi::Action::Mute : WatchUi::Action::EndCall;
            Application::GetInstance().OnWatchAction(action, 0, "", "");
        }, LV_EVENT_CLICKED, this);
        lv_obj_add_flag(button, LV_OBJ_FLAG_HIDDEN);
        if (index == 0) {
            voice_mute_button_ = button;
            voice_mute_icon_ = icon;
        } else {
            voice_end_button_ = button;
        }
    }

    low_battery_popup_ = lv_obj_create(screen);
    lv_obj_set_scrollbar_mode(low_battery_popup_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(low_battery_popup_, LV_HOR_RES * 0.9, text_font->line_height * 2);
    lv_obj_align(low_battery_popup_, LV_ALIGN_BOTTOM_MID, 0, -lvgl_theme->spacing(4));
    lv_obj_set_style_bg_color(low_battery_popup_, lvgl_theme->low_battery_color(), 0);
    lv_obj_set_style_radius(low_battery_popup_, lvgl_theme->spacing(4), 0);

    low_battery_label_ = lv_label_create(low_battery_popup_);
    lv_label_set_text(low_battery_label_, Lang::Strings::BATTERY_NEED_CHARGE);
    lv_obj_set_style_text_color(low_battery_label_, lv_color_white(), 0);
    lv_obj_center(low_battery_label_);
    lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
    watch_ui_ = std::make_unique<WatchUi>(voice_root_, lvgl_theme->text_font(),
        [](WatchUi::Action a, int n, const std::string& text, const std::string& secret) {
            Application::GetInstance().OnWatchAction(a, n, text, secret);
        });
    for (int i = 0; i < 2; ++i) {
        auto b = lv_obj_create(screen);
        lv_obj_set_pos(b, i == 0 ? 52 : 264, 74);
        lv_obj_set_size(b, 44, 44);
        lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_pad_all(b, 0, 0);
        lv_obj_set_style_border_width(b, 0, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x1A1A1A), 0);
        lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        auto icon = lv_image_create(b);
        lv_image_set_src(icon, i == 0 ? &watch_icons::back : &watch_icons::more);
        lv_obj_set_style_image_recolor(icon, lv_color_white(), 0);
        lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
        lv_obj_center(icon);
        lv_obj_add_event_cb(b, [](lv_event_t* e) {
            auto self = static_cast<LcdDisplay*>(lv_event_get_user_data(e));
            const bool home = lv_obj_get_x(static_cast<lv_obj_t*>(lv_event_get_target(e))) == 52;
            if (home) Application::GetInstance().OnWatchAction(WatchUi::Action::EndCall, 0, "", "");
            self->watch_ui_->Show(home ? WatchUi::Page::Home : WatchUi::Page::CodexSettings);
            Application::GetInstance().OnWatchAction(WatchUi::Action::Refresh, 0, "", "");
        }, LV_EVENT_CLICKED, this);
    }
    lv_obj_add_event_cb(emoji_box_, [](lv_event_t*) {
        Application::GetInstance().OnWatchAction(WatchUi::Action::OpenVoice, 0, "", "");
    }, LV_EVENT_CLICKED, this);
    lv_obj_remove_flag(status_bar_, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    touch_input_ = lv_indev_create();
    lv_indev_set_type(touch_input_, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(touch_input_, display_);
    lv_indev_set_user_data(touch_input_, this);
    lv_indev_set_read_cb(touch_input_, [](lv_indev_t* input, lv_indev_data_t* data) {
        auto self = static_cast<LcdDisplay*>(lv_indev_get_user_data(input));
        const uint32_t sample = self->touch_sample_.load();
        data->point.x = sample & 0x1ff;
        data->point.y = (sample >> 9) & 0x1ff;
        data->state = (sample >> 18) ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    });
    ESP_LOGI(TAG, "Codex watch UI: home, voice, settings, keyboard; LVGL touch ready");
}

void LcdDisplay::SetPreviewImage(std::unique_ptr<LvglImage> image) {
    DisplayLockGuard lock(this);
    if (preview_image_ == nullptr) {
        ESP_LOGE(TAG, "Preview image is not initialized");
        return;
    }

    if (image == nullptr) {
        esp_timer_stop(preview_timer_);
        lv_obj_remove_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
        preview_image_cached_.reset();
        if (gif_controller_) {
            gif_controller_->Start();
        }
        return;
    }

    preview_image_cached_ = std::move(image);
    auto img_dsc = preview_image_cached_->image_dsc();
    lv_image_set_src(preview_image_, img_dsc);
    if (img_dsc->header.w > 0 && img_dsc->header.h > 0) {
        // zoom factor 0.5
        lv_image_set_scale(preview_image_, 128 * width_ / img_dsc->header.w);
    }

    // Hide emoji_box_
    if (gif_controller_) {
        gif_controller_->Stop();
    }
    lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
    esp_timer_stop(preview_timer_);
    ESP_ERROR_CHECK(esp_timer_start_once(preview_timer_, PREVIEW_IMAGE_DURATION_MS * 1000));
}

void LcdDisplay::SetChatMessage(const char* role, const char* content) {
    if (!setup_ui_called_) {
        ESP_LOGW(TAG, "SetChatMessage('%s', '%s') called before SetupUI() - message will be lost!",
                 role, content);
    }
    DisplayLockGuard lock(this);
    if (chat_message_label_ == nullptr) {
        if (setup_ui_called_) {
            ESP_LOGW(TAG,
                     "SetChatMessage('%s', '%s') failed: chat_message_label_ is nullptr (SetupUI() "
                     "was called but label not created)",
                     role, content);
        }
        return;
    }
    lv_anim_delete(chat_message_label_, nullptr);
    lv_label_set_text(chat_message_label_, content);
    /* One line carries both halves of the conversation, so they have to be
     * told apart by eye: the reply is what is being read out, so it is the one
     * in white, and what was heard sits back in grey. */
    lv_obj_set_style_text_color(chat_message_label_,
                                lv_color_hex(strcmp(role, "user") == 0 ? kVoiceGray : 0xFFFFFF), 0);
    // Show bottom_bar_ only when there is content (and subtitle is not globally hidden)
    if (bottom_bar_ != nullptr) {
        if (content == nullptr || content[0] == '\0') {
            lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        } else if (!hide_subtitle_) {
            lv_obj_remove_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void LcdDisplay::ClearChatMessages() {
    DisplayLockGuard lock(this);
    // In non-wechat mode, just clear the chat message label and hide the bar
    if (chat_message_label_ != nullptr) {
        lv_label_set_text(chat_message_label_, "");
    }
    if (bottom_bar_ != nullptr) {
        lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
    }
}

void LcdDisplay::SetVoiceModel(const char* name) {
    DisplayLockGuard lock(this);
    if (voice_model_label_ != nullptr) {
        const char* short_name = strrchr(name, '/');
        lv_label_set_text(voice_model_label_, short_name == nullptr ? name : short_name + 1);
    }
}

void LcdDisplay::HideVoiceModels() {
    DisplayLockGuard lock(this);
    if (voice_model_panel_ != nullptr) lv_obj_add_flag(voice_model_panel_, LV_OBJ_FLAG_HIDDEN);
}

void LcdDisplay::ShowVoiceModels(const std::vector<std::string>& names, size_t page) {
    DisplayLockGuard lock(this);
    if (voice_model_panel_ == nullptr) {
        voice_model_panel_ = lv_obj_create(lv_screen_active());
        lv_obj_set_size(voice_model_panel_, width_, height_);
        lv_obj_set_pos(voice_model_panel_, 0, 0);
        lv_obj_set_style_bg_color(voice_model_panel_, lv_color_black(), 0);
        lv_obj_set_style_text_color(voice_model_panel_, lv_color_white(), 0);
        lv_obj_set_style_pad_all(voice_model_panel_, 0, 0);
        lv_obj_set_style_border_width(voice_model_panel_, 0, 0);
        lv_obj_remove_flag(voice_model_panel_, LV_OBJ_FLAG_SCROLLABLE);
    }
    lv_obj_clean(voice_model_panel_);
    lv_obj_remove_flag(voice_model_panel_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(voice_model_panel_);
    auto title = lv_label_create(voice_model_panel_);
    lv_label_set_text(title, "Model for next call");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 56);
    if (names.empty() || (names.size() == 1 && names.front() == "Default")) {
        auto hint = lv_label_create(voice_model_panel_);
        lv_obj_set_width(hint, 220);
        lv_label_set_text(hint, "Open a call first\nto load your models");
        lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(hint, LV_ALIGN_CENTER, 0, 20);
    }
    for (int row = 0; row < voice_geometry::kModelsPerPage; ++row) {
        const size_t index = page * voice_geometry::kModelsPerPage + row;
        if (index >= names.size()) break;
        auto button = lv_obj_create(voice_model_panel_);
        lv_obj_set_pos(button, voice_geometry::kModelRowLeft,
                       voice_geometry::kModelRowTop + row * voice_geometry::kModelRowStep);
        lv_obj_set_size(button, voice_geometry::kModelRowWidth, voice_geometry::kModelRowHeight);
        lv_obj_set_style_bg_color(button, lv_color_hex(0x1A1A1A), 0);
        lv_obj_set_style_border_width(button, 0, 0);
        lv_obj_set_style_pad_all(button, 0, 0);
        lv_obj_remove_flag(button, LV_OBJ_FLAG_SCROLLABLE);
        auto label = lv_label_create(button);
        const char* short_name = strrchr(names[index].c_str(), '/');
        lv_label_set_text(label, short_name == nullptr ? names[index].c_str() : short_name + 1);
        lv_obj_set_width(label, 200);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(label);
    }
    for (int index = 0; index < 2; ++index) {
        auto button = lv_obj_create(voice_model_panel_);
        lv_obj_set_pos(button, index == 0 ? voice_geometry::kMuteLeft : voice_geometry::kEndLeft,
                       voice_geometry::kButtonTop);
        lv_obj_set_size(button, voice_geometry::kButtonSize, voice_geometry::kButtonSize);
        lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_pad_all(button, 0, 0);
        lv_obj_set_style_border_width(button, 0, 0);
        lv_obj_set_style_bg_color(button, lv_color_hex(0x1A1A1A), 0);
        lv_obj_remove_flag(button, LV_OBJ_FLAG_SCROLLABLE);
        auto label = lv_label_create(button);
        lv_label_set_text(label, index == 0 ? "Back" : "Next");
        lv_obj_center(label);
    }
}

static bool VoiceIconIsThinking(const char* activity) {
    if (activity == nullptr) return false;
    auto eq_ci = [](const char* a, const char* b, size_t n) {
        for (size_t i = 0; i < n; ++i) {
            char ca = a[i], cb = b[i];
            if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca + 32);
            if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb + 32);
            if (ca != cb || ca == '\0') return ca == cb;
        }
        return true;
    };
    while (*activity == ' ' || *activity == '\t') ++activity;
    return eq_ci(activity, "thinking", 8) || eq_ci(activity, "reasoning", 9);
}

/* The state word and the tool caption share one slot at the top of the call
 * screen. Whichever is more specific wins: "Searching email" beats "THINKING",
 * and when the tool is done the word comes back. */
void LcdDisplay::ShowVoiceToolCaption(bool tool) {
    if (voice_state_caption_ != nullptr) {
        if (tool) lv_obj_add_flag(voice_state_caption_, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(voice_state_caption_, LV_OBJ_FLAG_HIDDEN);
    }
    if (!tool) ClearVoiceToolCaption();
}

/* Lets go of a tool caption that was being held open. */
void LcdDisplay::ReleaseVoiceToolHold() {
    if (voice_tool_hold_timer_ != nullptr) {
        lv_timer_delete(voice_tool_hold_timer_);
        voice_tool_hold_timer_ = nullptr;
    }
}

void LcdDisplay::ClearVoiceToolCaption() {
    if (voice_tool_text_ != nullptr) { lv_obj_delete(voice_tool_text_); voice_tool_text_ = nullptr; }
    if (voice_tool_icon_ != nullptr) { lv_obj_delete(voice_tool_icon_); voice_tool_icon_ = nullptr; }
    if (voice_activity_image_) {
        lv_image_cache_drop(voice_activity_image_->image_dsc());
        voice_activity_image_.reset();
    }
}

/* Draws "Searching email" where the state word goes, in the state word's own
 * dot-matrix face, with the connector's icon beside it.
 *
 * Its width comes out of dm_width rather than a clamp, so the old
 * scripts/tests/test_voice_layout.py went with the pill it sized; what it was
 * guarding - that the caption fits the screen - is now scripts/tests/caption_host,
 * which renders this slot against the round boundary instead of asserting on
 * numbers.
 *
 * One step smaller than the state word, and that is not a style choice: the
 * screen is round, so at this height it is 240px wide, and a tool caption at
 * the state word's size runs to nearly 300. Same typeface, one size down, is
 * what fits. Longer captions are cut rather than allowed off the edge.
 *
 * The icon's place is held whether or not there is an icon yet. The server
 * sends the words first and the connector's logo in a second message, once it
 * has fetched and converted it - so laying the words out to fit the space they
 * have now would shove them sideways when the logo turns up a moment later.
 * They are laid out for the logo from the start, and it drops into the gap. */
void LcdDisplay::UpdateVoiceToolCaption(const char* activity) {
    if (voice_root_ == nullptr || activity == nullptr) return;
    if (voice_tool_text_ != nullptr) { lv_obj_delete(voice_tool_text_); voice_tool_text_ = nullptr; }

    /* The dot-matrix font is A-Z, digits and punctuation - it folds lowercase
     * itself and draws anything else as a space. */
    char text[kVoiceToolMaxChars + 1];
    size_t n = 0;
    for (const char* c = activity; *c != '\0' && n < kVoiceToolMaxChars; ++c) {
        if (static_cast<unsigned char>(*c) < 0x80) text[n++] = *c;
    }
    while (n > 0 && text[n - 1] == ' ') --n;
    text[n] = '\0';
    if (n == 0) return;

    dm_style_t style = {2, 1, 1, kVoiceAmber, 0x101010};
    const int text_w = dm_width(text, &style);
    const int icon_w = kVoiceToolIconSize;   // held even before the logo lands
    const int gap = 8;
    const int left = 180 - (icon_w + gap + text_w) / 2;
    /* Both sit on the state word's own centre line, so the slot does not jump
     * when one replaces the other. */
    const int centre_y = kVoiceCaptionTop + DM_H * 3 / 2;
    voice_tool_text_ = dm_text(voice_root_, left + icon_w + gap, centre_y - DM_H * 2 / 2, text,
                               &style);
    if (voice_tool_icon_ != nullptr) {
        lv_obj_set_pos(voice_tool_icon_, left, centre_y - kVoiceToolIconSize / 2);
        lv_obj_move_foreground(voice_tool_icon_);
    }
}

void LcdDisplay::UpdateVoiceStateCaption(const char* text, uint32_t color) {
    if (voice_root_ == nullptr || text == nullptr) return;
    if (voice_state_caption_ != nullptr && voice_state_caption_text_ == text &&
        voice_state_caption_color_ == color) return;
    if (voice_state_caption_ != nullptr) lv_obj_delete(voice_state_caption_);
    dm_style_t style = {3, 2, 1, color, 0x101010};
    voice_state_caption_ = dm_text_center(voice_root_, 180, 42, text, &style);
    voice_state_caption_text_ = text;
    voice_state_caption_color_ = color;
    // Rebuilt from scratch each time, so it has to be put back behind the tool
    // caption if one is up.
    if (voice_tool_active_ && voice_state_caption_ != nullptr) {
        lv_obj_add_flag(voice_state_caption_, LV_OBJ_FLAG_HIDDEN);
    }
}



void LcdDisplay::SetVoiceActivity(const char* activity, const char* icon, const char* pixels) {
    DisplayLockGuard lock(this);
    if (voice_root_ == nullptr) return;
    /* Nothing is torn down before the hold decision below: the branch that
     * keeps a just-announced tool on screen does so by leaving it alone, and
     * clearing up here deleted the very caption it was protecting - an empty
     * slot for the whole 1.5s, since the state word stays hidden behind it. */
    if (activity == nullptr || activity[0] == '\0' || strcmp(activity, "Listening") == 0) {
        ReleaseVoiceToolHold();
        ClearVoiceToolCaption();
        voice_tool_active_ = false;
        voice_working_ = false;
        ShowVoiceToolCaption(false);
        SetStatus(Lang::Strings::LISTENING);
        return;
    }
    const bool named_tool = !VoiceIconIsThinking(activity) && strcmp(activity, "Answering…") != 0;
    /* A tool that has just been announced keeps the slot for long enough to be
     * read. Only a plain "Thinking" waits its turn - another named tool is
     * real news and replaces it at once. */
    if (!named_tool && voice_tool_active_ && voice_tool_hold_timer_ == nullptr) {
        const uint32_t shown = lv_tick_elaps(voice_tool_shown_at_);
        if (shown < kVoiceToolHoldMs) {
            voice_working_ = strcmp(activity, "Answering…") != 0;
            UpdateVoiceStateCaption("THINKING", kVoiceAmber);
            voice_tool_hold_timer_ = lv_timer_create([](lv_timer_t* timer) {
                auto* self = static_cast<LcdDisplay*>(lv_timer_get_user_data(timer));
                self->ReleaseVoiceToolHold();
                self->voice_tool_active_ = false;
                self->ShowVoiceToolCaption(false);
            }, kVoiceToolHoldMs - shown, this);
            if (voice_tool_hold_timer_ != nullptr) {
                lv_timer_set_repeat_count(voice_tool_hold_timer_, 1);
                return;
            }
        }
    }
    /* Past the hold: whatever was up is either being replaced or is going
     * away, so now it can go. */
    ReleaseVoiceToolHold();
    ClearVoiceToolCaption();
    /* Only a named tool takes the slot: plain "Thinking" is already the state
     * word, and saying it twice on one screen reads as a stutter. */
    voice_tool_active_ = named_tool;
    if (named_tool) voice_tool_shown_at_ = lv_tick_get();
    /* Working covers thinking too - the character should be doing something
     * from the first status right through to the answer, and "Thinking" is
     * the one that arrives first. */
    voice_working_ = strcmp(activity, "Answering…") != 0;
    /* The word underneath stays truthful even while the tool caption covers
     * it, so when a tool finishes the slot falls back to THINKING rather than
     * to whatever was there before the turn started. */
    UpdateVoiceStateCaption("THINKING", kVoiceAmber);
    if (!voice_tool_active_) {
        ShowVoiceToolCaption(false);
        return;
    }

    if (pixels != nullptr && strlen(pixels) == 3072) {
        // One 24px BGRA icon; never decode a downloaded image on the device.
        auto data = static_cast<unsigned char*>(heap_caps_malloc(2304, MALLOC_CAP_8BIT));
        size_t size = 0;
        if (data != nullptr && mbedtls_base64_decode(data, 2304, &size,
                reinterpret_cast<const unsigned char*>(pixels), 3072) == 0 && size == 2304) {
            voice_activity_image_ = std::make_unique<LvglAllocatedImage>(
                data, size, 24, 24, 96, LV_COLOR_FORMAT_ARGB8888);
            voice_tool_icon_ = lv_image_create(voice_root_);
            lv_image_set_src(voice_tool_icon_, voice_activity_image_->image_dsc());
            // 24px artwork into the 20px the caption line leaves for it.
            lv_image_set_scale(voice_tool_icon_, 256 * kVoiceToolIconSize / 24);
            lv_obj_set_size(voice_tool_icon_, kVoiceToolIconSize, kVoiceToolIconSize);
        } else {
            heap_caps_free(data);
        }
    }
    if (voice_tool_icon_ == nullptr && icon != nullptr && strcmp(icon, "search") == 0) {
        voice_tool_icon_ = lv_label_create(voice_root_);
        lv_obj_set_style_text_font(voice_tool_icon_, &BUILTIN_ICON_FONT, 0);
        // Matches the caption beside it, and the state word it stands in for.
        lv_obj_set_style_text_color(voice_tool_icon_, lv_color_hex(kVoiceAmber), 0);
        lv_label_set_text(voice_tool_icon_, MATERIAL_SYMBOLS_SEARCH);
        lv_obj_set_size(voice_tool_icon_, kVoiceToolIconSize, kVoiceToolIconSize);
        lv_obj_set_style_text_align(voice_tool_icon_, LV_TEXT_ALIGN_CENTER, 0);
    }
    UpdateVoiceToolCaption(activity);
    ShowVoiceToolCaption(true);
}

void LcdDisplay::SetVoiceMicrophoneMuted(bool muted) {
    DisplayLockGuard lock(this);
    if (voice_mute_icon_ == nullptr) {
        return;
    }
    lv_label_set_text(voice_mute_icon_, muted ? MATERIAL_SYMBOLS_MIC_OFF : MATERIAL_SYMBOLS_MIC);
    lv_obj_set_style_bg_color(voice_mute_button_, lv_color_hex(muted ? 0xA52C3D : 0x1A1A1A), 0);
    if (Application::GetInstance().GetDeviceState() == kDeviceStateListening) {
        const auto caption = CaptionForDeviceState(kDeviceStateListening, muted);
        UpdateVoiceStateCaption(caption.text, caption.color);
    }
}

void LcdDisplay::SetVoiceCharacter(int shape, int colour) {
    DisplayLockGuard lock(this);
    voice_shape_ = std::clamp(shape, 0, voice_character::kShapeCount - 1);
    voice_colour_ = std::clamp(colour, 0, voice_character::kColorCount - 1);
    RenderVoiceOrb(static_cast<float>(lv_tick_elaps(voice_orb_started_at_)) / 1000.0f);
}

void LcdDisplay::SetStatus(const char* status) {
    DisplayLockGuard lock(this);
    const bool mic_muted = Application::GetInstance().GetAudioService().IsMicrophoneMuted();
    if (!voice_tool_active_ || strcmp(status, Lang::Strings::SPEAKING) == 0 ||
        strcmp(status, Lang::Strings::STANDBY) == 0 ||
        strcmp(status, Lang::Strings::ERROR) == 0) {
        const auto caption = CaptionForDeviceState(Application::GetInstance().GetDeviceState(), mic_muted);
        UpdateVoiceStateCaption(caption.text, caption.color);
    }
    // When muted we must not say "Listening" on the pill, but an active tool
    // caption still owns the pill — don't overwrite it.
    const char* pill_status = status;
    if (mic_muted && strcmp(status, Lang::Strings::LISTENING) == 0) {
        pill_status = "Muted";
    }
    // STANDBY/Error means the call ended, and SPEAKING means the reply is
    // already being read out — either way the tool caption is stale. Without
    // clearing it here the pill keeps showing the last tool caption for the
    // rest of the call, because nothing else ever releases the latch.
    if (strcmp(status, Lang::Strings::STANDBY) == 0 ||
        strcmp(status, Lang::Strings::ERROR) == 0 ||
        strcmp(status, Lang::Strings::SPEAKING) == 0) {
        ReleaseVoiceToolHold();
        voice_tool_active_ = false;
        voice_working_ = false;
    }
    if (!voice_tool_active_) {
        LvglDisplay::SetStatus(pill_status);
        ShowVoiceToolCaption(false);
    }
    if (emoji_box_ == nullptr) {
        return;
    }
    const bool listening = strcmp(status, Lang::Strings::LISTENING) == 0;
    const bool speaking = strcmp(status, Lang::Strings::SPEAKING) == 0;
    const bool connecting = strcmp(status, Lang::Strings::CONNECTING) == 0;
    // Mute affects INPUT only. Output (SPEAKING) and the connecting handshake
    // keep pulsing; only LISTENING-with-mic-muted goes dim.
    const bool orb_active = speaking || connecting || (listening && !mic_muted);
    if (watch_ui_) watch_ui_->SetCallActive(speaking || listening || connecting);
    if (voice_mute_button_ != nullptr && voice_end_button_ != nullptr) {
        const bool connected = listening || speaking;
        if (connected) {
            lv_obj_remove_flag(voice_mute_button_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(voice_end_button_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(voice_mute_button_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(voice_end_button_, LV_OBJ_FLAG_HIDDEN);
        }
    }
    // Entering the handshake starts orbit; leaving it - connected, or the user
    // gave up - starts the exit that takes the rings away.
    if (connecting != voice_orb_connecting_) {
        if (connecting) voice_orbit_started_at_ = lv_tick_get();
        voice_orbit_exit_at_ = connecting ? 0 : lv_tick_get();
        voice_orb_connecting_ = connecting;
    }
    const uint32_t orb_color = strcmp(status, Lang::Strings::ERROR) == 0 ? 0xCF4B59 : 0x7465EB;
    const bool was_orb_active = voice_orb_active_;
    voice_orb_color_ = orb_color;
    voice_orb_active_ = orb_active;
    lv_obj_set_style_bg_opa(emoji_box_, LV_OPA_TRANSP, 0);
    if (orb_active) {
        if (!was_orb_active) voice_orb_started_at_ = lv_tick_get();
        if (voice_orb_timer_ != nullptr) lv_timer_resume(voice_orb_timer_);
        if (voice_orb_canvas_ != nullptr) {
            RenderVoiceOrb(static_cast<float>(lv_tick_elaps(voice_orb_started_at_)) / 1000.0f);
            lv_obj_set_style_image_opa(voice_orb_canvas_, LV_OPA_COVER, 0);
        }
    } else {
        // A handshake that ended without a call - cancelled, or failed - drops
        // the rings rather than leaving an exit half-run.
        voice_orbit_exit_at_ = 0;
        // Between calls he is still alive: blinking, and his gaze drifting.
        // The timer keeps running and he stays at full strength - the old
        // half-transparent idle read as the screen being dimmed, not as him
        // waiting. It costs nothing when the call screen is not up, because
        // the timer skips a hidden screen, and the display sleeps on its own.
        if (voice_orb_timer_ != nullptr) lv_timer_resume(voice_orb_timer_);
        if (voice_orb_canvas_ != nullptr) {
            RenderVoiceOrb(static_cast<float>(lv_tick_elaps(voice_orb_started_at_)) / 1000.0f);
            lv_obj_set_style_image_opa(voice_orb_canvas_, LV_OPA_COVER, 0);
        }
    }
}

/* Walks the cycle on. Returns true while a pose other than the plain resting
 * character is on screen - which is the whole cycle, plus the cross-fade back
 * into idle after the agent has answered. */
bool LcdDisplay::AdvanceWorkingCycle() {
    const auto now = static_cast<bloub_state_id_t>(voice_cycle_state_);
    const float held = static_cast<float>(lv_tick_elaps(voice_cycle_started_at_)) / 1000.0f;
    if (voice_working_) {
        if (now == BLOUB_STATE_IDLE || held >= bloub_state_duration(now)) {
            /* Snapshot the state being left at the moment it ends: the
             * cross-fade reads from it while it is frozen there. */
            bloub_pose_sample(now, held, SHAPE_PROFILES[voice_shape_], &s_pose_leaving);
            voice_cycle_index_ = static_cast<uint8_t>((voice_cycle_index_ + 1) % kWorkingCycleLength);
            voice_cycle_state_ = static_cast<uint8_t>(kWorkingCycle[voice_cycle_index_]);
            voice_cycle_started_at_ = lv_tick_get();
        }
        return true;
    }
    if (now != BLOUB_STATE_IDLE) {
        bloub_pose_sample(now, held, SHAPE_PROFILES[voice_shape_], &s_pose_leaving);
        voice_cycle_state_ = static_cast<uint8_t>(BLOUB_STATE_IDLE);
        voice_cycle_started_at_ = lv_tick_get();
        return true;
    }
    /* Idle, and the fade into it has finished: nothing left to do. */
    return held < bloub_state_morph(BLOUB_STATE_IDLE);
}

void LcdDisplay::RenderVoiceOrb(float seconds) {
    if (voice_orb_canvas_ == nullptr || voice_orb_buffer_ == nullptr) return;

    /* The character, not a fluid gradient. Ported from bloub (see
     * main/display/bloub/) and drawn into the same 166px canvas the orb used.
     * White on black keeps both colours swap-invariant, so nothing here has to
     * care how the panel orders its 16-bit words. */
    const int size = voice_geometry::kOrbSize;
    const uint16_t body = lv_color_to_u16(lv_color_hex(voice_character::kColors[voice_colour_]));
    const uint16_t back = lv_color_to_u16(lv_color_hex(0x000000));

    /* Where orbit is: running all through the handshake, then handed back over
     * bloub's own cross-fade once the call is up. */
    bloub_orbit_t orbit_state{0.0f, 1.0f};
    if (voice_orb_connecting_ || voice_orbit_exit_at_ != 0) {
        orbit_state.t = static_cast<float>(lv_tick_elaps(voice_orbit_started_at_)) / 1000.0f;
        orbit_state.exit = 0.0f;
    }
    if (!voice_orb_connecting_ && voice_orbit_exit_at_ != 0) {
        const uint32_t elapsed = lv_tick_elaps(voice_orbit_exit_at_);
        if (elapsed >= kOrbitExitMs) { voice_orbit_exit_at_ = 0; orbit_state.exit = 1.0f; }
        else orbit_state.exit = static_cast<float>(elapsed) / static_cast<float>(kOrbitExitMs);
    }
    const bool orbiting = orbit_state.exit < 1.0f;

    /* Idle life: the blink schedule and the gaze drift, both pure functions of
     * the time this screen has been up. */
    const bloub_liveliness_t life = bloub_liveliness(seconds, 1.0f, true, true);
    /* Facing the user. bloub's rest gaze is a three-quarter view measured off
     * the reference video, which reads as looking off to one side on a device
     * that is meant to be looking at whoever is in front of it. */
    static const bloub_gaze_t kAttentive = { 4.0f, 5.0f, -4.0f };
    bloub_gaze_t gaze = kAttentive;
    gaze.yaw += life.d_yaw;
    gaze.pitch += life.d_pitch;
    gaze.roll += life.d_roll;

    /* Connecting: the body turns and the eyes run round the sphere with it,
     * both easing back to the resting face as the call comes up. */
    bloub_orbit_pose_t spin{};
    if (orbiting) bloub_orbit_pose(&orbit_state, gaze, &spin);

    /* While the agent is working he runs the cycle: one state held for its
     * measured time, then a cross-fade into the next. When the answer comes
     * the cycle ends on idle, which is the same cross-fade - so the way back
     * to his resting face needs no special case. Kept on the LVGL task, which
     * is the only thing that calls this. */
    static bloub_pose_t pose_cur, pose_shown;
    const bool cycling = !orbiting && AdvanceWorkingCycle();
    if (cycling) {
        const auto now = static_cast<bloub_state_id_t>(voice_cycle_state_);
        const float held = static_cast<float>(lv_tick_elaps(voice_cycle_started_at_)) / 1000.0f;
        bloub_pose_sample(now, held, SHAPE_PROFILES[voice_shape_], &pose_cur);
        const float morph = bloub_state_morph(now);
        if (held < morph) {
            bloub_pose_blend(&s_pose_leaving, &pose_cur,
                             bloub_ease_out_quint(held / morph), &pose_shown);
        } else {
            pose_shown = pose_cur;
        }
        /* The idle life rides on top of whatever the state is doing, except
         * where the state has put the eyes away. */
        pose_shown.gaze.yaw += life.d_yaw;
        pose_shown.gaze.pitch += life.d_pitch;
        pose_shown.gaze.roll += life.d_roll;
        gaze = pose_shown.gaze;
    }

    bloub_face_cfg_t face;
    memset(&face, 0, sizeof(face));
    face.radii = cycling ? pose_shown.radii : SHAPE_PROFILES[voice_shape_];
    face.gaze = &gaze;
    face.split = cycling ? pose_shown.split : 16.0f;
    if (orbiting) { face.rot = spin.rot; gaze = spin.gaze; }
    /* Nearly filling its canvas: bloub's face is the whole device, not a small
     * puck in the middle of one. It only shrinks to make room for the rings,
     * and comes back up on the same cross-fade that unwinds the spin, so the
     * size, the turn and the gaze all land together. */
    const float shrink = orbiting
        ? kOrbitScale + (1.0f - kOrbitScale) * bloub_ease_out_quint(orbit_state.exit)
        : 1.0f;
    const float ball = static_cast<float>(size) * 0.46f * shrink;
    face.scale = ball;
    face.cx = face.cy = static_cast<float>(size) * 0.5f;
    face.sx = face.sy = 1.0f;
    face.eye_alpha = 1.0f;
    if (cycling) {
        face.rot = pose_shown.rot;
        face.cx += pose_shown.cx * ball;
        face.cy += pose_shown.cy * ball;
        face.sx = pose_shown.sx;
        face.sy = pose_shown.sy;
        face.eye_alpha = pose_shown.eye_alpha;
    }
    for (int e = 0; e < 2; e++) {
        face.eyes[e].w = cycling ? pose_shown.eyes[e].w : 0.21f;
        face.eyes[e].h = orbiting ? spin.eye_h : (cycling ? pose_shown.eyes[e].h : 0.44f);
        face.eyes[e].open = bloub_blink_scale(life.lid) * (cycling ? pose_shown.eyes[e].open : 1.0f);
    }

    auto* pixels = reinterpret_cast<uint16_t*>(voice_orb_buffer_);
    memset(voice_orb_buffer_, 0, sizeof(lv_color16_t) * static_cast<size_t>(size) * size);
    /* The far half of the rings, then the body over them, then the near half:
     * that is what makes a ring pass behind the character and come back round
     * in front of it. */
    if (orbiting) bloub_orbit_draw(pixels, size, size, &orbit_state, ball, face.cx, face.cy, true);
    if (cycling) bloub_pose_draw_arcs(pixels, size, size, &pose_shown, ball, face.cx, face.cy, true);
    bloub_draw_face(pixels, size, size, &face, body, back);
    if (orbiting) bloub_orbit_draw(pixels, size, size, &orbit_state, ball, face.cx, face.cy, false);
    if (cycling) {
        bloub_pose_draw_arcs(pixels, size, size, &pose_shown, ball, face.cx, face.cy, false);
        bloub_pose_draw_dots(pixels, size, size, &pose_shown, ball, face.cx, face.cy, body);
    }
    lv_obj_invalidate(voice_orb_canvas_);
}

void LcdDisplay::SetEmotion(const char* emotion) {
    return;
    if (!setup_ui_called_) {
        ESP_LOGW(TAG, "SetEmotion('%s') called before SetupUI() - emotion will not be displayed!",
                 emotion);
    }
    if (emoji_image_ == nullptr) {
        if (setup_ui_called_) {
            ESP_LOGW(TAG,
                     "SetEmotion('%s') failed: emoji_image_ is nullptr (SetupUI() was called but "
                     "emoji image not created)",
                     emotion);
        }
        return;
    }

    auto emoji_collection = static_cast<LvglTheme*>(current_theme_)->emoji_collection();
    auto image = emoji_collection != nullptr ? emoji_collection->GetEmojiImage(emotion) : nullptr;
    if (image == nullptr) {
        auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
        const char* utf8 = noto_emoji_get_utf8(emotion);
        const lv_font_t* emotion_font = lvgl_theme->emoji_font()->font();
        if (utf8 == nullptr) {
            utf8 = material_symbols_get_utf8(emotion);
            emotion_font = lvgl_theme->large_icon_font()->font();
        }
        if (utf8 != nullptr && emoji_label_ != nullptr) {
            DisplayLockGuard lock(this);
            if (gif_controller_) {
                gif_controller_->Stop();
                gif_controller_.reset();
            }
            lv_obj_set_style_text_font(emoji_label_, emotion_font, 0);
            lv_label_set_text(emoji_label_, utf8);
            lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    DisplayLockGuard lock(this);
    // Stop any running GIF animation in the same lock scope as setting new image
    // to prevent LVGL from accessing freed image data between operations
    if (gif_controller_) {
        gif_controller_->Stop();
        gif_controller_.reset();
    }
    if (image->IsGif()) {
        // Create new GIF controller
        gif_controller_ = std::make_unique<LvglGif>(image->image_dsc());

        if (gif_controller_->IsLoaded()) {
            // Set up frame update callback
            gif_controller_->SetFrameCallback(
                [this]() { lv_image_set_src(emoji_image_, gif_controller_->image_dsc()); });

            // Set initial frame and start animation
            lv_image_set_src(emoji_image_, gif_controller_->image_dsc());
            gif_controller_->Start();

            // Show GIF, hide others
            lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
        } else {
            ESP_LOGE(TAG, "Failed to load GIF for emotion: %s", emotion);
            gif_controller_.reset();
        }
    } else {
        lv_image_set_src(emoji_image_, image->image_dsc());
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
    }

}

void LcdDisplay::SetTheme(Theme* theme) {
    DisplayLockGuard lock(this);

    auto lvgl_theme = static_cast<LvglTheme*>(theme);

    // Get the active screen
    lv_obj_t* screen = lv_screen_active();

    // Set font
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();
    auto large_icon_font = lvgl_theme->large_icon_font()->font();

    if (voice_root_ != nullptr) lv_obj_set_style_text_font(voice_root_, text_font, 0);
    if (watch_ui_ != nullptr) watch_ui_->SetFont(lvgl_theme->text_font());

    if (text_font->line_height >= 40) {
        lv_obj_set_style_text_font(mute_label_, large_icon_font, 0);
        lv_obj_set_style_text_font(battery_label_, large_icon_font, 0);
        lv_obj_set_style_text_font(network_label_, large_icon_font, 0);
    } else {
        lv_obj_set_style_text_font(mute_label_, icon_font, 0);
        lv_obj_set_style_text_font(battery_label_, icon_font, 0);
        lv_obj_set_style_text_font(network_label_, icon_font, 0);
    }

    // Set parent text color
    lv_obj_set_style_text_font(screen, text_font, 0);
    lv_obj_set_style_text_color(screen, lvgl_theme->text_color(), 0);

    // Set background image
    if (lvgl_theme->background_image() != nullptr) {
        lv_obj_set_style_bg_image_src(container_, lvgl_theme->background_image()->image_dsc(), 0);
    } else {
        lv_obj_set_style_bg_image_src(container_, nullptr, 0);
        lv_obj_set_style_bg_color(container_, lvgl_theme->background_color(), 0);
    }

    // Update top bar background color with 50% opacity
    if (top_bar_ != nullptr) {
        lv_obj_set_style_bg_opa(top_bar_, LV_OPA_50, 0);
        lv_obj_set_style_bg_color(top_bar_, lvgl_theme->background_color(), 0);
    }

    // Update status bar elements
    lv_obj_set_style_text_color(network_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_text_color(status_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_text_color(notification_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_text_color(mute_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_text_color(battery_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_text_color(emoji_label_, lvgl_theme->text_color(), 0);

    // If we have the chat message style, update all message bubbles
    // Simple UI mode - just update the main chat message
    if (chat_message_label_ != nullptr) {
        lv_obj_set_style_text_color(chat_message_label_, lvgl_theme->text_color(), 0);
    }

    if (emoji_label_ != nullptr) {
        lv_obj_set_style_text_color(emoji_label_, lvgl_theme->text_color(), 0);
    }

    // Update bottom bar background color with 50% opacity
    if (bottom_bar_ != nullptr) {
        lv_obj_set_style_bg_opa(bottom_bar_, LV_OPA_50, 0);
        lv_obj_set_style_bg_color(bottom_bar_, lvgl_theme->background_color(), 0);
    }

    // Update low battery popup
    lv_obj_set_style_bg_color(low_battery_popup_, lvgl_theme->low_battery_color(), 0);

    lv_obj_set_style_bg_color(container_, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_image_src(container_, nullptr, 0);
    lv_obj_set_style_text_color(voice_root_, lv_color_white(), 0);
    lv_obj_set_style_text_color(status_label_, lv_color_white(), 0);
    lv_obj_set_style_text_color(chat_message_label_, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(bottom_bar_, LV_OPA_TRANSP, 0);
    // No errors occurred. Save theme to settings
    Display::SetTheme(lvgl_theme);
}

void LcdDisplay::SetHideSubtitle(bool hide) {
    DisplayLockGuard lock(this);
    hide_subtitle_ = hide;

    // Immediately update UI visibility based on the setting
    if (bottom_bar_ != nullptr) {
        if (hide) {
            lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        } else {
            // Only show if there is actual content to display
            const char* text =
                (chat_message_label_ != nullptr) ? lv_label_get_text(chat_message_label_) : nullptr;
            if (text != nullptr && text[0] != '\0') {
                lv_obj_remove_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

void LcdDisplay::FeedTouch(bool pressed, int x, int y) {
    touch_sample_.store((static_cast<uint32_t>(pressed) << 18) |
                       (static_cast<uint32_t>(std::clamp(y, 0, 359)) << 9) |
                       static_cast<uint32_t>(std::clamp(x, 0, 359)));
}
void LcdDisplay::ShowVoicePage() {
    DisplayLockGuard lock(this);
    if (watch_ui_) watch_ui_->Show(WatchUi::Page::Voice);
}
void LcdDisplay::UpdateWatchInfo(const WatchUi::Info& info) {
    DisplayLockGuard lock(this);
    if (watch_ui_) watch_ui_->SetInfo(info);
}
void LcdDisplay::UpdateStatusBar(bool update_all) {
    LvglDisplay::UpdateStatusBar(update_all);
    DisplayLockGuard lock(this);
    time_t now = time(nullptr); struct tm tm{}; localtime_r(&now, &tm);
    char clock[16] = "--:--", date[48] = "Waiting for network time";
    if (tm.tm_year >= 124) { strftime(clock, sizeof(clock), "%H:%M", &tm); strftime(date, sizeof(date), "%a, %b %d", &tm); }
    if (voice_clock_) lv_label_set_text(voice_clock_, clock);
    if (watch_ui_) watch_ui_->Tick(clock, date);
}
void LcdDisplay::ShowConfirmScreen(const char* summary) {
    DisplayLockGuard lock(this);
    if (confirm_root_ == nullptr) {
        confirm_root_ = lv_obj_create(lv_screen_active());
        lv_obj_set_size(confirm_root_, 360, 360);
        lv_obj_set_pos(confirm_root_, 0, 0);
        lv_obj_set_style_bg_color(confirm_root_, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(confirm_root_, LV_OPA_70, 0);
        lv_obj_set_style_border_width(confirm_root_, 0, 0);
        lv_obj_set_style_pad_all(confirm_root_, 0, 0);
        lv_obj_remove_flag(confirm_root_, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
        confirm_summary_ = lv_label_create(confirm_root_);
        lv_obj_set_size(confirm_summary_, confirm_geometry::kSummaryWidth, confirm_geometry::kSummaryHeight);
        lv_label_set_long_mode(confirm_summary_, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(confirm_summary_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(confirm_summary_, lv_color_hex(confirm_geometry::kSummaryTextColor), 0);
        lv_obj_set_pos(confirm_summary_, (360 - confirm_geometry::kSummaryWidth) / 2, confirm_geometry::kSummaryOffsetY);
        confirm_reject_btn_ = lv_obj_create(confirm_root_);
        lv_obj_set_size(confirm_reject_btn_, confirm_geometry::kButtonWidth, confirm_geometry::kButtonHeight);
        lv_obj_set_pos(confirm_reject_btn_, confirm_geometry::kRejectButtonOffsetX, confirm_geometry::kButtonOffsetY);
        lv_obj_set_style_radius(confirm_reject_btn_, 20, 0);
        lv_obj_set_style_bg_color(confirm_reject_btn_, lv_color_hex(confirm_geometry::kRejectBackgroundColor), 0);
        lv_obj_set_style_border_width(confirm_reject_btn_, 0, 0);
        lv_obj_remove_flag(confirm_reject_btn_, LV_OBJ_FLAG_CLICKABLE);
        auto reject_label = lv_label_create(confirm_reject_btn_);
        lv_label_set_text(reject_label, "Reject");
        lv_obj_set_style_text_color(reject_label, lv_color_hex(confirm_geometry::kButtonTextColor), 0);
        lv_obj_center(reject_label);
        confirm_approve_btn_ = lv_obj_create(confirm_root_);
        lv_obj_set_size(confirm_approve_btn_, confirm_geometry::kButtonWidth, confirm_geometry::kButtonHeight);
        lv_obj_set_pos(confirm_approve_btn_, confirm_geometry::kApproveButtonOffsetX, confirm_geometry::kButtonOffsetY);
        lv_obj_set_style_radius(confirm_approve_btn_, 20, 0);
        lv_obj_set_style_bg_color(confirm_approve_btn_, lv_color_hex(confirm_geometry::kApproveBackgroundColor), 0);
        lv_obj_set_style_border_width(confirm_approve_btn_, 0, 0);
        lv_obj_remove_flag(confirm_approve_btn_, LV_OBJ_FLAG_CLICKABLE);
        auto approve_label = lv_label_create(confirm_approve_btn_);
        lv_label_set_text(approve_label, "Approve");
        lv_obj_set_style_text_color(approve_label, lv_color_hex(confirm_geometry::kButtonTextColor), 0);
        lv_obj_center(approve_label);
    }
    lv_label_set_text(confirm_summary_, summary ? summary : "");
    lv_obj_remove_flag(confirm_root_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(confirm_root_);
}
void LcdDisplay::HideConfirmScreen() {
    DisplayLockGuard lock(this);
    if (confirm_root_ != nullptr) {
        lv_obj_add_flag(confirm_root_, LV_OBJ_FLAG_HIDDEN);
    }
}
