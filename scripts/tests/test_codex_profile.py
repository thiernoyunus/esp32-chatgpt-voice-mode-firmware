#!/usr/bin/env python3
"""Run with ESP-IDF's Python: reject Classic and old display presets."""
from pathlib import Path
import tempfile
import kconfiglib

root = Path(__file__).resolve().parents[2]
source = (root / "main/Kconfig.projbuild").read_text()
voice = source[source.index("config VOICEMODE_CODEX_VOICE"):source.index("config VOICEMODE_NTP_SERVER")]
display = source[source.index("choice DISPLAY_STYLE"):source.index("config USE_MULTILINE_CHAT_MESSAGE")]
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory) / "Kconfig"
    path.write_text("\n".join(
        f"config {name}\n    bool\n    default y\n"
        for name in ("VOICEMODE_PROTOCOL", "IDF_TARGET_ESP32S3", "BOARD_TYPE_WAVESHARE_ESP32_S3_TOUCH_LCD_1_85C")
    ) + voice + display)
    config = kconfiglib.Kconfig(str(path), warn=False)
    assert config.syms["VOICEMODE_CODEX_VOICE"].str_value == "y"
    for style in ("USE_EMOTE_MESSAGE_STYLE", "USE_WECHAT_MESSAGE_STYLE"):
        config.syms[style].set_value("y")
        assert config.syms[style].str_value == "n"
        assert config.syms["USE_DEFAULT_MESSAGE_STYLE"].str_value == "y"
    config.syms["VOICEMODE_CODEX_VOICE"].set_value("n")
    config.syms["USE_EMOTE_MESSAGE_STYLE"].set_value("y")
    assert config.syms["VOICEMODE_CODEX_VOICE"].str_value == "y"
    assert config.syms["USE_EMOTE_MESSAGE_STYLE"].str_value == "n"
print("PASS: only Codex orb builds, even with stale Classic settings")
