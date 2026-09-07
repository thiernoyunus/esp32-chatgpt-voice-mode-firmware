#!/usr/bin/env python3
"""Build and run the real LVGL WatchUi host harness, then check fresh PNGs."""

import os
import subprocess
from pathlib import Path

from PIL import Image


ROOT = Path(__file__).resolve().parents[2]
HOST = ROOT / "scripts/tests/watch_ui_host"
BUILD = Path(
    os.environ.get("APOLLO_WATCH_UI_BUILD", "/tmp/apollo-watch-ui-host-owned/build")
)
OUTPUT = Path("/tmp")
CHECK_ROUND = Path("/Users/thiernodiallo/Coding/tools/lvgl-mcp/check_round.py")
EXPECTED_TAGS = {
    "home",
    "settings",
    "brightness",
    "volume",
    "clock",
    "wifi",
    "models",
    "about",
    "keyboard_open",
    "keyboard_ab",
    "keyboard_upper",
    "keyboard_sym",
    "keyboard_typed",
    "keyboard_cancel",
    "keyboard_pass",
    "slider_50",
    "nav_stable",
}


subprocess.run(["cmake", "-S", str(HOST), "-B", str(BUILD)], cwd=ROOT, check=True)
subprocess.run(["cmake", "--build", str(BUILD), "-j2"], cwd=ROOT, check=True)

for path in OUTPUT.glob("apollo-watch-*.png"):
    path.unlink()

subprocess.run([str(BUILD / "watch_ui_test")], cwd=ROOT, check=True)
shots = sorted(OUTPUT.glob("apollo-watch-*.png"))
tags = {path.stem.split("-", 3)[3] for path in shots}
assert tags == EXPECTED_TAGS, f"unexpected screenshot set: {sorted(tags)}"

for path in shots:
    with Image.open(path) as image:
        assert image.size == (360, 360), f"{path} is {image.size}, expected 360x360"
    subprocess.run(["python3", str(CHECK_ROUND), str(path)], cwd=ROOT, check=True)

print(f"PASS: {len(shots)} fresh 360x360 WatchUi screenshots passed round-boundary checks")
