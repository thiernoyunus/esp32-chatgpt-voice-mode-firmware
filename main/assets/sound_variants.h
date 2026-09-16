// Pitch-shifted siblings of the frequent UI sounds (v2 ~one semitone down,
// v3 ~one semitone up, generated at asset-conversion time). Picking one at
// random keeps a repeated effect from sounding machine-identical.
#pragma once

#include <string_view>

#include <esp_random.h>

#include "lang_config.h"

namespace Lang {
    namespace SoundVariants {

        inline const std::string_view& Pick(const std::string_view* variants, size_t count) {
            return variants[esp_random() % count];
        }

        inline const std::string_view& ModeSwitch() {
            static const std::string_view variants[] = {
                Sounds::OGG_MODE_SWITCH, Sounds::OGG_MODE_SWITCH_V2, Sounds::OGG_MODE_SWITCH_V3};
            return Pick(variants, 3);
        }

        inline const std::string_view& ListenStart() {
            static const std::string_view variants[] = {
                Sounds::OGG_LISTEN_START, Sounds::OGG_LISTEN_START_V2, Sounds::OGG_LISTEN_START_V3};
            return Pick(variants, 3);
        }

        inline const std::string_view& ListenEnd() {
            static const std::string_view variants[] = {
                Sounds::OGG_LISTEN_END, Sounds::OGG_LISTEN_END_V2, Sounds::OGG_LISTEN_END_V3};
            return Pick(variants, 3);
        }

        inline const std::string_view& SpeechDone() {
            static const std::string_view variants[] = {
                Sounds::OGG_SPEECH_DONE, Sounds::OGG_SPEECH_DONE_V2, Sounds::OGG_SPEECH_DONE_V3};
            return Pick(variants, 3);
        }
    }
}
