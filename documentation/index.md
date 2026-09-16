# Firmware handbook

This firmware runs on a Waveshare ESP32-S3-Touch-LCD-1.85C (V2, round 360×360 touch display), forked from [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32). It talks to a listener on one Mac on the same wifi over a control WebSocket plus per-call WebRTC (Codex Voice). That listener is [esp32-chatgpt-voice-mode](https://github.com/thiernoyunus/esp32-chatgpt-voice-mode); the two are halves of one wire contract and change together.

This handbook is meant to be read in order. Later chapters assume the concepts introduced earlier. The server-side handbook lives in the main repo and covers everything above the wire.

## Contents

### Part I — Introduction

1. [Purpose](introduction/purpose.md) — What this fork is and is not
2. [Architecture](introduction/architecture.md) — How the codebase is laid out

### Part II — Runtime

3. [Protocol](runtime/protocol.md) — Codex Voice (control WebSocket + WebRTC)
4. [Audio](runtime/audio.md) — Mic to server, server to speaker
5. [Face](runtime/face.md) — Watch UI and call-face orb
6. [Touch](runtime/touch.md) — Hold-to-talk, gestures, screen sleep
7. [Sounds](runtime/sounds.md) — UI effects and their pitch variants

### Part III — Operations

8. [Toolchain](operations/toolchain.md) — ESP-IDF environment
9. [Build](operations/build.md) — Building for the 1.85C
10. [Flash](operations/flash.md) — Flashing and serial, with the traps
11. [Provisioning](operations/provisioning.md) — Pointing a device at a server

### Part IV — Reference

12. [Upstream](reference/upstream.md) — Relationship with xiaozhi-esp32
13. [Barge-in](reference/barge-in.md) — Talking over the assistant: what has been tried, and why it failed
