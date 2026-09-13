#ifndef LCD_DISPLAY_H
#define LCD_DISPLAY_H

#include "gif/lvgl_gif.h"
#include "lvgl_display.h"

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <atomic>
#include <memory>
#include "watch_ui.h"

#define PREVIEW_IMAGE_DURATION_MS 5000

class LcdDisplay : public LvglDisplay {
protected:
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;

    lv_draw_buf_t draw_buf_;
    lv_obj_t* top_bar_ = nullptr;
    lv_obj_t* status_bar_ = nullptr;
    lv_obj_t* content_ = nullptr;
    lv_obj_t* container_ = nullptr;
    lv_obj_t* side_bar_ = nullptr;
    lv_obj_t* bottom_bar_ = nullptr;
    lv_obj_t* preview_image_ = nullptr;
    lv_obj_t* emoji_label_ = nullptr;
    lv_obj_t* emoji_image_ = nullptr;
    std::unique_ptr<LvglGif> gif_controller_ = nullptr;
    lv_obj_t* emoji_box_ = nullptr;
    lv_obj_t* chat_message_label_ = nullptr;
    esp_timer_handle_t preview_timer_ = nullptr;
    std::unique_ptr<LvglImage> preview_image_cached_ = nullptr;
    bool hide_subtitle_ = false;  // Control whether to hide chat messages/subtitles
    lv_obj_t* voice_root_ = nullptr;
    std::unique_ptr<WatchUi> watch_ui_;
    lv_indev_t* touch_input_ = nullptr;
    std::atomic<uint32_t> touch_sample_{0};
    lv_obj_t* voice_clock_ = nullptr;
    lv_obj_t* voice_mute_button_ = nullptr;
    lv_obj_t* voice_mute_icon_ = nullptr;
    lv_obj_t* voice_end_button_ = nullptr;
    lv_obj_t* voice_orb_canvas_ = nullptr;
    lv_obj_t* voice_state_caption_ = nullptr;
    lv_timer_t* voice_orb_timer_ = nullptr;
    lv_color16_t* voice_orb_buffer_ = nullptr;
    lv_obj_t* voice_model_label_ = nullptr;
    lv_obj_t* voice_model_panel_ = nullptr;
    // The tool caption that stands in for the state word, and its icon.
    lv_obj_t* voice_tool_text_ = nullptr;
    lv_obj_t* voice_tool_icon_ = nullptr;
    // When the tool caption went up, and the one-shot that ends its minimum
    // stay if a plain "Thinking" arrived while it was still too fresh.
    uint32_t voice_tool_shown_at_ = 0;
    lv_timer_t* voice_tool_hold_timer_ = nullptr;
    std::unique_ptr<LvglAllocatedImage> voice_activity_image_;
    bool voice_tool_active_ = false;
    bool voice_orb_active_ = false;
    bool voice_orb_connecting_ = false;
    // True from the agent's first status report until it answers.
    bool voice_working_ = false;
    // Where the working cycle is. Held as plain integers so this header does
    // not have to pull in bloub_states.h, whose profile tables are inline.
    // The values are bloub_state_id_t; the cycle order is in lcd_display.cc.
    uint8_t voice_cycle_state_ = 0;   // BLOUB_STATE_IDLE
    uint8_t voice_cycle_index_ = 0;
    uint32_t voice_cycle_started_at_ = 0;
    // Ticks the rings appeared at, and the handshake ended at - the second is
    // 0 once they have finished leaving.
    uint32_t voice_orbit_started_at_ = 0;
    uint32_t voice_orbit_exit_at_ = 0;
    uint32_t voice_orb_started_at_ = 0;
    uint32_t voice_orb_color_ = 0x7465EB;
    int voice_shape_ = 0;
    int voice_colour_ = 0;
    std::string voice_state_caption_text_;
    uint32_t voice_state_caption_color_ = 0;

    void RenderVoiceOrb(float seconds);
    bool AdvanceWorkingCycle();
    void UpdateVoiceStateCaption(const char* text, uint32_t color);
    void ShowVoiceToolCaption(bool tool);
    void UpdateVoiceToolCaption(const char* activity);
    void ClearVoiceToolCaption();
    void ReleaseVoiceToolHold();
    lv_obj_t* confirm_root_ = nullptr;
    lv_obj_t* confirm_summary_ = nullptr;
    lv_obj_t* confirm_approve_btn_ = nullptr;
    lv_obj_t* confirm_reject_btn_ = nullptr;

    void InitializeLcdThemes();
    virtual bool Lock(int timeout_ms = 0) override;
    virtual void Unlock() override;

protected:
    // Add protected constructor
    LcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
               int height);

public:
    ~LcdDisplay();
    virtual void SetEmotion(const char* emotion) override;
    void SetStatus(const char* status) override;
    void SetVoiceMicrophoneMuted(bool muted) override;
    void SetVoiceActivity(const char* activity, const char* icon = "none",
                          const char* pixels = nullptr) override;
    void SetVoiceModel(const char* name) override;
    void SetVoiceCharacter(int shape, int colour);
    void ShowVoiceModels(const std::vector<std::string>& names, size_t page) override;
    void HideVoiceModels() override;
    void FeedTouch(bool pressed, int x, int y) override;
    void ShowVoicePage() override;
    void UpdateWatchInfo(const WatchUi::Info& info);
    void UpdateStatusBar(bool update_all = false) override;
    void ShowConfirmScreen(const char* summary) override;
    void HideConfirmScreen() override;
    virtual void SetChatMessage(const char* role, const char* content) override;
    virtual void ClearChatMessages() override;
    virtual void SetPreviewImage(std::unique_ptr<LvglImage> image) override;
    virtual void SetupUI() override;
    // Add theme switching function
    virtual void SetTheme(Theme* theme) override;

    // Set whether to hide chat messages/subtitles
    void SetHideSubtitle(bool hide);
};

// SPI LCD display
class SpiLcdDisplay : public LcdDisplay {
public:
    SpiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                  int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                  bool swap_xy);
};

// RGB LCD display
class RgbLcdDisplay : public LcdDisplay {
public:
    RgbLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                  int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                  bool swap_xy);
};

// MIPI LCD display
class MipiLcdDisplay : public LcdDisplay {
public:
    MipiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                   int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                   bool swap_xy);
};

#endif  // LCD_DISPLAY_H
