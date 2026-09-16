#!/usr/bin/env python3
"""Check that the opening of a reply survives the start of a call.

The greeting arrives between the voice channel opening and the microphone
starting, and starting to listen clears the speaker queues. Frames held outside
those queues are the difference between a reply that begins normally and one
that begins in the middle. This compiles the real pre-roll buffer.
"""
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]

PROGRAM = r"""
#include "voice_preroll.h"

#include <cassert>
#include <cstdio>
#include <memory>
#include <vector>

int main() {
    VoicePreroll<std::unique_ptr<int>> preroll(3);

    /* Frames come back in the order they arrived: the start of the sentence is
     * what is being rescued. */
    {
        assert(preroll.Push(std::make_unique<int>(1)));
        assert(preroll.Push(std::make_unique<int>(2)));
        assert(preroll.Size() == 2);
        std::vector<int> played;
        const size_t delivered = preroll.Flush([&](std::unique_ptr<int> frame) {
            played.push_back(*frame);
            return true;
        });
        assert(delivered == 2);
        assert(played.size() == 2 && played[0] == 1 && played[1] == 2);
        assert(preroll.Empty());
    }
    /* Handing frames over empties the buffer, so a second call cannot replay
     * the first call's greeting. */
    {
        std::vector<int> played;
        preroll.Flush([&](std::unique_ptr<int> frame) {
            played.push_back(*frame);
            return true;
        });
        assert(played.empty());
    }
    /* The buffer is bounded, and overflow is reported rather than hidden: a
     * call slower to start listening than a greeting is long is a fault worth
     * seeing, not a reason to hold more audio. */
    {
        for (int value = 0; value < 3; ++value) {
            assert(preroll.Push(std::make_unique<int>(value)));
        }
        assert(!preroll.Push(std::make_unique<int>(99)));
        assert(preroll.Size() == 3);
        assert(preroll.Dropped() == 1);
        std::vector<int> played;
        preroll.Flush([&](std::unique_ptr<int> frame) {
            played.push_back(*frame);
            return true;
        });
        /* The earliest frames are kept, which is the beginning of the reply. */
        assert(played.size() == 3 && played[0] == 0 && played[2] == 2);
    }
    /* A sink that refuses frames - a full speaker queue - must not stop the
     * rest from being offered. */
    {
        preroll.Push(std::make_unique<int>(1));
        preroll.Push(std::make_unique<int>(2));
        int offered = 0;
        const size_t delivered = preroll.Flush([&](std::unique_ptr<int>) {
            ++offered;
            return false;
        });
        assert(offered == 2);
        assert(delivered == 0);
        assert(preroll.Empty());
    }
    /* A call that ends before it starts leaves nothing behind for the next. */
    {
        preroll.Push(std::make_unique<int>(1));
        preroll.Push(std::make_unique<int>(2));
        preroll.Push(std::make_unique<int>(3));
        preroll.Push(std::make_unique<int>(4));
        preroll.Clear();
        assert(preroll.Empty());
        assert(preroll.Dropped() == 0);
    }

    printf("pre-roll buffer verified\n");
    return 0;
}
"""

with tempfile.TemporaryDirectory() as directory:
    source = Path(directory) / "preroll_check.cc"
    source.write_text(PROGRAM)
    binary = Path(directory) / "preroll_check"
    compile_result = subprocess.run(
        ["c++", "-std=c++17", "-I", str(ROOT / "main/audio"),
         str(source), "-o", str(binary)],
        capture_output=True, text=True,
    )
    if compile_result.returncode != 0:
        sys.exit("could not compile the pre-roll check:\n" + compile_result.stderr)
    run_result = subprocess.run([str(binary)], capture_output=True, text=True)
    if run_result.returncode != 0:
        sys.exit("pre-roll check failed:\n" + run_result.stdout + run_result.stderr)
    print(run_result.stdout.strip())
