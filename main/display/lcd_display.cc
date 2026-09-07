#include "lcd_display.h"
#include "assets/lang_config.h"
#include "gif/lvgl_gif.h"
#include "lvgl_theme.h"
#include "settings.h"
#include "voice_geometry.h"
#include "confirm_geometry.h"
#include "watch_icons.h"

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
#ifdef CONFIG_APOLLO_CODEX_VOICE
#include "application.h"
#include <esp_heap_caps.h>
#include <mbedtls/base64.h>
#endif

#define TAG "LcdDisplay"

LV_FONT_DECLARE(BUILTIN_TEXT_FONT);
LV_FONT_DECLARE(BUILTIN_ICON_FONT);
LV_FONT_DECLARE(font_material_symbols_30_4);
LV_FONT_DECLARE(font_noto_emoji_30_4);

#ifdef CONFIG_APOLLO_CODEX_VOICE
namespace {
// Fluid shading ported from Rare UI's Fluid Orb: https://www.rareui.com/components/fluidorb
constexpr uint32_t kFluidOrbFramePeriodMs = 66;
constexpr int kFluidOrbSampleStep = 2;

float FluidOrbMix(float first, float second, float amount) {
    return first + (second - first) * amount;
}

float FluidOrbSmoothStep(float edge0, float edge1, float value) {
    const float amount = std::clamp((value - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return amount * amount * (3.0f - 2.0f * amount);
}

float FluidOrbHash(float x, float y) {
    // The browser shader uses sine here; an integer hash keeps the same smooth noise field
    // without hundreds of thousands of trig calls per frame on the ESP32.
    const uint32_t ix = static_cast<uint32_t>(static_cast<int32_t>(x));
    const uint32_t iy = static_cast<uint32_t>(static_cast<int32_t>(y));
    uint32_t value = ix * 374761393u + iy * 668265263u;
    value = (value ^ (value >> 13)) * 1274126177u;
    value ^= value >> 16;
    return static_cast<float>(value) / 4294967295.0f;
}

float FluidOrbNoise(float x, float y) {
    const float ix = std::floor(x);
    const float iy = std::floor(y);
    const float fx = x - ix;
    const float fy = y - iy;
    const float ux = fx * fx * (3.0f - 2.0f * fx);
    const float uy = fy * fy * (3.0f - 2.0f * fy);
    const float lower = FluidOrbMix(FluidOrbHash(ix, iy), FluidOrbHash(ix + 1.0f, iy), ux);
    const float upper = FluidOrbMix(FluidOrbHash(ix, iy + 1.0f),
                                    FluidOrbHash(ix + 1.0f, iy + 1.0f), ux);
    return FluidOrbMix(lower, upper, uy);
}

float FluidOrbFbm(float x, float y) {
    float value = 0.0f;
    float amplitude = 0.6f;
    for (int octave = 0; octave < 3; ++octave) {
        value += amplitude * FluidOrbNoise(x, y);
        x *= 2.0f;
        y *= 2.0f;
        amplitude *= 0.5f;
    }
    return value;
}
}  // namespace
#endif

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
#ifdef CONFIG_APOLLO_CODEX_VOICE
    theme_name = "dark";
#endif
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

#ifdef CONFIG_APOLLO_CODEX_VOICE
    if (touch_input_) lv_indev_delete(touch_input_);
    watch_ui_.reset();
    if (voice_orb_timer_ != nullptr) {
        lv_timer_delete(voice_orb_timer_);
        voice_orb_timer_ = nullptr;
    }
#endif

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
#ifdef CONFIG_APOLLO_CODEX_VOICE
    if (voice_orb_buffer_ != nullptr) {
        heap_caps_free(voice_orb_buffer_);
        voice_orb_buffer_ = nullptr;
    }
#endif
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

#if CONFIG_USE_WECHAT_MESSAGE_STYLE
void LcdDisplay::SetupUI() {
    // Prevent duplicate calls - if already called, return early
    if (setup_ui_called_) {
        ESP_LOGW(TAG, "SetupUI() called multiple times, skipping duplicate call");
        return;
    }

    Display::SetupUI();  // Mark SetupUI as called
    DisplayLockGuard lock(this);

    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();
    auto large_icon_font = lvgl_theme->large_icon_font()->font();

    auto screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, text_font, 0);
    lv_obj_set_style_text_color(screen, lvgl_theme->text_color(), 0);
    lv_obj_set_style_bg_color(screen, lvgl_theme->background_color(), 0);

    /* Container */
    container_ = lv_obj_create(screen);
    lv_obj_set_size(container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_radius(container_, 0, 0);
    lv_obj_set_flex_flow(container_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_pad_row(container_, 0, 0);
    lv_obj_set_style_bg_color(container_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_border_color(container_, lvgl_theme->border_color(), 0);

    /* Layer 1: Top bar - for status icons */
    top_bar_ = lv_obj_create(container_);
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
    lv_obj_set_width(notification_label_, LV_HOR_RES * 0.8);
    lv_obj_set_style_text_align(notification_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(notification_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(notification_label_, "");
    lv_obj_align(notification_label_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    status_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(status_label_, LV_HOR_RES * 0.8);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(status_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(status_label_, Lang::Strings::INITIALIZING);
    lv_obj_align(status_label_, LV_ALIGN_CENTER, 0, 0);

    /* Content - Chat area */
    content_ = lv_obj_create(container_);
    lv_obj_set_style_radius(content_, 0, 0);
    lv_obj_set_width(content_, LV_HOR_RES);
    lv_obj_set_flex_grow(content_, 1);
    lv_obj_set_style_pad_all(content_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_border_width(content_, 0, 0);
    lv_obj_set_style_bg_color(content_, lvgl_theme->chat_background_color(),
                              0);  // Background for chat area

    // Enable scrolling for chat content
    lv_obj_set_scrollbar_mode(content_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(content_, LV_DIR_VER);

    // Create a flex container for chat messages
    lv_obj_set_flex_flow(content_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(content_, lvgl_theme->spacing(4), 0);  // Space between messages

    // We'll create chat messages dynamically in SetChatMessage
    chat_message_label_ = nullptr;

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

    emoji_image_ = lv_img_create(screen);
    lv_obj_align(emoji_image_, LV_ALIGN_TOP_MID, 0,
                 text_font->line_height + lvgl_theme->spacing(8));

    // Display AI logo while booting
    emoji_label_ = lv_label_create(screen);
    lv_obj_center(emoji_label_);
    lv_obj_set_style_text_font(emoji_label_, large_icon_font, 0);
    lv_obj_set_style_text_color(emoji_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(emoji_label_, MATERIAL_SYMBOLS_ROBOT_2);
}
#if CONFIG_IDF_TARGET_ESP32P4
#define MAX_MESSAGES 40
#else
#define MAX_MESSAGES 20
#endif
void LcdDisplay::SetChatMessage(const char* role, const char* content) {
    if (!setup_ui_called_) {
        ESP_LOGW(TAG, "SetChatMessage('%s', '%s') called before SetupUI() - message will be lost!",
                 role, content);
    }
    DisplayLockGuard lock(this);
    if (content_ == nullptr) {
        if (setup_ui_called_) {
            ESP_LOGW(TAG,
                     "SetChatMessage('%s', '%s') failed: content_ is nullptr (SetupUI() was called "
                     "but container not created)",
                     role, content);
        }
        return;
    }

    // Check if message count exceeds limit
    uint32_t child_count = lv_obj_get_child_cnt(content_);
    if (child_count >= MAX_MESSAGES) {
        // Delete the oldest message (first child object)
        lv_obj_t* first_child = lv_obj_get_child(content_, 0);
        if (first_child != nullptr) {
            lv_obj_del(first_child);
            // Refresh child count after deletion
            child_count = lv_obj_get_child_cnt(content_);
        }
        // Scroll to the last message immediately (get last_child after deletion)
        if (child_count > 0) {
            lv_obj_t* last_child = lv_obj_get_child(content_, child_count - 1);
            if (last_child != nullptr && lv_obj_is_valid(last_child)) {
                lv_obj_scroll_to_view_recursive(last_child, LV_ANIM_OFF);
            }
        }
    }

    // Collapse system messages (if it's a system message, check if the last message is also a
    // system message)
    if (strcmp(role, "system") == 0) {
        // Refresh child count to get accurate count after potential deletion above
        child_count = lv_obj_get_child_cnt(content_);
        if (child_count > 0) {
            // Get the last message container
            lv_obj_t* last_container = lv_obj_get_child(content_, child_count - 1);
            if (last_container != nullptr && lv_obj_is_valid(last_container) &&
                lv_obj_get_child_cnt(last_container) > 0) {
                // Get the bubble inside the container
                lv_obj_t* last_bubble = lv_obj_get_child(last_container, 0);
                if (last_bubble != nullptr && lv_obj_is_valid(last_bubble)) {
                    // Check if bubble type is system message
                    void* bubble_type_ptr = lv_obj_get_user_data(last_bubble);
                    if (bubble_type_ptr != nullptr &&
                        strcmp((const char*)bubble_type_ptr, "system") == 0) {
                        // If the last message is also a system message, delete it
                        lv_obj_del(last_container);
                    }
                }
            }
        }
    } else {
        // Hide the centered AI logo
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    }

    // Avoid empty message boxes
    if (strlen(content) == 0) {
        return;
    }

    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);

    // Create a message bubble
    lv_obj_t* msg_bubble = lv_obj_create(content_);
    lv_obj_set_style_radius(msg_bubble, 8, 0);
    lv_obj_set_scrollbar_mode(msg_bubble, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_border_width(msg_bubble, 0, 0);
    lv_obj_set_style_pad_all(msg_bubble, lvgl_theme->spacing(4), 0);

    // Create the message text
    lv_obj_t* msg_text = lv_label_create(msg_bubble);
    lv_label_set_text(msg_text, content);

    // Calculate bubble width constraints
    lv_coord_t max_width = LV_HOR_RES * 85 / 100 - 16;  // 85% of screen width
    lv_coord_t min_width = 20;

    // Let LVGL calculate the natural text width first
    lv_obj_set_width(msg_text, LV_SIZE_CONTENT);
    lv_obj_update_layout(msg_text);
    lv_coord_t text_width = lv_obj_get_width(msg_text);

    // Ensure text width is not less than minimum width
    if (text_width < min_width) {
        text_width = min_width;
    }

    // Constrain to max width
    lv_coord_t bubble_width = (text_width < max_width) ? text_width : max_width;

    // Set message text width
    lv_obj_set_width(msg_text, bubble_width);
    lv_label_set_long_mode(msg_text, LV_LABEL_LONG_WRAP);

    // Set bubble width
    lv_obj_set_width(msg_bubble, bubble_width);
    lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);

    // Set alignment and style based on message role
    if (strcmp(role, "user") == 0) {
        // User messages are right-aligned with green background
        lv_obj_set_style_bg_color(msg_bubble, lvgl_theme->user_bubble_color(), 0);
        lv_obj_set_style_bg_opa(msg_bubble, LV_OPA_70, 0);
        // Set text color for contrast
        lv_obj_set_style_text_color(msg_text, lvgl_theme->text_color(), 0);

        // Set custom attribute to mark bubble type
        lv_obj_set_user_data(msg_bubble, (void*)"user");

        // Set appropriate width for content
        lv_obj_set_width(msg_bubble, LV_SIZE_CONTENT);
        lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);

        // Don't grow
        lv_obj_set_style_flex_grow(msg_bubble, 0, 0);
    } else if (strcmp(role, "assistant") == 0) {
        // Assistant messages are left-aligned with white background
        lv_obj_set_style_bg_color(msg_bubble, lvgl_theme->assistant_bubble_color(), 0);
        lv_obj_set_style_bg_opa(msg_bubble, LV_OPA_70, 0);
        // Set text color for contrast
        lv_obj_set_style_text_color(msg_text, lvgl_theme->text_color(), 0);

        // Set custom attribute to mark bubble type
        lv_obj_set_user_data(msg_bubble, (void*)"assistant");

        // Set appropriate width for content
        lv_obj_set_width(msg_bubble, LV_SIZE_CONTENT);
        lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);

        // Don't grow
        lv_obj_set_style_flex_grow(msg_bubble, 0, 0);
    } else if (strcmp(role, "system") == 0) {
        // System messages are center-aligned with light gray background
        lv_obj_set_style_bg_color(msg_bubble, lvgl_theme->system_bubble_color(), 0);
        lv_obj_set_style_bg_opa(msg_bubble, LV_OPA_70, 0);
        // Set text color for contrast
        lv_obj_set_style_text_color(msg_text, lvgl_theme->system_text_color(), 0);

        // Set custom attribute to mark bubble type
        lv_obj_set_user_data(msg_bubble, (void*)"system");

        // Set appropriate width for content
        lv_obj_set_width(msg_bubble, LV_SIZE_CONTENT);
        lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);

        // Don't grow
        lv_obj_set_style_flex_grow(msg_bubble, 0, 0);
    }

    // Create a full-width container for user messages to ensure right alignment
    if (strcmp(role, "user") == 0) {
        // Create a full-width container
        lv_obj_t* container = lv_obj_create(content_);
        lv_obj_set_width(container, LV_HOR_RES);
        lv_obj_set_height(container, LV_SIZE_CONTENT);

        // Make container transparent and borderless
        lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(container, 0, 0);
        lv_obj_set_style_pad_all(container, 0, 0);

        // Move the message bubble into this container
        lv_obj_set_parent(msg_bubble, container);

        // Right align the bubble in the container
        lv_obj_align(msg_bubble, LV_ALIGN_RIGHT_MID, -25, 0);

        // Auto-scroll to this container
        lv_obj_scroll_to_view_recursive(container, LV_ANIM_ON);
    } else if (strcmp(role, "system") == 0) {
        // Create full-width container for system messages to ensure center alignment
        lv_obj_t* container = lv_obj_create(content_);
        lv_obj_set_width(container, LV_HOR_RES);
        lv_obj_set_height(container, LV_SIZE_CONTENT);

        lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(container, 0, 0);
        lv_obj_set_style_pad_all(container, 0, 0);

        lv_obj_set_parent(msg_bubble, container);
        lv_obj_align(msg_bubble, LV_ALIGN_CENTER, 0, 0);
        lv_obj_scroll_to_view_recursive(container, LV_ANIM_ON);
    } else {
        // For assistant messages
        // Left align assistant messages
        lv_obj_align(msg_bubble, LV_ALIGN_LEFT_MID, 0, 0);

        // Auto-scroll to the message bubble
        lv_obj_scroll_to_view_recursive(msg_bubble, LV_ANIM_ON);
    }

    // Store reference to the latest message label
    chat_message_label_ = msg_text;
}

void LcdDisplay::SetPreviewImage(std::unique_ptr<LvglImage> image) {
    DisplayLockGuard lock(this);
    if (content_ == nullptr) {
        return;
    }

    if (image == nullptr) {
        return;
    }

    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    // Create a message bubble for image preview
    lv_obj_t* img_bubble = lv_obj_create(content_);
    lv_obj_set_style_radius(img_bubble, 8, 0);
    lv_obj_set_scrollbar_mode(img_bubble, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_border_width(img_bubble, 0, 0);
    lv_obj_set_style_pad_all(img_bubble, lvgl_theme->spacing(4), 0);

    // Set image bubble background color (similar to system message)
    lv_obj_set_style_bg_color(img_bubble, lvgl_theme->assistant_bubble_color(), 0);
    lv_obj_set_style_bg_opa(img_bubble, LV_OPA_70, 0);

    // Set custom attribute to mark bubble type
    lv_obj_set_user_data(img_bubble, (void*)"image");

    // Create the image object inside the bubble
    lv_obj_t* preview_image = lv_image_create(img_bubble);

    // Calculate appropriate size for the image
    lv_coord_t max_width = LV_HOR_RES * 70 / 100;   // 70% of screen width
    lv_coord_t max_height = LV_VER_RES * 50 / 100;  // 50% of screen height

    // Calculate zoom factor to fit within maximum dimensions
    auto img_dsc = image->image_dsc();
    lv_coord_t img_width = img_dsc->header.w;
    lv_coord_t img_height = img_dsc->header.h;
    if (img_width == 0 || img_height == 0) {
        img_width = max_width;
        img_height = max_height;
        ESP_LOGW(TAG, "Invalid image dimensions: %ld x %ld, using default dimensions: %ld x %ld",
                 img_width, img_height, max_width, max_height);
    }

    lv_coord_t zoom_w = (max_width * 256) / img_width;
    lv_coord_t zoom_h = (max_height * 256) / img_height;
    lv_coord_t zoom = (zoom_w < zoom_h) ? zoom_w : zoom_h;

    // Ensure zoom doesn't exceed 256 (100%)
    if (zoom > 256)
        zoom = 256;

    // Set image properties
    lv_image_set_src(preview_image, img_dsc);
    lv_image_set_scale(preview_image, zoom);

    // Add event handler to clean up LvglImage when image is deleted
    // We need to transfer ownership of the unique_ptr to the event callback
    LvglImage* raw_image = image.release();  // Release ownership of smart pointer
    lv_obj_add_event_cb(
        preview_image,
        [](lv_event_t* e) {
            LvglImage* img = (LvglImage*)lv_event_get_user_data(e);
            if (img != nullptr) {
                delete img;  // Properly release memory by deleting LvglImage object
            }
        },
        LV_EVENT_DELETE, (void*)raw_image);

    // Calculate actual scaled image dimensions
    lv_coord_t scaled_width = (img_width * zoom) / 256;
    lv_coord_t scaled_height = (img_height * zoom) / 256;

    // Set bubble size to be 16 pixels larger than the image (8 pixels on each side)
    lv_obj_set_width(img_bubble, scaled_width + 16);
    lv_obj_set_height(img_bubble, scaled_height + 16);

    // Don't grow in flex layout
    lv_obj_set_style_flex_grow(img_bubble, 0, 0);

    // Center the image within the bubble
    lv_obj_center(preview_image);

    // Left align the image bubble like assistant messages
    lv_obj_align(img_bubble, LV_ALIGN_LEFT_MID, 0, 0);

    // Auto-scroll to the image bubble
    lv_obj_scroll_to_view_recursive(img_bubble, LV_ANIM_ON);
}

void LcdDisplay::ClearChatMessages() {
    DisplayLockGuard lock(this);
    if (content_ == nullptr) {
        return;
    }

    // Use lv_obj_clean to delete all children of content_ (chat message bubbles)
    lv_obj_clean(content_);

    // Reset chat_message_label_ as it has been deleted
    chat_message_label_ = nullptr;

    // Show the centered AI logo (emoji_label_) again
    if (emoji_label_ != nullptr) {
        lv_obj_remove_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    }

    ESP_LOGI(TAG, "Chat messages cleared");
}
#else
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
    lv_obj_set_size(voice_root_, 360, 360);
    lv_obj_set_pos(voice_root_, 0, 0);
    lv_obj_set_style_pad_all(voice_root_, 0, 0);
    lv_obj_set_style_border_width(voice_root_, 0, 0);
    lv_obj_set_style_radius(voice_root_, 0, 0);
    lv_obj_remove_flag(voice_root_, LV_OBJ_FLAG_SCROLLABLE);
    auto screen = voice_root_;
    lv_obj_set_style_text_font(screen, text_font, 0);
    lv_obj_set_style_text_color(screen, lvgl_theme->text_color(), 0);
    lv_obj_set_style_bg_color(screen, lvgl_theme->background_color(), 0);

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

#ifdef CONFIG_APOLLO_CODEX_VOICE
    // Keep text inside the circle, away from the clipped top and bottom edges.
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x0C1220), 0);
    lv_obj_set_style_bg_color(container_, lv_color_hex(0x0C1220), 0);
    lv_obj_remove_flag(container_, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    lv_obj_set_style_text_color(status_label_, lv_color_white(), 0);
    lv_obj_set_style_text_color(notification_label_, lv_color_white(), 0);
    lv_obj_set_style_text_color(chat_message_label_, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(bottom_bar_, LV_OPA_TRANSP, 0);
    lv_obj_set_width(top_bar_, 160);
    lv_obj_align(top_bar_, LV_ALIGN_TOP_MID, 0, 24);
    lv_obj_add_flag(top_bar_, LV_OBJ_FLAG_HIDDEN);
    // Activity updates resize the pill to its text, capped at 260px.
    lv_obj_set_size(status_bar_, 220, 36);
    lv_obj_align(status_bar_, LV_ALIGN_CENTER, 0, 4);
    lv_obj_set_style_radius(status_bar_, 32, 0);
    lv_obj_set_style_bg_color(status_bar_, lv_color_hex(0x303346), 0);
    lv_obj_set_style_bg_opa(status_bar_, LV_OPA_80, 0);
    lv_obj_set_size(status_label_, 200, 24);
    lv_obj_set_size(notification_label_, 200, 24);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_DOT);
    lv_label_set_long_mode(notification_label_, LV_LABEL_LONG_DOT);
    // Leave space between the orb, captions, and call controls.
    lv_obj_set_size(bottom_bar_, 190, 26);
    lv_obj_align(bottom_bar_, LV_ALIGN_BOTTOM_MID, 0, -24);
    lv_obj_set_size(chat_message_label_, 184, 24);
    lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_SCROLL);

    // One small connector icon beside the activity text.
    voice_status_icon_ = lv_obj_create(status_bar_);
    lv_obj_set_size(voice_status_icon_, 24, 32);
    lv_obj_align(voice_status_icon_, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_set_style_bg_opa(voice_status_icon_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(voice_status_icon_, 0, 0);
    lv_obj_set_style_pad_all(voice_status_icon_, 0, 0);
    lv_obj_remove_flag(voice_status_icon_, LV_OBJ_FLAG_SCROLLABLE);
    voice_status_text_ = lv_label_create(status_bar_);
    lv_obj_set_size(voice_status_text_, 168, 48);
    lv_obj_align(voice_status_text_, LV_ALIGN_LEFT_MID, 36, 0);
    lv_obj_set_style_text_color(voice_status_text_, lv_color_white(), 0);
    lv_label_set_long_mode(voice_status_text_, LV_LABEL_LONG_DOT);
    lv_label_set_text(voice_status_text_, "");
    lv_obj_add_flag(voice_status_text_, LV_OBJ_FLAG_HIDDEN);
    voice_tool_active_ = false;

    lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(emoji_box_, voice_geometry::kOrbSize, voice_geometry::kOrbSize);
    lv_obj_align(emoji_box_, LV_ALIGN_CENTER, 0, 4);
    lv_obj_set_style_radius(emoji_box_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_clip_corner(emoji_box_, true, 0);
    lv_obj_set_style_bg_opa(emoji_box_, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(emoji_box_, lv_color_hex(0x7465EB), 0);
    lv_obj_set_style_bg_grad_color(emoji_box_, lv_color_hex(0xD9EFFF), 0);
    lv_obj_set_style_bg_grad_dir(emoji_box_, LV_GRAD_DIR_VER, 0);

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
            lv_obj_set_style_radius(voice_orb_canvas_, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_clip_corner(voice_orb_canvas_, true, 0);
            lv_obj_set_style_image_opa(voice_orb_canvas_, LV_OPA_50, 0);
            lv_obj_remove_flag(voice_orb_canvas_, LV_OBJ_FLAG_SCROLLABLE);
            RenderVoiceOrb(0.0f);
            voice_orb_timer_ = lv_timer_create(
                [](lv_timer_t* timer) {
                    auto display = static_cast<LcdDisplay*>(lv_timer_get_user_data(timer));
                    if (display->voice_orb_active_ && !lv_obj_has_flag(display->voice_root_, LV_OBJ_FLAG_HIDDEN)) {
                        display->RenderVoiceOrb(
                            static_cast<float>(lv_tick_elaps(display->voice_orb_started_at_)) /
                            1000.0f);
                    }
                },
                kFluidOrbFramePeriodMs, this);
            if (voice_orb_timer_ != nullptr) {
                lv_timer_pause(voice_orb_timer_);
            }
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
        lv_obj_set_style_bg_color(button, lv_color_hex(0x292929), 0);
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
#endif

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
        lv_obj_set_pos(b, i == 0 ? 62 : 246, 62);
        lv_obj_set_size(b, 52, 52);
        lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_pad_all(b, 0, 0);
        lv_obj_set_style_border_width(b, 0, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x181F2C), 0);
        lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        auto icon = lv_image_create(b);
        lv_image_set_src(icon, i == 0 ? &watch_icons::home : &watch_icons::more);
        lv_obj_set_style_image_recolor(icon, lv_color_white(), 0);
        lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
        lv_obj_center(icon);
        lv_obj_add_event_cb(b, [](lv_event_t* e) {
            auto self = static_cast<LcdDisplay*>(lv_event_get_user_data(e));
            const bool home = lv_obj_get_x(static_cast<lv_obj_t*>(lv_event_get_target(e))) == 62;
            if (home) Application::GetInstance().OnWatchAction(WatchUi::Action::EndCall, 0, "", "");
            self->watch_ui_->Show(home ? WatchUi::Page::Home : WatchUi::Page::CodexSettings);
            Application::GetInstance().OnWatchAction(WatchUi::Action::Refresh, 0, "", "");
        }, LV_EVENT_CLICKED, this);
    }
    voice_clock_ = lv_label_create(screen);
    lv_label_set_text(voice_clock_, "--:--");
    lv_obj_align(voice_clock_, LV_ALIGN_TOP_MID, 0, 26);
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
    // Show bottom_bar_ only when there is content (and subtitle is not globally hidden)
    if (bottom_bar_ != nullptr) {
        if (content == nullptr || content[0] == '\0') {
            lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        } else if (!hide_subtitle_) {
            lv_obj_remove_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        }
    }
#if CONFIG_USE_MULTILINE_CHAT_MESSAGE && !defined(CONFIG_APOLLO_CODEX_VOICE)
    // Re-align bottom_bar_ after text change so it stays anchored to the bottom
    // as its height adapts to the wrapped content.
    if (bottom_bar_ != nullptr) {
        lv_obj_align(bottom_bar_, LV_ALIGN_BOTTOM_MID, 0, 0);
    }
#endif
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
#endif

#ifdef CONFIG_APOLLO_CODEX_VOICE
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
        lv_obj_set_style_bg_color(button, lv_color_hex(0x292929), 0);
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
        lv_obj_set_style_bg_color(button, lv_color_hex(0x292929), 0);
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


static void SizeVoicePill(lv_obj_t* bar, lv_obj_t* label, const char* text, bool has_icon) {
    if (bar == nullptr || label == nullptr || text == nullptr) return;
    lv_point_t size{};
    lv_text_get_size(&size, text, lv_obj_get_style_text_font(label, LV_PART_MAIN), 0, 0,
                     LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    const int icon_space = has_icon ? 32 : 0;
    const int width = std::clamp(static_cast<int>(size.x) + 24 + icon_space, 100, 260);
    lv_obj_set_width(bar, width);
    lv_obj_set_size(label, width - 24 - icon_space, LV_SIZE_CONTENT);
    lv_obj_align(label, LV_ALIGN_CENTER, icon_space / 2, 0);
}

void LcdDisplay::SetVoiceActivity(const char* activity, const char* icon, const char* pixels) {
    DisplayLockGuard lock(this);
    if (voice_status_text_ == nullptr) return;
    lv_obj_clean(voice_status_icon_);
    if (voice_activity_image_) {
        lv_image_cache_drop(voice_activity_image_->image_dsc());
        voice_activity_image_.reset();
    }
    lv_obj_add_flag(voice_status_icon_, LV_OBJ_FLAG_HIDDEN);
    if (activity == nullptr || activity[0] == '\0' || strcmp(activity, "Listening") == 0) {
        voice_tool_active_ = false;
        SetStatus(Lang::Strings::LISTENING);
        return;
    }
    voice_tool_active_ = !VoiceIconIsThinking(activity) && strcmp(activity, "Answering…") != 0;
    lv_label_set_text(voice_status_text_, activity);
    lv_obj_remove_flag(voice_status_text_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
    bool has_icon = false;
    if (!VoiceIconIsThinking(activity) && pixels != nullptr && strlen(pixels) == 3072) {
        // One 24px BGRA icon; never decode a downloaded image on the device.
        auto data = static_cast<unsigned char*>(heap_caps_malloc(2304, MALLOC_CAP_8BIT));
        size_t size = 0;
        if (data != nullptr && mbedtls_base64_decode(data, 2304, &size,
                reinterpret_cast<const unsigned char*>(pixels), 3072) == 0 && size == 2304) {
            voice_activity_image_ = std::make_unique<LvglAllocatedImage>(
                data, size, 24, 24, 96, LV_COLOR_FORMAT_ARGB8888);
            auto image = lv_image_create(voice_status_icon_);
            lv_image_set_src(image, voice_activity_image_->image_dsc());
            lv_obj_center(image);
            has_icon = true;
        } else {
            heap_caps_free(data);
        }
    }
    if (!has_icon && icon != nullptr && strcmp(icon, "search") == 0 &&
        !VoiceIconIsThinking(activity)) {
        auto label = lv_label_create(voice_status_icon_);
        lv_obj_set_style_text_font(label, &BUILTIN_ICON_FONT, 0);
        lv_obj_set_style_text_color(label, lv_color_white(), 0);
        lv_label_set_text(label, MATERIAL_SYMBOLS_SEARCH);
        lv_obj_center(label);
        has_icon = true;
    }
    if (has_icon) lv_obj_remove_flag(voice_status_icon_, LV_OBJ_FLAG_HIDDEN);
    SizeVoicePill(status_bar_, voice_status_text_, activity, has_icon);
}

void LcdDisplay::SetVoiceMicrophoneMuted(bool muted) {
    DisplayLockGuard lock(this);
    if (voice_mute_icon_ == nullptr) {
        return;
    }
    lv_label_set_text(voice_mute_icon_, muted ? MATERIAL_SYMBOLS_MIC_OFF : MATERIAL_SYMBOLS_MIC);
    lv_obj_set_style_bg_color(voice_mute_button_, lv_color_hex(muted ? 0xA52C3D : 0x292929), 0);
}

void LcdDisplay::SetStatus(const char* status) {
    DisplayLockGuard lock(this);
    const bool mic_muted = Application::GetInstance().GetAudioService().IsMicrophoneMuted();
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
        voice_tool_active_ = false;
    }
    if (!voice_tool_active_) {
        LvglDisplay::SetStatus(pill_status);
        SizeVoicePill(status_bar_, status_label_, pill_status, false);
        if (voice_status_text_ != nullptr) {
            lv_obj_add_flag(voice_status_text_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
        }
        if (voice_status_icon_ != nullptr) {
            lv_obj_add_flag(voice_status_icon_, LV_OBJ_FLAG_HIDDEN);
        }
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
    const uint32_t orb_color = strcmp(status, Lang::Strings::ERROR) == 0 ? 0xCF4B59 : 0x7465EB;
    const bool was_orb_active = voice_orb_active_;
    voice_orb_color_ = orb_color;
    voice_orb_active_ = orb_active;
    lv_obj_set_style_bg_opa(emoji_box_, orb_active ? LV_OPA_COVER : LV_OPA_50, 0);
    lv_obj_set_style_bg_color(emoji_box_, lv_color_hex(orb_color), 0);
    if (orb_active) {
        if (!was_orb_active) voice_orb_started_at_ = lv_tick_get();
        if (voice_orb_timer_ != nullptr) lv_timer_resume(voice_orb_timer_);
        if (voice_orb_canvas_ != nullptr) {
            RenderVoiceOrb(static_cast<float>(lv_tick_elaps(voice_orb_started_at_)) / 1000.0f);
            lv_obj_set_style_image_opa(voice_orb_canvas_, LV_OPA_COVER, 0);
        }
    } else {
        if (voice_orb_timer_ != nullptr) lv_timer_pause(voice_orb_timer_);
        if (voice_orb_canvas_ != nullptr) {
            RenderVoiceOrb(0.0f);
            lv_obj_set_style_image_opa(voice_orb_canvas_, LV_OPA_50, 0);
        }
    }
}

void LcdDisplay::RenderVoiceOrb(float seconds) {
    if (voice_orb_canvas_ == nullptr || voice_orb_buffer_ == nullptr) return;

    const int size = voice_geometry::kOrbSize;
    const float color_r = static_cast<float>((voice_orb_color_ >> 16) & 0xFF) / 255.0f;
    const float color_g = static_cast<float>((voice_orb_color_ >> 8) & 0xFF) / 255.0f;
    const float color_b = static_cast<float>(voice_orb_color_ & 0xFF) / 255.0f;
    const float t = seconds * 0.22f;
    const float drift_x = std::sin(t) + 0.6f * std::sin(t * 1.7f + 1.3f);
    const float drift_y = std::cos(t * 0.8f) + 0.6f * std::cos(t * 1.3f + 2.1f);
    const float light_r = FluidOrbMix(1.0f, color_r, 0.5f);
    const float light_g = FluidOrbMix(1.0f, color_g, 0.5f);
    const float light_b = FluidOrbMix(1.0f, color_b, 0.5f);

    // ponytail: sample a 100x100 grid and expand it to 2x2 pixels; full-resolution noise is
    // needlessly expensive on the ESP32, and the display's 16-bit color already softens it.
    for (int y = 0; y < size; y += kFluidOrbSampleStep) {
        const float canvas_y = static_cast<float>(y) + 0.5f;
        const float uv_y = 1.0f - canvas_y / static_cast<float>(size);
        for (int x = 0; x < size; x += kFluidOrbSampleStep) {
            const float uv_x = (static_cast<float>(x) + 0.5f) / static_cast<float>(size);
            const float p_x = uv_x * 1.8f + drift_x * 0.7f;
            const float p_y = uv_y + drift_y * 0.7f;
            const float q_x = FluidOrbFbm(p_x + drift_x, p_y + drift_y);
            const float q_y = FluidOrbFbm(p_x + 3.2f - drift_x, p_y + 1.5f - drift_y);
            const float noise = FluidOrbFbm(p_x + 1.2f * q_x, p_y + 1.2f * q_y);
            const float base = std::clamp(1.0f - uv_y, 0.0f, 1.0f);
            const float anchor = FluidOrbSmoothStep(0.0f, 0.3f, uv_y);
            const float shade = std::clamp(base + (noise - 0.5f) * 0.8f * anchor, 0.0f, 1.0f);

            float red = FluidOrbMix(1.0f, light_r, FluidOrbSmoothStep(0.28f, 0.52f, shade));
            float green = FluidOrbMix(1.0f, light_g, FluidOrbSmoothStep(0.28f, 0.52f, shade));
            float blue = FluidOrbMix(1.0f, light_b, FluidOrbSmoothStep(0.28f, 0.52f, shade));
            const float dark_mix = FluidOrbSmoothStep(0.58f, 0.88f, shade);
            red = FluidOrbMix(red, color_r, dark_mix);
            green = FluidOrbMix(green, color_g, dark_mix);
            blue = FluidOrbMix(blue, color_b, dark_mix);

            const float dx = uv_x - 0.5f;
            const float dy = uv_y - 0.5f;
            const float edge = FluidOrbSmoothStep(0.5f, 0.49f, std::sqrt(dx * dx + dy * dy));
            lv_color16_t pixel{};
            pixel.blue = static_cast<uint16_t>(
                             std::clamp(blue * edge, 0.0f, 1.0f) * 255.0f) >> 3;
            pixel.green = static_cast<uint16_t>(
                              std::clamp(green * edge, 0.0f, 1.0f) * 255.0f) >> 2;
            pixel.red = static_cast<uint16_t>(
                            std::clamp(red * edge, 0.0f, 1.0f) * 255.0f) >> 3;

            for (int block_y = 0; block_y < kFluidOrbSampleStep && y + block_y < size; ++block_y) {
                for (int block_x = 0; block_x < kFluidOrbSampleStep && x + block_x < size;
                     ++block_x) {
                    voice_orb_buffer_[(y + block_y) * size + x + block_x] = pixel;
                }
            }
        }
    }
    lv_obj_invalidate(voice_orb_canvas_);
}
#endif

void LcdDisplay::SetEmotion(const char* emotion) {
#ifdef CONFIG_APOLLO_CODEX_VOICE
    return;
#endif
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

#if CONFIG_USE_WECHAT_MESSAGE_STYLE
    // In WeChat message style, if emotion is neutral, don't display it
    uint32_t child_count = lv_obj_get_child_cnt(content_);
    if (strcmp(emotion, "neutral") == 0 && child_count > 0) {
        // Stop GIF animation if running
        if (gif_controller_) {
            gif_controller_->Stop();
            gif_controller_.reset();
        }

        lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    }
#endif
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
#if CONFIG_USE_WECHAT_MESSAGE_STYLE
    // Set content background opacity
    lv_obj_set_style_bg_opa(content_, LV_OPA_TRANSP, 0);

    // Iterate through all children of content (message containers or bubbles)
    uint32_t child_count = lv_obj_get_child_cnt(content_);
    for (uint32_t i = 0; i < child_count; i++) {
        lv_obj_t* obj = lv_obj_get_child(content_, i);
        if (obj == nullptr)
            continue;

        lv_obj_t* bubble = nullptr;

        // Check if this object is a container or bubble
        // If it's a container (user or system message), get its child as bubble
        // If it's a bubble (assistant message), use it directly
        if (lv_obj_get_child_cnt(obj) > 0) {
            // Might be a container, check if it's a user or system message container
            // User and system message containers are transparent
            lv_opa_t bg_opa = lv_obj_get_style_bg_opa(obj, LV_PART_MAIN);
            if (bg_opa == LV_OPA_TRANSP) {
                // This is a user or system message container
                bubble = lv_obj_get_child(obj, 0);
            } else {
                // This might be an assistant message bubble itself
                bubble = obj;
            }
        } else {
            // No child elements, might be other UI elements, skip
            continue;
        }

        if (bubble == nullptr)
            continue;

        // Use saved user data to identify bubble type
        void* bubble_type_ptr = lv_obj_get_user_data(bubble);
        if (bubble_type_ptr != nullptr) {
            const char* bubble_type = static_cast<const char*>(bubble_type_ptr);

            // Apply correct color based on bubble type
            if (strcmp(bubble_type, "user") == 0) {
                lv_obj_set_style_bg_color(bubble, lvgl_theme->user_bubble_color(), 0);
            } else if (strcmp(bubble_type, "assistant") == 0) {
                lv_obj_set_style_bg_color(bubble, lvgl_theme->assistant_bubble_color(), 0);
            } else if (strcmp(bubble_type, "system") == 0) {
                lv_obj_set_style_bg_color(bubble, lvgl_theme->system_bubble_color(), 0);
            } else if (strcmp(bubble_type, "image") == 0) {
                lv_obj_set_style_bg_color(bubble, lvgl_theme->system_bubble_color(), 0);
            }

            // Update border color
            lv_obj_set_style_border_color(bubble, lvgl_theme->border_color(), 0);

            // Update text color for the message
            if (lv_obj_get_child_cnt(bubble) > 0) {
                lv_obj_t* text = lv_obj_get_child(bubble, 0);
                if (text != nullptr) {
                    // Set text color based on bubble type
                    if (strcmp(bubble_type, "system") == 0) {
                        lv_obj_set_style_text_color(text, lvgl_theme->system_text_color(), 0);
                    } else {
                        lv_obj_set_style_text_color(text, lvgl_theme->text_color(), 0);
                    }
                }
            }
        } else {
            ESP_LOGW(TAG, "child[%lu] Bubble type is not found", i);
        }
    }
#else
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
#endif

    // Update low battery popup
    lv_obj_set_style_bg_color(low_battery_popup_, lvgl_theme->low_battery_color(), 0);

    lv_obj_set_style_bg_color(container_, lv_color_hex(0x0C1220), 0);
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

#ifdef CONFIG_APOLLO_CODEX_VOICE
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
#endif
