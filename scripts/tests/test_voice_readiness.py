#!/usr/bin/env python3
"""Check that a silent call can say which link never arrived.

"The call is open" and "the reply was heard" are different facts, and treating
them as one is what made silent calls hard to explain: the device reported
itself ready while the audio path was still missing a link. This compiles the
real readiness header, and the real stall wording out of the protocol source,
then drives them through the sequences that matter.
"""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "main/protocols/codex_voice_protocol.cc").read_text()

match = re.search(
    r"std::string StallMessage\(uint32_t missing_stage, bool will_retry\) \{.*?\n\}",
    SOURCE,
    re.S,
)
if match is None:
    sys.exit("StallMessage is gone from codex_voice_protocol.cc; this test tracks it")

PROGRAM = r"""
#include "voice_readiness.h"

#include <cassert>
#include <cstdio>
#include <string>

STALL_MESSAGE

int main() {
    /* A call reaches the links in order, and each one is reported once. */
    {
        VoiceReadiness readiness;
        assert(readiness.Mask() == 0);
        assert(readiness.Mark(kVoiceStagePeerConnected));
        assert(!readiness.Mark(kVoiceStagePeerConnected));
        assert(readiness.Mark(kVoiceStageAudioTrack));
        assert(readiness.Describe() == "peer,audio-track");
        assert(readiness.FirstMissing() == kVoiceStageEventChannel);
    }
    /* Nothing has to be reached in the listed order: the peer can report an
     * audio track before it reports itself connected. */
    {
        VoiceReadiness readiness;
        readiness.Mark(kVoiceStageAudioTrack);
        assert(readiness.Describe() == "audio-track");
        assert(readiness.FirstMissing() == kVoiceStagePeerConnected);
    }
    /* Every link, which is what a call in working order looks like, and then
     * there is nothing left to report. */
    {
        VoiceReadiness readiness;
        for (int index = 0; index < kVoiceStageCount; ++index) {
            readiness.Mark(1u << index);
        }
        assert(readiness.Mask() == kVoiceStageAll);
        assert(readiness.FirstMissing() == 0);
        assert(readiness.Describe() == "peer,audio-track,event-channel,session,playback");
    }
    /* A new call starts from nothing, so a fault cannot be blamed on the last
     * call's success. */
    {
        VoiceReadiness readiness;
        readiness.Mark(kVoiceStageAll);
        readiness.Reset();
        assert(readiness.Mask() == 0);
        assert(readiness.Describe() == "none");
    }

    /* The part a person reads. Every missing link has to name itself, because
     * "nothing came through" is true of each of them. */
    assert(StallMessage(kVoiceStagePeerConnected, true).find("audio never arrived") != std::string::npos);
    assert(StallMessage(kVoiceStageAudioTrack, true).find("audio never arrived") != std::string::npos);
    assert(StallMessage(kVoiceStageEventChannel, true).find("channel never opened") != std::string::npos);
    assert(StallMessage(kVoiceStageSessionStarted, true).find("session never started") != std::string::npos);
    assert(StallMessage(kVoiceStagePlaybackAdmitted, true).find("speaker") != std::string::npos);

    /* Retrying is promised only while there is a retry left. */
    assert(StallMessage(kVoiceStageAudioTrack, true).find("Reconnecting") != std::string::npos);
    assert(StallMessage(kVoiceStageAudioTrack, false).find("Reconnecting") == std::string::npos);
    assert(StallMessage(kVoiceStageAudioTrack, false).find("Tap to try again") != std::string::npos);

    /* Every message still has to be a message. */
    const uint32_t every_stage[] = {kVoiceStagePeerConnected, kVoiceStageAudioTrack,
                                    kVoiceStageEventChannel, kVoiceStageSessionStarted,
                                    kVoiceStagePlaybackAdmitted, 0u};
    for (uint32_t missing : every_stage) {
        const std::string message = StallMessage(missing, true);
        assert(!message.empty());
        assert(message.back() == '.');
    }

    printf("readiness stages and stall wording verified\n");
    return 0;
}
"""

program = PROGRAM.replace("STALL_MESSAGE", match.group(0))

with tempfile.TemporaryDirectory() as directory:
    source = Path(directory) / "readiness_check.cc"
    source.write_text(program)
    binary = Path(directory) / "readiness_check"
    compile_result = subprocess.run(
        ["c++", "-std=c++17", "-I", str(ROOT / "main/protocols"),
         str(source), "-o", str(binary)],
        capture_output=True, text=True,
    )
    if compile_result.returncode != 0:
        sys.exit("could not compile the readiness check:\n" + compile_result.stderr)
    run_result = subprocess.run([str(binary)], capture_output=True, text=True)
    if run_result.returncode != 0:
        sys.exit("readiness check failed:\n" + run_result.stdout + run_result.stderr)
    print(run_result.stdout.strip())
