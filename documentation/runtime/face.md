# Face

The round panel is an **LVGL** display driven by `main/display/lcd_display.*`. Idle chrome is `WatchUi` (home plus settings). During a call, `LcdDisplay::RenderVoiceOrb` draws the **bloub** character from `main/display/bloub/` into a canvas — shape and colour come from watch settings, with connecting orbit rings and a working-state cycle while the agent is busy.

There is no emote engine and no `emote_display` in this tree. The earlier emotion mapping went away with the dialect that used it.

## Call chrome

Around the orb the UI shows mute / end controls, the current model label, streaming captions, tool captions from `realtime_status`, and the confirm overlay when one is active.

## Navigation

Prev: [Audio](audio.md) · Next: [Touch](touch.md)
