#!/usr/bin/env python3
"""Build and run the real LVGL WatchUi host harness, then check fresh PNGs."""

import os
import sys
import subprocess
from pathlib import Path

from PIL import Image


ROOT = Path(__file__).resolve().parents[2]
HOST = ROOT / "scripts/tests/watch_ui_host"
BUILD = Path(
    os.environ.get("VOICEMODE_WATCH_UI_BUILD", "/tmp/voicemode-watch-ui-host-owned/build")
)
OUTPUT = Path("/tmp")
# The LVGL simulator lives outside this repo, so both it and its round-boundary
# checker have to be pointed at. Skip rather than fail: this harness is a local
# rendering aid, not part of the build.
SIM_DIR = Path(os.environ.get("VOICEMODE_LVGL_SIM_DIR", ""))
CHECK_ROUND = SIM_DIR.parent / "check_round.py" if SIM_DIR.name else Path()
if not SIM_DIR.is_dir() or not CHECK_ROUND.is_file():
    print("SKIP: set VOICEMODE_LVGL_SIM_DIR to an LVGL simulator checkout "
          "(expects ../check_round.py beside it)")
    sys.exit(0)
EXPECTED_TAGS = {
    "home",
    "settings",
    "brightness",
    "volume",
    "clock",
    "wifi",
    "models",
    "about",
    "chatgpt",
    "shapes",
    "colours",
    "chatgpt_voices",
    "keyboard_open",
    "keyboard_ab",
    "keyboard_upper",
    "keyboard_sym",
    "keyboard_typed",
    "keyboard_cancel",
    "keyboard_pass",
    "slider_50",
    "nav_stable",
    # The pages the harness gained alongside these: every one it renders has to
    # be listed, or the run fails on the set rather than on any one screenshot.
    "sleep",
    "reasoning",
    "chats",
    "approvals",
    "wifisetup",
    "notice",
}


subprocess.run(["cmake", "-S", str(HOST), "-B", str(BUILD)], cwd=ROOT, check=True)
subprocess.run(["cmake", "--build", str(BUILD), "-j2"], cwd=ROOT, check=True)

for path in OUTPUT.glob("watch-*.png"):
    path.unlink()

subprocess.run([str(BUILD / "watch_ui_test")], cwd=ROOT, check=True)
shots = sorted(OUTPUT.glob("watch-*.png"))
tags = {path.stem.split("-", 3)[3] for path in shots}
assert tags == EXPECTED_TAGS, f"unexpected screenshot set: {sorted(tags)}"

for path in shots:
    with Image.open(path) as image:
        assert image.size == (360, 360), f"{path} is {image.size}, expected 360x360"
    subprocess.run(["python3", str(CHECK_ROUND), str(path)], cwd=ROOT, check=True)

print(f"PASS: {len(shots)} fresh 360x360 WatchUi screenshots passed round-boundary checks")
