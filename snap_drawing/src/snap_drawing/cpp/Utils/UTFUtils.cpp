//
//  UTFUtils.cpp
//  snap_drawing
//
//  Created by Simon Corsin on 3/16/22.
//

#include "snap_drawing/cpp/Utils/UTFUtils.hpp"
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
#include <unicode/utf8.h>
#include <limits>
#else
#include "include/core/SkTypes.h"

namespace SkUTF {

// Those APIs exist in Skia but not in the public headers
SK_SPI int CountUTF8(const char* utf8, size_t byteLength);    // NOLINT
SK_SPI SkUnichar NextUTF8(const char** ptr, const char* end); // NOLINT
SK_SPI size_t ToUTF8(SkUnichar uni, char* utf8);              // NOLINT

} // namespace SkUTF
#endif

namespace snap::drawing {

std::vector<Character> utf8ToUnicode(std::string_view utf8) {
    std::vector<Character> out;
    utf8ToUnicode(utf8, out);
    return out;
}

size_t utf8ToUnicode(std::string_view utf8, std::vector<Character>& output) {
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
    if (utf8.size() > static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
        return 0;
    }
    std::vector<Character> decoded;
    int32_t index = 0;
    const auto length = static_cast<int32_t>(utf8.size());
    while (index < length) {
        UChar32 character;
        U8_NEXT(utf8.data(), index, length, character);
        if (character < 0) {
            return 0;
        }
        decoded.push_back(static_cast<Character>(character));
    }
    output.insert(output.end(), decoded.begin(), decoded.end());
    return decoded.size();
#else
    const auto* utf8Start = utf8.data();

    auto unicodeCount = SkUTF::CountUTF8(utf8Start, utf8.size());
    if (unicodeCount <= 0) {
        return 0;
    }

    auto previousSize = output.size();
    output.resize(previousSize + unicodeCount);

    auto* unicodePtr = reinterpret_cast<SkUnichar*>(output.data() + previousSize);

    const auto* utf8End = utf8Start + utf8.size();

    while (utf8Start < utf8End) {
        (*unicodePtr) = SkUTF::NextUTF8(&utf8Start, utf8End);
        unicodePtr++;
    }

    return unicodeCount;
#endif
}

std::string unicodeToUtf8(const Character* unicode, size_t length) {
    std::string characters;
    char buffer[4];

#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
    for (size_t i = 0; i < length; i++) {
        if (unicode[i] > 0x10FFFF || (unicode[i] >= 0xD800 && unicode[i] <= 0xDFFF)) {
            continue;
        }
        int32_t written = 0;
        U8_APPEND_UNSAFE(buffer, written, unicode[i]);
        characters.append(buffer, static_cast<size_t>(written));
    }
#else
    const auto* skCharactersPtr = reinterpret_cast<const SkUnichar*>(unicode);

    for (size_t i = 0; i < length; i++) {
        size_t written = SkUTF::ToUTF8(skCharactersPtr[i], buffer);
        characters.append(buffer, written);
    }
#endif

    return characters;
}

} // namespace snap::drawing
