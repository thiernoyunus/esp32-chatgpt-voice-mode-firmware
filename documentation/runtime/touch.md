# Touch

The round screen is the only input surface. The board's touch task (`main/boards/waveshare/esp32-s3-touch-lcd-1.85c/`) feeds raw samples to LVGL, which hit-tests the controls on the current page. There is no board-level hold, double-tap, or swipe recognizer on the Codex Voice path.

## Touch behavior

| Surface | Effect |
|---------|--------|
| Home | Tap a tile to open Voice, Settings, or Clock |
| Settings | Tap a button or row to navigate or change the selected value |
| Keyboard | Tap keys to edit; Cancel returns without saving; Next/Join submits the field |
| Voice | Tap the orb to open a call, or tap the on-screen mute/end controls during a call |
| Confirmation | Tap Sí or No; taps outside the two buttons do nothing |

## Design notes

- Any sign of the user — touch, wake word, a turn starting — calls `Application::NoteUserActivity()`, which wakes the screen and restarts the inactivity countdown. After 60 s idle the backlight goes dark and the display enters power-save mode (that is the part that costs CPU).
- A `confirm_request` replaces the face with a full-screen prompt: the summary plus Sí/No touch buttons (`main/display/confirm_geometry.h` holds the layout, shared with the hit-test so they cannot drift). While it is up, the touch task routes releases only to those two zones; taps outside them do nothing. Voice stays available through the boot button and the wake word.
- If the touch controller remains unresponsive after the retry, the task asks the main application to close any active voice call before it stops. This keeps a failed input device from leaving a call running without its screen controls.
- The screen dismisses on: a button press, local expiry (from `expiresAt`, clamped; falls back to 30 s if the clock is unsynced), a server `confirm_close`, any `ui_state` whose state is not `confirm`, or the channel closing.

## Navigation

Prev: [Face](face.md) · Next: [Sounds](sounds.md)
