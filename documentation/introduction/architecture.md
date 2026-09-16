# Architecture

A build selects exactly one board implementation; everything else is shared infrastructure inherited from upstream.

## Layout

| Path | Role |
|------|------|
| `main/application.*` | Main event loop, protocol lifecycle, high-level behavior |
| `main/device_state_machine.*` | Legal runtime state transitions |
| `main/protocols/apollo_protocol.*` | Apollo's websocket dialect (the one that matters) |
| `main/boards/waveshare/esp32-s3-touch-lcd-1.85c/` | Pins, panel, touch task, board assets |
| `main/audio/` | Codecs, audio service, engines, wake word, queues |
| `main/display/emote_display.*` | Emote-engine display: face, accent ring |
| `main/assets/` | Sounds, language strings, `sound_variants.h` |
| `scripts/` | Build, asset generation, sound conversion |

## Threads that matter

- The **application loop** owns state transitions and display scheduling (`Schedule`).
- The **audio service** runs capture, wake word, encode/decode, and playback queues.
- The **emote engine** renders the face at ~30 fps on its own task and flushes dirty regions to the panel.
- The **touch task** (board-specific) turns raw touch into taps, swipes, and hold-to-talk.

## Design notes

- Cross-thread display work goes through `Application::Schedule`, not direct calls.
- The emote engine owns every pixel: overlays (like the accent ring) happen in the flush callback, because nothing else survives the animations.

## Navigation

Prev: [Purpose](purpose.md) · Next: [Protocol](../runtime/protocol.md)
