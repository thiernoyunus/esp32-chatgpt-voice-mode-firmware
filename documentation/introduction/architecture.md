# Architecture

A build selects exactly one board implementation; everything else is shared infrastructure inherited from upstream.

## Layout

| Path | Role |
|------|------|
| `main/application.*` | Main event loop, protocol lifecycle, high-level behavior |
| `main/device_state_machine.*` | Legal runtime state transitions |
| `main/protocols/codex_voice_protocol.*` | Codex Voice: control WebSocket + per-call WebRTC |
| `main/boards/waveshare/esp32-s3-touch-lcd-1.85c/` | Pins, panel, touch task, board assets |
| `main/audio/` | Codecs, audio service, engines, wake word, queues |
| `main/display/lcd_display.*` / `watch_ui.*` / `bloub/` | Round LVGL watch UI and call-face orb |
| `main/assets/` | Sounds, language strings, `sound_variants.h` |
| `scripts/` | Build, asset generation, sound conversion |

## Threads that matter

- The **application loop** owns state transitions and display scheduling (`Schedule`).
- The **audio service** runs capture, wake word, encode/decode, and playback queues.
- **LVGL** renders the watch chrome and the call-face orb on the display task.
- The **touch task** (board-specific) turns raw touch into taps, swipes, and hold-to-talk.

## Design notes

- Cross-thread display work goes through `Application::Schedule`, not direct calls.
- The call face is drawn by `LcdDisplay::RenderVoiceOrb` into an LVGL canvas; watch pages live in `WatchUi`.

## Navigation

Prev: [Purpose](purpose.md) · Next: [Protocol](../runtime/protocol.md)
