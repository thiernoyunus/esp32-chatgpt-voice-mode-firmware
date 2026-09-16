# Barge-in — what has been tried

Talking over the device while it is answering does not work. This is the record of
what has been attempted, what each attempt measured, and why each one failed,
so the same ground is not covered twice.

**Status: unsolved.** Nothing described here is on `main`. Everything below was
built, flashed to hardware, and measured.

## The shape of the problem

The device's microphone stays open while it speaks, and the far end (Codex) decides
when a reply has been interrupted, based on the audio we upload. The device
already handles being told "the user started talking" correctly. So the
plumbing is not the issue — what we upload is.

Two things spoil it, and they are independent:

1. **The echo canceller has nothing to work with.** It removes the device's voice
   from the microphone by subtracting a copy of what the speaker is playing.
   The board supplies that copy on a wired channel. It is correctly wired and
   correctly timed, and about a thousand times too quiet to subtract anything.
2. **The microphone is overloaded.** the device's own speaker drives it past the
   converter's ceiling, and a signal that has hit the ceiling cannot be
   cancelled out of — the shape of what was said over it is already gone.

### The measurement that frames everything

At the canceller's input, peak per second, with the speaker at volume 33:

| condition | microphone | reference copy |
|---|---|---|
| user talking, speaker off | 5923 | **2** |
| Device speaking | 13177 (peaks 32768) | **15** |

The second row is the deficit: roughly 60 dB. The first row proves the wired
channel is genuine and not crosstalk — it ignores the user completely and
responds only to the speaker.

Volume matters more than it looks. From the ES8311 driver's own range
(`0x00` = -95.5 dB, `0xFF` = +32 dB), speaker level in dB is roughly
`volume - 89`. So **volume 82 is 282x louder than volume 33**.

## Attempts

### 1. Lower the echo suppression — no effect

`AEC_NLP_LEVEL_VERYAGGR` to `AGGR`. VERYAGGR was not the cause; it was masking
the fact that the canceller does nothing, by gating the microphone whenever the
speaker was active. Changing it changed nothing useful.

### 2. Fix the microphone gain — a real bug, unrelated

Post-canceller software gain was 16x. Speech peaks at about 6000, so 16x drove
it to ~96000 against a 32767 ceiling: every loud syllable sheared flat. Now 4x.
This improved speech recognition. It did nothing for barge-in. **On `main`.**

### 3. Multiply the wired reference — actively harmful, do not repeat

Scaled the wired copy by 256, then 512, before the canceller.

| | user's voice out of the canceller |
|---|---|
| x256 | median 1164 |
| x512 | median 1021 — worse, while mic gain went *up* |

The copy's own noise floor (~2) scales with it. At x512 that is ~1024 of
permanent "the speaker is playing", so the canceller gated the microphone in a
silent room. Speaking had to get louder and louder. **A constant multiplier can
never work here: it raises hiss and signal together.** Reverted.

### 4. Raise the codec's reference gain — not viable

The ES7210 driver already sets all four channels to 30 dB at init, and the
ceiling is 37.5 dB. That is +7.5 dB against a 60 dB deficit.

### 5. Software reference + full-duplex mode — correct, unconfirmed (PR #14, closed)

Took the reference from the audio itself, immediately before it goes to the
speaker: clean, full-scale, and exactly zero when nothing is playing, which the
wired one can never be. Also switched `AEC_MODE_VOIP_HIGH_PERF` (one side at a
time) to `AEC_MODE_FD_HIGH_PERF` (full duplex, which is what interrupting is).

Measured on hardware:

| | reference | in silence |
|---|---|---|
| wired | 15–32 vs mic 13177 | ~2 hiss |
| scaled x512 | loud, hiss scaled too | ~1024 phantom |
| **software** | **16237** vs mic 32768 | **0** |

Two traps, both hit, both of which still *look* correct in the logs:

- The copy must be substituted **before** the capture buffer is resampled
  (24 kHz to 16 kHz). After it, the reference plays back 1.5x too fast.
- Anything left in the queue becomes a permanent timing offset. First build
  measured 110 ms out of step because the boot chime queued before capture
  started consuming. The canceller needs the reference with the echo or
  slightly ahead, never behind.

**Result: still could not interrupt.** Because of the second problem —

### 6. Cut the microphone amplifier — most promising, untested

With the software reference in place, at volume 82 the microphone read 32768
every second: pinned, and so unusable regardless of how good the reference is.

Dropped the ES7210's input gain from 30 dB to 6 dB and made the loudness up
after cancellation instead (`kCodexVoiceInputGain` 4 to 32). Volume stays where
the user wants it; the converter gets headroom.

| at volume 82 | before | after |
|---|---|---|
| mic into the canceller | 32768 (pinned) | **23773** (headroom) |
| reference | 15 | **16439** |

**First time both halves were in range at once — and it was never tested with a
real call.** This is where to resume.

## If picking this up again

1. Redo attempt 5 + 6 together and test with a real call at the volume actually
   used. That combination has never been tried.
2. Watch for the device interrupting *itself*: that means suppression is too low,
   not that the approach is wrong.
3. Check speech still transcribes. Attempt 6 moves 24 dB from before the
   converter to after it, and that was never verified.
4. There may be a volume ceiling. 30 dB of microphone headroom is all the chip
   has; if the speaker still overwhelms it at maximum volume, no setting fixes
   that and the remaining levers are physical.

Do not revisit attempt 3. That family is falsified.

## Navigation

Prev: [Upstream](upstream.md)
