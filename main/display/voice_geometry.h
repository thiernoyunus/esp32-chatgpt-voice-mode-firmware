#pragma once

namespace voice_geometry {
constexpr int kButtonSize = 52;
constexpr int kButtonTop = 246;
constexpr int kMuteLeft = 62;
constexpr int kEndLeft = 246;
constexpr int kOrbSize = 166;
constexpr int kModelLeft = 100, kModelTop = 24, kModelWidth = 160, kModelHeight = 32;
constexpr int kModelRowLeft = 70, kModelRowTop = 100, kModelRowWidth = 220;
constexpr int kModelRowHeight = 44, kModelRowStep = 54, kModelsPerPage = 3;
constexpr bool ContainsModelPicker(int x, int y) {
    return x >= kModelLeft && x < kModelLeft + kModelWidth && y >= kModelTop &&
           y < kModelTop + kModelHeight;
}
constexpr int ModelRowAt(int x, int y) {
    if (x < kModelRowLeft || x >= kModelRowLeft + kModelRowWidth || y < kModelRowTop)
        return -1;
    const int row = (y - kModelRowTop) / kModelRowStep;
    return row < kModelsPerPage && (y - kModelRowTop) % kModelRowStep < kModelRowHeight ? row : -1;
}

constexpr bool ContainsButton(int left, int x, int y) {
    const int dx = x - left - kButtonSize / 2;
    const int dy = y - kButtonTop - kButtonSize / 2;
    return dx * dx + dy * dy <= kButtonSize * kButtonSize / 4;
}
static_assert(ContainsButton(kMuteLeft, 88, 272));
static_assert(ContainsButton(kEndLeft, 272, 272));
static_assert(!ContainsButton(kMuteLeft, 180, 300));
static_assert(!ContainsButton(kEndLeft, 180, 300));
static_assert((kMuteLeft - 180) * (kMuteLeft - 180) +
                  (kButtonTop + kButtonSize - 180) * (kButtonTop + kButtonSize - 180) <
              180 * 180);
static_assert((kEndLeft + kButtonSize - 180) * (kEndLeft + kButtonSize - 180) +
                  (kButtonTop + kButtonSize - 180) * (kButtonTop + kButtonSize - 180) <
              180 * 180);
}  // namespace voice_geometry
