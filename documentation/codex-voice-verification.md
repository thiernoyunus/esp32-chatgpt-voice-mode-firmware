# Codex Voice verification — 2026-09-05

Work remains on `experiment/codex-voice`; the Classic release is unchanged.

## Reproduced and corrected

- The captured microphone question was quiet (loudest one-second RMS approximately -33 dBFS). In a silent Chromium WebRTC comparison, the original recording produced no answer; the same recording amplified by 16 produced “That's four.” No extra voice-start message or separate API key was required.
- On the ESP32, 60 ms Opus packets still produced no answer with that amplification. Switching only the Codex build to 20 ms packets produced a reply. Classic retains 60 ms packets.
- Live audio was discarded unless the caption-driven state was Speaking. Codex now accepts audio throughout an open Listening/Speaking call, does not reset playback when captions start, and clears playback on close. Classic retains its original playback policy.
- Tap/double-tap now toggles a voice call. Closed/failed sessions cannot mark a replacement call ready or erase its state.
- The Codex screen uses an original gradient orb and text positioned inside the round screen. It is not claimed to be the Watch demo's artwork.

## Evidence and limits

Two automated device calls in the same boot, separated by close/reopen, both recognized the saved question and returned “That's four.” The user confirmed audible playback. A temporary local capture was taken after the speaker write. The diagnostic recording, automatic call logic, and capture code were then removed from production source.

The clean application image was flashed and its written hash verified. The linked image contains no embedded test recording; the device returned to idle wake-word monitoring. Local checks passed after cleanup. No new server deployment was needed.

Run `python3 scripts/tests/test_voice_close.py` for the host checks. These compile the current callbacks and check caption-independent playback, closed-call silence, replacement-call safety, failed readiness, gain saturation, and 20 ms Codex versus 60 ms Classic framing.

The input multiplier is a bounded, fixed calibration, not automatic gain control. Closer/louder speakers, acoustic interruption, long calls, and final screen fit still need ordinary device use. A passed recorded-input test does not establish those behaviors.

## Annotated call controls — installed September 5

**Current correction:** the user subsequently removed the model picker from scope. The latest installed diagnostic image removes its visible button and touch route, shows mute/end only during a connected call, and moves the orb upward. The historical first-image description below is not the current UI specification. Further activity-icon and layout refinements are in progress, not yet installed.

The September 5 recording requests a larger orb, mute without hanging up, compact tool activity over the orb, a model picker, and optional future chat history. The installed patch includes a 200-pixel drawn gradient orb, sound-level-driven brightness, separate mic and end-call controls, actual tool-activity captions, and a three-choices-per-page model picker. The model choice applies to the next call's backing Codex task, not the separate speech model. The catalog comes from the installed Codex app-server and includes a local Default recovery choice. A brief greeting is requested through v3 initialItems, but spontaneous greeting has not been verified. Plugin browsing and chat history remain deferred.

The 360 × 360 layout uses a 200-pixel orb and two 52-pixel circular controls. Shared drawing/touch geometry checks keep both controls within the circular screen. Tapping the orb during a call no longer hangs up; use the close button. A first touch on a sleeping screen still only wakes it.

Microphone mute clears pending send audio and rejects audio that was being encoded when mute changed. It leaves the microphone processor and speaker playback running, preserving echo cancellation and the open call. Mute remains selected across calls until explicitly toggled back; it is not persisted across a device reboot. The mute control remains visible and usable while idle or after an error. The optional server-AEC timestamp queue is bounded even during mute; this device uses device AEC.

Run `python3 scripts/tests/test_microphone_mute.py`, `test_voice_close.py`, and `test_voice_messages.py` from the same directory. Host checks pass for mute/unmute, stale encoded audio, retained playback, button hit testing, sleep wake-up, confirmation-screen ownership, catalog/activity validation, and stale-call messages. Firmware builds with the locally installed ESP-IDF 6.0 (the repository requests 6.0.2).

Opus 5 reviewed the original mute/queue/control changes; its two actionable issues were corrected. Sonnet 5 reviewed the voice path, and MiniMax M3 implemented the model catalog. Parent review corrected catalog discovery on default calls, real reasoning-effort field shape, setup cleanup, and timeout handling. Server checks passed: 673 main tests plus 18 explicit bridge tests. Worker version 6667aafc-d8b5-4408-8e80-6342b77906a3 was deployed; health and device hello-to-ui_state verification passed; the local bridge was restarted and reconnected.

The first UI/model/mute image (SHA-256 ec6f9e3f4710e36a20c77e6f4312d9413affdc239ccda8331e9b671305574c0e) was flashed to the existing app partition and its written hash verified. It leaves 16% of the app slot free. A live user question and assistant answer appeared in serial logs after reboot, with about 71 KB free internal SRAM. This is not proof that the user heard the answer. Physical screen fit, mute privacy/playback/unmute, model switching, repeated calls, and acoustic interruption still need device confirmation.

## Interruption evidence and remaining boundary

A silent browser-side v3 probe observed live data-channel events including turn.created, turn.delta, input_transcript.added, and output_transcript.added. Injecting a second recorded instruction while the assistant was responding changed the answer from counting to the new sky-color question, without ending the call. The probe measured zero output audio energy, so it proves conversation switching only, not audible interruption; the cause of that measurement remains unresolved. Source-only claims that v3 cannot interrupt or that these events never reach the data channel were disproved by the live observations.

The latest installed diagnostic image handles a validated turn.created with role user by clearing queued playback on the main task, while leaving the microphone and call open. Closed/replaced-call events are ignored. The actual-parser host test exercises those guards. Physical interruption remains unverified; transcript completion alone still does not reset playback.

## Current silent-answer investigation

September 6 follow-up: the user confirmed a silent call whose live captions included “Hi, I am here.” and “Hey! What can I do for you?”. The initial short burst did not reach the previous every-250-frame diagnostic threshold. Later playback logged nonzero peak 2983, followed by peak 1, with DAC mute register 0, volume a3, UI volume 65; incoming audio outpaced playback and decode backlog reached 120 while microphone frames dropped. This does not yet identify the root cause of the silent reply. Installed diagnostic-only build SHA256 `e92b81163e2824fbbdd4781deced96a3d0ac6839adb46dac1e34d3f8d3323f05` changes receive/playback logs to once per second and queries native peer transport statistics every five seconds. Build, voice message/close host tests, flash hash verification passed; app partition remains 16% free. No UI or audio behavior fix was included. A separately authorized MacBook microphone recording contained all-zero samples and cannot be used as speaker evidence; no Shure recording or laptop playback was started.

The user again confirmed new answer captions without audible playback. Device diagnostics saw incoming sound packets and a decoded PCM peak of 24063 with output enabled, volume 65, and microphone unmuted. Subsequent windows contained near-silence (peak 1). Receiving/decoding audio does not prove sound at the physical speaker or that the nonzero window belongs to the user's latest answer.

During that call, roughly 250 output chunks took 7.5 seconds while incoming chunks arrived faster, and the microphone encode queue repeatedly dropped frames. Internal SRAM remained around 71 KB. The bottleneck and silent answer are not yet proven to share a cause. Additional source-only diagnostics measure decoded chunk duration/size/backlog, speaker-write duration, and the ES8311 DAC mute/volume registers; these measurements must be installed before drawing conclusions. No sound is routed through the laptop.

## September 6 installed update

Application SHA-256 `2debda137e1aabfd2375a8b1093ab0436363a8e674df39272545235d4697f269` was flashed to the app partition and verified. It leaves 16% of that partition free. The additional audio diagnostics above are now installed. The user reported sound returning before this flash; intermittent playback is still unresolved, not a verified repair.

The installed layout has no Default button, connected-only mute/end controls, a content-sized activity pill capped at 260px, and captions below the orb. The device accepts exactly one 24x24 BGRA icon (2304 decoded bytes) and replaces/releases it on the next activity. There are no baked-in app logos. The only built-in fallback symbol is generic web search. Geometry and parser host checks pass; a new physical photo is still needed for final visual confirmation.

The bridge now uses Codex `appContext.connectorId` and `actionName`, resolves `app/read` icon metadata, and asynchronously sends bounded icon pixels. Late results are discarded when the activity or session changes. Initial image support is PNG from OpenAI CDN domains only, resized using the already-installed `/opt/homebrew/bin/magick`; unsupported formats/hosts and MCP servers without connector metadata fall back to text. This is not universal MCP icon support. Tests exercise real PNG conversion, download limits, cache eviction, and stale-result handling. Checks: 673 server tests, 41 explicit script tests, type checking. Worker version `895396d7-b5a1-4f25-93ef-5cfa06fbe3c2` deployed; bridge restarted. Live connector icon retrieval/rendering remains unverified.

The next physical test exposed a separate startup error: line 162 of local Codex config joined `[mcp_servers.annotate-session]` and `command` without a newline. The file was backed up to `config.toml.apollo-20260906-line162.bak`; only that newline was corrected. TOML parsing and a fresh ephemeral Codex thread startup passed afterward. This startup failure does not explain every earlier silent response.

Serial diagnostics are saved locally in ignored `log.xiaozhi.20260906003014.txt` so subsequent measurements survive output truncation. These are text logs, not an audio recording.

## September 6 Codex-only profile repair

The board preset selected Emote independently of Codex Voice, allowing real WebRTC
with the old eyes display. The active workspace now fixes Codex Voice on, excludes
Emote/WeChat choices, selects default orb assets in the board preset, and rejects
incompatible profiles at CMake configuration. Captions and call controls remain.
The stale `build/` directory was moved to `../firmware-backups/eyes-build-20260906`.

`test_codex_profile.py` verifies stale Classic/eyes settings cannot override the
profile. Voice messages, close, mute, and layout checks pass. The message parser
now forwards valid device MCP envelopes independently of voice requestId;
parser checks cover missing callbacks and invalid payloads.

Final application SHA-256:
`2c10c8ac80877578b6074f5ccec33c9ed6edde367969d4bc162b36b0f4deb66d`.
Generated config and linked ELF were checked before flashing: CodexVoiceProtocol
and RenderVoiceOrb present, EmoteDisplay and ApolloProtocol absent. Flash hashes
verified on /dev/cu.usbmodem1101. Final serial boot initialized LcdDisplay/LVGL,
loaded default assets, and reached Codex Voice WebRTC listening. ESP-IDF 6.0.2.
Logs: `/tmp/apollo-orb-final-flash.log`, `/tmp/apollo-orb-final-boot.log`.
Live MCP screen/status calls still time out even during the connected call;
no device screenshot was obtained, so final visual appearance remains unverified.
No server changes, commit, or push were made during this repair.
