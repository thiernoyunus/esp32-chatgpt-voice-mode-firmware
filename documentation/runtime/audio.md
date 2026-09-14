# Audio

Everything audio goes through `main/audio/audio_service.*`: capture, wake word, encode/decode queues, and playback. On the live Codex Voice path, call audio is **Opus over WebRTC**; local UI sounds still use the Ogg/Opus effect path.

## Uplink (mic → server)

- Capture at 16 kHz mono. While a Codex Voice call is open, frames go out as Opus on the WebRTC peer, not as raw PCM websocket frames.
- **Wake word runs on the AFE output.** With `CONFIG_USE_MICRO_WAKE_WORD` (the shipping config) a microWakeWord streaming TFLite model ("Hey, Apólo", embedded in the app image from `main/assets/wakewords/`) consumes post-AEC mono frames in `AfeAudioEngine::HandleWakeWordResult()` via `MicroWakeWord`, exactly like the MultiNet path. Because the detector sees echo-cancelled audio, wake-word detection remains available during playback. That does not prove ordinary speech barge-in: talking over a continuous reply is still unresolved; see [the barge-in record](../reference/barge-in.md). An earlier note here claimed the AFE-integrated WakeNet never detects on this board; that was a misdiagnosis — the flashed image carried a different model than sdkconfig selected.
- Tuning lives in menuconfig: `MICRO_WAKE_WORD_PROBABILITY_CUTOFF` (percent, higher = stricter) and `MICRO_WAKE_WORD_SLIDING_WINDOW_SIZE`. Swap the model file with `idf.py -DMICRO_WAKE_WORD_MODEL=<name>.tflite reconfigure`.

## Downlink (server → speaker)

Two payload kinds share the decode queue, distinguished by `AudioStreamPacket.pcm`:

| Kind | Source | Path |
|------|--------|------|
| `pcm = true` | Decoded call audio handed to the service as PCM | Copied straight to the playback queue |
| `pcm = false` | Local sound effects | Ogg demuxer → Opus decoder |

Both resample to the codec's output rate (24 kHz on this board) when needed.

## Design notes

- The `pcm` flag exists because a build flag can't make this call: the queue carries both kinds at once. Before it existed, Opus effect packets were played as raw PCM — loud radio static at a volume unrelated to the file.
- If captions arrive but speaker frames do not, `CodexVoiceProtocol` may tear the call down and retry (see [Protocol](protocol.md)).

## Navigation

Prev: [Protocol](protocol.md) · Next: [Face](face.md)
