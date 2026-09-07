#pragma once

// Watch face icon assets for Apollo round ESP32/LVGL UI
// All icons are A8 (alpha-only) format for tinting with any color
// ChatGPT logo:52x52 for96px tile center
// Control icons:24x24

#include <lvgl.h>

namespace watch_icons {

// Official ChatGPT/OpenAI knot logo (from chat.openai.com/favicon.svg)
LV_IMAGE_DECLARE(chatgpt);

// Lucide icons (MIT licensed, https://lucide.dev)
LV_IMAGE_DECLARE(settings);
LV_IMAGE_DECLARE(wifi);
LV_IMAGE_DECLARE(sun);
LV_IMAGE_DECLARE(volume);
LV_IMAGE_DECLARE(clock);
LV_IMAGE_DECLARE(back);
LV_IMAGE_DECLARE(more);
LV_IMAGE_DECLARE(home);
LV_IMAGE_DECLARE(info);
LV_IMAGE_DECLARE(shield);
LV_IMAGE_DECLARE(mic);
LV_IMAGE_DECLARE(mic_off);
LV_IMAGE_DECLARE(close);

} // namespace watch_icons

