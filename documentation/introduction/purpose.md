# Purpose

This repository is the device half: a desk assistant with an animated face, living on a round-display ESP32-S3. The other half is a listener on one Mac on the same wifi ([esp32-chatgpt-voice-mode](https://github.com/thiernoyunus/esp32-chatgpt-voice-mode)), which opens the ChatGPT Voice call through Codex running there. There is no server on the internet.

## What it is

- A fork of [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) that keeps its hardware abstraction (boards, audio engines, displays) and replaces the brain: the only protocol that matters here is the Codex Voice one.
- Built and tested for exactly one board: the Waveshare ESP32-S3-Touch-LCD-1.85C **V2** (round 360×360 touch LCD, dual mic, speaker).
- A thin client on purpose. The device captures audio, renders a face, plays what the server sends, and reports gestures. Intelligence stays server-side.

## What it is not

- Not a general xiaozhi distribution: other boards, transports (MQTT/UDP), and cloud integrations are inherited but unmaintained here.
- Not self-contained: without the Mac listener to connect to, the device only wakes, listens, and times out.

## Design rule

The firmware and the Mac listener are two halves of one wire contract, and they change together.

## Navigation

Next: [Architecture](architecture.md)
