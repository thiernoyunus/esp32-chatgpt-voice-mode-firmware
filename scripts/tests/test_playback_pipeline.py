#!/usr/bin/env python3
"""Guard the realtime playback budget and inbound-audio recovery.

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
    protocol = read("main/protocols/codex_voice_protocol.cc")

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

    assert re.search(
        r"constexpr size_t kMinimumVoiceAudioBytes = 3;", protocol
    ), "three-byte Opus packets must satisfy the inbound-audio threshold"

    # What the watchdog does is now driven through the real transitions in
    # scripts/tests/test_reply_audio_watchdog.py. The assertions that used to
    # sit here pinned the SHAPE of that code instead - including, as it turned
    # out, the line that caused the bug - so a source match was green while a
    # barge-in's trailing packet answered for the next reply.
    on_peer_audio = protocol.split(
        "int CodexVoiceProtocol::OnPeerAudio", 1)[1].split(
        "int CodexVoiceProtocol::OnDataChannelOpen", 1)[0]
    assert "speech_expected_since_ms_" not in on_peer_audio, (
        "the audio callback must not disarm the watchdog: it cannot tell which "
        "reply a frame belongs to"
    )
    assert "last_audio_frame_ms_.store(now)" in on_peer_audio, (
        "the audio callback still has to publish when the frame arrived"
    )

    start_speaking = protocol.split(
        "void CodexVoiceProtocol::StartSpeaking()", 1)[1].split(
        "void CodexVoiceProtocol::StopSpeaking()", 1)[0]
    assert "if (!speaking_.exchange(true))" in start_speaking

    watchdog = protocol.split(
        "void CodexVoiceProtocol::CheckInboundAudioStall()", 1)[1].split(
        "int CodexVoiceProtocol::OnPeerState", 1)[0]
    assert "compare_exchange_strong(expecting, 0)" in watchdog, (
        "audio recovery must ignore a timeout raced by a real frame"
    )

    print(f"playback {playback * frame_ms} ms, microphone {encode * frame_ms} ms, "
          f"opus_codec={codec} > audio_output={output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
