# Upstream

This repository started as a fork of [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) and owes it the entire hardware foundation: the audio service, the board abstraction, the LVGL display path, and the ESP-IDF project scaffolding.

It is no longer a tracking fork. The divergence is deliberate and one-way:

## Removed from upstream

- **All boards** except `boards/common/` and the Waveshare ESP32-S3-Touch-LCD-1.85C (~100 board implementations).
- **Upstream protocols** (`mqtt_protocol`, `websocket_protocol`) and this fork's later JSON-over-websocket dialect. The live path is Codex Voice (`codex_voice_protocol.*`).
- **All languages** except es-ES (device language) and en-US (fallback base).
- Non-S3 chip configs, cellular modem support (ML307/NT26, dual-network, ethernet), upstream docs, docker packaging, and the zh/ja READMEs.
- Camera/video, USB-network, BluFi, unused LED drivers, unused audio codecs, and the emote/OLED display stacks.

## Kept from upstream

- The board/audio/display architecture and the `boards/common/` helpers the 1.85C actually uses.
- The build tooling (`scripts/build.py`, asset generation) and managed components still required here (esp-sr, codecs, LVGL stack).
- `LICENSE` (MIT) and attribution: if you need a general-purpose multi-board xiaozhi, use upstream — it is excellent. This fork trades all of that generality for one desk, one board, one Mac.

## Navigation

Prev: [Provisioning](../operations/provisioning.md) · Back to [Index](../index.md)
