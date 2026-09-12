#pragma once

#include <cstdint>

/* The character the watch wears on the call screen: one of bloub's eight
 * silhouettes, in one of these colours.
 *
 * One copy, because there were two - the swatches on the Colour page and the
 * body the renderer fills were separate literal lists in separate files, and
 * nothing would have caught them drifting apart. Picking a colour would then
 * have given you a different one.
 *
 * The counts live here for the same reason: the pickers, the settings that
 * persist them and the renderer all have to agree on how many there are, and
 * they were agreeing by three hand-written numbers. bloub_shapes.h is not the
 * place for any of this - that folder is the ported character, and this is our
 * choice of palette on top of it. */
namespace voice_character {

inline constexpr uint32_t kColors[] = {
    0xF1EFE9, 0xA3A3A3, 0x8B5E3C, 0xE8483F, 0xF08A24, 0xF0B429,
    0x3ECF8E, 0x2FBFA0, 0x3B93F0, 0x8B5CF6, 0xE152B0,
};
inline constexpr int kColorCount = static_cast<int>(sizeof(kColors) / sizeof(kColors[0]));

/* Checked against bloub's own SHAPE_COUNT in lcd_display.cc, which is the one
 * place that includes both this and the port. Kept as a number here so files
 * that only need the bound - the settings, the picker - do not have to pull in
 * the silhouette tables to get it. */
inline constexpr int kShapeCount = 8;

}  // namespace voice_character
