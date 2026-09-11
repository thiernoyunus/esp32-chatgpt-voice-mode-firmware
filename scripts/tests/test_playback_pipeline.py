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

    on_peer_audio = protocol.split(
        "int CodexVoiceProtocol::OnPeerAudio", 1)[1].split(
        "int CodexVoiceProtocol::OnDataChannelOpen", 1)[0]
    assert re.search(
        r"if \(is_real_audio\) \{\s*"
        r"\+\+real_audio_frames;\s*"
        r"protocol->reply_audio_received_\.store\(true\);\s*"
        r"protocol->speech_expected_since_ms_\.store\(0\);\s*"
        r"protocol->last_audio_frame_ms_\.store\(now\);",
        on_peer_audio,
        re.S,
    ), "real audio must invalidate the watchdog before publishing its timestamp"

    start_speaking = protocol.split(
        "void CodexVoiceProtocol::StartSpeaking()", 1)[1].split(
        "void CodexVoiceProtocol::StopSpeaking()", 1)[0]
    assert "if (!speaking_.exchange(true))" in start_speaking

    assert re.search(
        r"A new user turn starts a fresh assistant reply expectation\..*?"
        r"reply_audio_received_\.store\(false\);\s*"
        r"speech_expected_since_ms_\.store\(0\);",
        protocol,
        re.S,
    ), "a new user turn must reset the previous reply's audio state"

    assistant_delta = protocol.split(
        'else if (strcmp(type->valuestring, "realtime_transcript_delta") == 0)', 1
    )[1].split(
        'else if (strcmp(type->valuestring, "realtime_transcript_done") == 0)', 1
    )[0]
    assert re.search(
        r"if \(!reply_audio_received_\.load\(\)\) \{.*?"
        r"speech_expected_since_ms_\.compare_exchange_strong",
        assistant_delta,
        re.S,
    ), "the watchdog must arm only while the reply has no real audio"
    assert "reply_audio_received_.load()" in assistant_delta, (
        "later transcript deltas must not re-arm recovery after audio arrives"
    )

    assistant_done = re.search(
        r'if \(strcmp\(role->valuestring, "assistant"\) == 0\) \{'
        r"(?P<body>.*?)\n\s*StopSpeaking\(\);",
        protocol,
        re.S,
    )
    assert assistant_done, "assistant transcript completion path is missing"
    assert re.search(
        r"if \(reply_audio_received_\.load\(\)\) \{\s*"
        r"speech_expected_since_ms_\.store\(0\);",
        assistant_done.group("body"),
        re.S,
    ), "transcript completion must clear only after real audio was received"
    assert "Do not disarm" not in assistant_done.group("body"), (
        "completion comments must match the conditional watchdog behavior"
    )

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
