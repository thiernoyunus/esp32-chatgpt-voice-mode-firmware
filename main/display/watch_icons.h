#pragma once

// Watch face icon assets for the round ESP32/LVGL UI
// Control icons are A8 (alpha-only) and tinted white where they are drawn.
// The Codex mark is the one full-colour asset - see below.
// Control icons:24x24

#include <lvgl.h>

namespace watch_icons {

// Codex mark: the blob with the chevron and bar knocked out of it. The white
// rounded square the logo sits in is the home tile itself, so the asset is
// only the glyph, at the tile's own 96px.
//
// The only full-colour icon here: RGB565A8, not A8, because the mark's
// identity is its purple-to-blue gradient and an alpha-only asset would
// flatten it to one tint. Icon() leaves non-A8 sources unrecoloured.
LV_IMAGE_DECLARE(codex);

// Lucide icons (MIT licensed, https://lucide.dev)
LV_IMAGE_DECLARE(settings);
LV_IMAGE_DECLARE(clock);
LV_IMAGE_DECLARE(back);
LV_IMAGE_DECLARE(more);

} // namespace watch_icons

