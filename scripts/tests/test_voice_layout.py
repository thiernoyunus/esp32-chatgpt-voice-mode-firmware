#!/usr/bin/env python3
"""Compile the real pill sizing helper with a tiny stand-in for text measurement."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / "main/display/lcd_display.cc").read_text()
helper = source[source.index("static void SizeVoicePill("):source.index("void LcdDisplay::SetVoiceActivity(")]
program = r'''
#include <algorithm>
#include <cassert>
#include <cstring>
struct lv_obj_t { int width = 0, offset = 0; };
struct lv_point_t { int x = 0, y = 0; };
constexpr int LV_PART_MAIN=0, LV_COORD_MAX=99999, LV_TEXT_FLAG_NONE=0;
constexpr int LV_SIZE_CONTENT=-1, LV_ALIGN_CENTER=0;
void* lv_obj_get_style_text_font(lv_obj_t*, int) { return nullptr; }
void lv_text_get_size(lv_point_t* size, const char* text, void*, int, int, int, int) {
    size->x = std::strlen(text)*12;
}
void lv_obj_set_width(lv_obj_t* object, int width) { object->width=width; }
void lv_obj_set_size(lv_obj_t* object, int width, int) { object->width=width; }
void lv_obj_align(lv_obj_t* object, int, int offset, int) { object->offset=offset; }
HELPER
int main() {
    SizeVoicePill(nullptr, nullptr, nullptr, false);
    for (const char* text : {"", "Thinking", "Search commits", "A very long activity that must be clipped safely"}) {
        for (bool icon : {false, true}) {
            lv_obj_t bar, label;
            SizeVoicePill(&bar, &label, text, icon);
            assert(bar.width >= 100 && bar.width <= 260);
            assert(label.width + 24 + (icon ? 32 : 0) == bar.width);
            assert(label.offset == (icon ? 16 : 0));
        }
    }
}
'''.replace("HELPER", helper)
with tempfile.TemporaryDirectory(prefix="voice-layout-") as directory:
    path = Path(directory)
    (path / "check.cc").write_text(program)
    subprocess.run(["c++", "-std=c++17", str(path / "check.cc"), "-o", str(path / "check")], check=True)
    subprocess.run([str(path / "check")], check=True)
print("PASS: activity pill fits content and reserves icon space within 260px")
