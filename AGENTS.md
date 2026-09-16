# AGENTS.md

## Project

Apollo's firmware — a hard fork of XiaoZhi (78/xiaozhi-esp32), reduced to exactly one purpose: the Waveshare ESP32-S3-Touch-LCD-1.85C V2 desk device talking to the Apollo server. All other boards, chips, protocols, and languages were removed on purpose; do not add generality back.

Use ESP-IDF v6.0.2.

## Architecture

- `main/application.*`: main event loop, protocol lifecycle, and high-level behavior.
- `main/device_state_machine.*`: legal runtime state transitions.
- `main/boards/common/`: board interfaces and the hardware helpers the 1.85C uses.
- `main/boards/waveshare/esp32-s3-touch-lcd-1.85c/`: pins, panel, touch task, board assets.
- `main/audio/`: audio service, codecs, wake word (runs on the raw mic here), queues.
- `main/protocols/apollo_protocol.*`: the only protocol. The server contract lives in the main repo.
- `main/display/emote_display.*`: emote-engine face and accent ring.
- `main/mcp_server.*`: device-side MCP tools and dispatch (not yet wired to Apollo).
- `main/Kconfig.projbuild` / `main/CMakeLists.txt`: trimmed to the single board and es-ES/en-US.
- `scripts/build.py`: canonical build entry point.

Read the closest existing implementation before adding a new one. Prefer the narrowest owning layer; do not put board-specific behavior into core modules.

## Required Rules

- The firmware adapts to Apollo's protocol, never the reverse.
- Preserve unrelated worktree changes and keep patches focused.
- Core code depends on `Board` interfaces, never the concrete board class or its `config.h`.
- Change runtime state through `Application::SetDeviceState()` and the state machine.
- Callbacks may run outside the main task. Schedule application mutations with `Application::Schedule()` or event bits.
- Do not block the main event loop or audio tasks. Avoid unbounded queues and repeated large allocations in audio paths.
- Validate network input and preserve `cJSON` ownership. NVS keys are persistent API and require migration when changed.
- Do not manually edit generated/vendor output: `build/`, `managed_components/`, `sdkconfig*`, `main/assets/lang_config.h`, or generated mmap headers. (`lang_config.h` is regenerated via `scripts/gen_lang.py`; see the handbook.)
- Format only touched C/C++ files with the repository `.clang-format`; avoid unrelated mass formatting.

## Commands

```sh
source /path/to/esp-idf/export.sh

# Build (the script prints [ERROR] but exits 0 on failure — read the output)
python3 scripts/build.py waveshare/esp32-s3-touch-lcd-1.85c

# Flash, from build/
python -m esptool --chip esp32s3 -b 460800 --before default-reset \
  --after hard-reset write-flash "@flash_args"
```

Never toggle DTR/RTS manually on the serial port (ROM download-mode trap); opening the port resets the chip.

## Validation

- Audio changes: verify capture, playback, wake word, interruption, and reconnect on the device.
- UI/asset changes: verify on the device; the emote engine only re-flushes dirty areas, so overlay bugs hide until a full redraw.
- Always report what was tested and what still needs physical hardware. A successful build is not hardware validation.

## Authoritative Documentation

- Handbook: `documentation/index.md` (mirrors the server repo's structure)
- Audio design: `main/audio/README.md`
- Server contract and roadmap: the main repo (`galfrevn/apollo`), `documentation/` and `docs/roadmap.md`

Keep detailed or fast-changing information in those files, not here.
