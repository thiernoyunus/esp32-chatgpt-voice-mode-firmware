# Face

The face is rendered by the emote engine (`espressif2022/esp_emote_*` components) through `main/display/emote_display.*`. There is no LVGL display on this board.

## Emotion mapping

Apollo's six emotions map onto the emote asset vocabulary in `MapApolloEmotion` (`apollo_protocol.cc`):

| Apollo | Emote asset | Why |
|--------|-------------|-----|
| `neutral` | `neutral` | Resting face; a single blink the application replays on a cadence |
| `curious` | `winking` | Looping neutral blink — attentive. Not `shocked`: the panic eyes read as an error |
| `focused` | `thinking` | |
| `questioning` | `confused` | |
| `talking` | `happy` | |
| `calm` | `sleepy` | |

Assets live in the board's `assets/360_360/emote.json`; non-looping assets freeze on their last frame, which reads as a crash, so the mapping avoids them for active states.

## Accent ring

An 8 px ring hugs the round display's edge, colored by the active speech mode (`ui_state.accentColor`, `#RRGGBB`). It is painted in the flush callback (`OverlayAccentRing`), directly onto each stripe the engine sends to the panel — the engine owns every pixel, so an overlay survives the animations only there.

Two non-obvious mechanics:

- The engine re-flushes **dirty areas only**. The screen border belongs to none of them, so a color change calls `RefreshAll()` to invalidate everything — otherwise only the ring segments that happen to overlap animated rows would repaint, and the rest would keep the old color.
- The engine renders byte-swapped RGB565 (`swap = true`), so the stored ring color is pre-swapped to match the buffer.

## Navigation

Prev: [Audio](audio.md) · Next: [Touch](touch.md)
