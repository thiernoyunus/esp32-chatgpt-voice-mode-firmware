<a id="readme-top"></a>

[![C++][cpp-shield]][cpp-url]
[![ESP-IDF][espidf-shield]][espidf-url]
[![Espressif][espressif-shield]][espressif-url]
[![FreeRTOS][freertos-shield]][freertos-url]
[![Opus][opus-shield]][opus-url]

# esp32-chatgpt-voice-mode-firmware

Firmware for a small round-screen ESP32-S3 that sits on a desk and talks to
ChatGPT Voice. It connects to **one Mac on your own wifi**, and that Mac does
the thinking. No cloud service of the author's is involved, and nothing about
the conversation is stored anywhere but that Mac.

This is the device half. The Mac half is
[esp32-chatgpt-voice-mode](https://github.com/thiernoyunus/esp32-chatgpt-voice-mode).

```
  device  ──── your wifi ────►  a Mac running Codex  ────►  OpenAI realtime
     ▲                                                             │
     └──────────────── spoken audio, directly ─────────────────────┘
```

Only the **call setup** goes through the Mac. The spoken audio travels straight
between the device and the realtime service, which is worth knowing before
debugging a silent call: nothing on the Mac can drop or repair the voice.

## Hardware

One board, deliberately: the **Waveshare ESP32-S3-Touch-LCD-1.85C (V2)** —
round 360×360 touch display. Around a hundred other board definitions were
removed on purpose. If you want a general-purpose, many-board build, use
[78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) directly; it is
excellent at being that, and this is not trying to be.

## Setting it up from nothing

Start here: **[SETUP.md](https://github.com/thiernoyunus/esp32-chatgpt-voice-mode/blob/main/SETUP.md)**
in the Mac repository walks through both halves end to end, with a check after
each step. The rest of this page assumes you have already done that.

## Build and flash

See [documentation/operations/build.md](documentation/operations/build.md).
Briefly, with ESP-IDF v6 installed:

```bash
idf.py build
idf.py -p /dev/cu.usbmodemXXXX flash
```

The address, secret and device id live in the gitignored
`sdkconfig.defaults.local` — see
[documentation/operations/provisioning.md](documentation/operations/provisioning.md)
for what goes in it and how a per-device override works. That file holds a
secret, which is why it is gitignored: never commit it.

## Watching what it does

With the device connected over USB, in two terminals:

```bash
python3 scripts/serial_log.py
python3 scripts/monitor.py
```

Then open <http://localhost:8787/> for the conversation, the audio frames that
actually reached playback, microphone drops, and screen snapshots. Logs land in
`~/.voicemode/voicemode_live.log`. Three-byte WebRTC keepalives are excluded from the
audio count, so the number means what it says.

## Where this came from

A hard fork of [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32), which
wrote the hardware foundation this depends on: the audio service, the board
abstraction, the LVGL display path, the ESP-IDF scaffolding. It is MIT
licensed, and [LICENSE](LICENSE) keeps their copyright.

[documentation/reference/upstream.md](documentation/reference/upstream.md) sets
out exactly what was kept and what was removed.

## Licence

MIT. See [LICENSE](LICENSE).

<!-- MARKDOWN LINKS & IMAGES -->
[cpp-shield]: https://img.shields.io/badge/C++-00599C?style=for-the-badge&logo=cplusplus&logoColor=white
[cpp-url]: https://isocpp.org/
[espidf-shield]: https://img.shields.io/badge/ESP--IDF%20v6-E7352C?style=for-the-badge&logo=espressif&logoColor=white
[espidf-url]: https://docs.espressif.com/projects/esp-idf/en/latest/
[espressif-shield]: https://img.shields.io/badge/ESP32--S3-000000?style=for-the-badge&logo=espressif&logoColor=E7352C
[espressif-url]: https://www.espressif.com/en/products/socs/esp32-s3
[freertos-shield]: https://img.shields.io/badge/FreeRTOS-4CAE4F?style=for-the-badge&logo=freertos&logoColor=white
[freertos-url]: https://www.freertos.org/
[opus-shield]: https://img.shields.io/badge/Opus-8A2BE2?style=for-the-badge&logo=xiph.org&logoColor=white
[opus-url]: https://opus-codec.org/
