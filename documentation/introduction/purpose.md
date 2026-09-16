# Purpose

This repository is the device half of Apollo: a personal desk agent with an animated face, living on a round-display ESP32-S3 on a desk. The server half — voice turns, tools, memory, background work — is [galfrevn/apollo](https://github.com/galfrevn/apollo) on Cloudflare Workers.

## What it is

- A fork of [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) that keeps its hardware abstraction (boards, audio engines, displays) and replaces the brain: the only protocol that matters here is Apollo's.
- Built and tested for exactly one board: the Waveshare ESP32-S3-Touch-LCD-1.85C **V2** (round 360×360 touch LCD, dual mic, speaker).
- A thin client on purpose. The device captures audio, renders a face, plays what the server sends, and reports gestures. Intelligence stays server-side.

## What it is not

- Not a general xiaozhi distribution: other boards, transports (MQTT/UDP), and cloud integrations are inherited but unmaintained here.
- Not self-contained: without an Apollo server to connect to, the device only wakes, listens, and times out.

## Design rule

The firmware adapts to Apollo, not the reverse. When the wire contract and the firmware disagree, the firmware changes.

## Navigation

Next: [Architecture](architecture.md)
