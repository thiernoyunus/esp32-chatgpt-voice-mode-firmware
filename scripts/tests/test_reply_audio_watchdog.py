#!/usr/bin/env python3
"""Drive the silent-reply watchdog through the sequences that broke it.

The old version asked a bool "has reply audio arrived?", which cannot tell this
reply's audio from the last one's. Barge-in makes that difference real: the
interrupted reply's packets are still draining when the next one starts, and one
of them would answer for a reply nobody had heard.

This compiles the real AtOrAfter out of the protocol source and re-states the
four transitions around it, so it fails if the comparison is weakened back to a
plain >, or if the stall check stops preferring the arming moment.
"""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "main/protocols/codex_voice_protocol.cc").read_text()

match = re.search(r"bool AtOrAfter\(uint32_t sample, uint32_t since\) \{.*?\n\}", SOURCE, re.S)
if match is None:
    sys.exit("AtOrAfter is gone from codex_voice_protocol.cc; this test tracks it")

PROGRAM = r"""
#include <cassert>
#include <cstdint>
#include <cstdio>

constexpr uint32_t kInboundAudioStallMs = 4000;
ATORAFTER

/* The four places the protocol touches this state, and nothing else. */
struct Watchdog {
    uint32_t expecting = 0;      // speech_expected_since_ms_
    uint32_t last_frame = 0;     // last_audio_frame_ms_

    void UserTurn()              { expecting = 0; }
    void TranscriptDelta(uint32_t now) { if (expecting == 0) expecting = now; }
    void Audio(uint32_t now)     { last_frame = now; }
    void TranscriptDone() {
        if (expecting != 0 && AtOrAfter(last_frame, expecting)) expecting = 0;
    }
    bool Fires(uint32_t now) const {
        if (expecting == 0) return false;
        const uint32_t quiet_since = AtOrAfter(last_frame, expecting) ? last_frame : expecting;
        return now - quiet_since >= kInboundAudioStallMs;
    }
};

int main() {
    /* The bug. A reply is interrupted, its packets are still draining, and the
     * next reply is silent. The trailing frame must not answer for it. */
    {
        Watchdog w;
        w.TranscriptDelta(1000); w.Audio(1100);   // a reply, heard
        w.UserTurn(2000);                          // the user cuts in
        w.Audio(2050);                             // a straggler from that reply
        w.TranscriptDelta(3000);                   // the next reply begins
        assert(!w.Fires(4000));                    // too early to judge
        assert(w.Fires(7000));                     // silent, and caught
    }
    /* A straggler must not disarm a check that is already running either. */
    {
        Watchdog w;
        w.TranscriptDelta(1000);
        w.Audio(900);                              // older than the arming
        assert(w.Fires(5000));
    }
    /* An ordinary audible reply is left alone, during and after. */
    {
        Watchdog w;
        w.TranscriptDelta(1000);
        w.Audio(1200); w.Audio(2400); w.Audio(3600);
        assert(!w.Fires(3700));
        w.TranscriptDone();
        assert(!w.Fires(60000));                   // silence after it ends is fine
    }
    /* Words finished, nothing ever heard: stays armed and fires. */
    {
        Watchdog w;
        w.TranscriptDelta(1000);
        w.TranscriptDone();
        assert(w.Fires(5000));
    }
    /* Audio racing ahead of the first delta still counts, as long as it lands
     * after the arming moment - the common case where both stream together. */
    {
        Watchdog w;
        w.TranscriptDelta(1000);
        w.Audio(1000);                             // same millisecond
        assert(!w.Fires(4500));
    }
    /* A new user turn cancels whatever was pending. */
    {
        Watchdog w;
        w.TranscriptDelta(1000);
        w.UserTurn();
        assert(!w.Fires(90000));
    }
    /* The millisecond clock wraps every 49 days. Armed just before the wrap,
     * with audio flowing normally just after it, a plain >= reads the fresh
     * frame as older than the arming and kills a call that is working. */
    {
        Watchdog w;
        w.TranscriptDelta(0xFFFFFF00u);            // moments before the wrap
        w.Audio(4000);                             // past it, and healthy
        assert(!w.Fires(4100));                    // heard 100ms ago: leave it be
    }
    printf("test_reply_audio_watchdog: ok\n");
    return 0;
}
"""


def main():
    program = PROGRAM.replace("ATORAFTER", match.group(0))
    # UserTurn takes no argument in the model; the call above passes one for
    # readability, so give it a default here rather than in the struct.
    program = program.replace("void UserTurn()              { expecting = 0; }",
                              "void UserTurn(uint32_t = 0)  { expecting = 0; }")
    with tempfile.TemporaryDirectory() as tmp:
        source = Path(tmp) / "watchdog.cc"
        binary = Path(tmp) / "watchdog"
        source.write_text(program)
        subprocess.run(["c++", "-std=c++17", "-O1", str(source), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
