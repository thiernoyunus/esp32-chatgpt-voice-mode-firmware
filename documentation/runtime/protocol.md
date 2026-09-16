# Protocol

The device speaks Codex Voice through `main/protocols/codex_voice_protocol.*`: a long-lived **control WebSocket** plus a **per-call WebRTC** session. The earlier JSON-over-websocket dialect is gone from this tree.

## Connection

`Start()` opens the control socket to `<base>/agents/voicemode/<device_id>?token=<token>`, sends `hello`, and leaves MCP available while idle. A voice call starts later: `OpenAudioChannel()` creates an `esp_peer` WebRTC session (Opus 16 kHz mono, send and receive), waits for the reliable data channel `oai-events`, then fires the opened callbacks.

Signaling stays on the control WebSocket. The local SDP goes out as `realtime_offer` (optional `model` / `threadId` / `temporary` / `voice`); the bridge answers with `realtime_answer` (SDP plus the model and chat lists the watch UI can show).

## Device → server

| Path | What moves |
|------|------------|
| Control WS `realtime_offer` | SDP to start a call, with optional model/chat/voice choices |
| Control WS `realtime_stop` | Hang up |
| Control WS `mcp` | Device-side MCP tool traffic |
| WebRTC Opus uplink | Microphone audio for the open call |

Listening / wake helpers on the base `Protocol` class are no-ops here: opening the audio channel *is* the call. Cancel and abort tear the WebRTC session down.

## Server → device

| Path | Handled as |
|------|------------|
| Control WS `realtime_answer` | Remote SDP plus model/chat inventory |
| Control WS `realtime_status` | Tool caption / icon while the agent works |
| Control WS `realtime_error` | Call failure |
| Control WS transcript deltas / done | Captions on the orb |
| WebRTC Opus downlink | Speaker audio |
| Peer data-channel events | Realtime turn / error traffic (`turn.created`, `error`, …) |

If captions arrive but no real inbound audio shows up for about four seconds, a stall detector tears the call down and may retry (up to three times).

## Design notes

- Mic and speaker audio ride Opus RTP on WebRTC, not binary websocket frames.
- Base-class Classic helpers (`SendTelemetry`, `SendConfirm`, `SendPlaybackAck`) stay as empty defaults; Codex Voice does not implement them.
- Turn the call off with `CloseAudioChannel` / `realtime_stop`; there is no separate `hold_end` / `audio_end` dialect on this path.

## Navigation

Prev: [Architecture](../introduction/architecture.md) · Next: [Audio](audio.md)
