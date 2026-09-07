#!/usr/bin/env python3
"""Guard the realtime playback budget.

Silent replies came from the speaker draining faster than the codec task
refilled it: the decode queue backed up to its cap and live speech was
discarded while stale frames played out. These checks fail if the buffer
depth or the codec task priority regress to a starving configuration.
"""
from pathlib import Path
import re
import sys


def read(path):
    return (Path(__file__).resolve().parents[2] / path).read_text()


def main():
    header = read("main/audio/audio_service.h")
    service = read("main/audio/audio_service.cc")

    playback = int(re.search(r"#define MAX_PLAYBACK_TASKS_IN_QUEUE (\d+)", header).group(1))
    encode = int(re.search(r"#define MAX_ENCODE_TASKS_IN_QUEUE (\d+)", header).group(1))
    frame_ms = int(re.search(
        r"#ifdef CONFIG_APOLLO_CODEX_VOICE\s*\n#define OPUS_FRAME_DURATION_MS (\d+)",
        header).group(1))

    # The output task must not be able to empty the buffer inside one scheduler
    # tick (10 ms at the configured 100 Hz), or playback stalls mid-reply.
    assert playback * frame_ms >= 100, (
        f"playback buffer is {playback * frame_ms} ms; too small to absorb jitter")

    # The capture side must outlast one blocking speaker write (~34 ms observed)
    # plus a decode, or microphone frames are dropped and the user's words are
    # cut mid-sentence.
    assert encode * frame_ms >= 100, (
        f"microphone buffer is {encode * frame_ms} ms; a slow codec write will drop speech")

    def priority(task):
        match = re.search(r'"%s",[^;]*?,\s*this,\s*(\d+),' % task, service, re.S)
        assert match, f"could not find priority for {task}"
        return int(match.group(1))

    codec = priority("opus_codec")
    output = priority("audio_output")

    # Decode feeds playback. If the codec task cannot preempt the output task,
    # the buffer drains to empty and the speaker goes silent mid-reply.
    assert codec > output, f"opus_codec priority {codec} must exceed audio_output {output}"

    print(f"playback {playback * frame_ms} ms, microphone {encode * frame_ms} ms, "
          f"opus_codec={codec} > audio_output={output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
