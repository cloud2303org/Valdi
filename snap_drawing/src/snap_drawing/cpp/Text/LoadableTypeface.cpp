//
//  LoadableTypeface.cpp
//  valdi-desktop-apple
//
//  Created by Simon Corsin on 9/2/20.
//

#include "snap_drawing/cpp/Text/LoadableTypeface.hpp"
#ifndef SNAP_DRAWING_TGFX_EXPERIMENT
#include "include/core/SkData.h"
#include "snap_drawing/cpp/Utils/BytesUtils.hpp"
#endif
#include "snap_drawing/cpp/Text/NativeFontManager.hpp"
#include "valdi_core/cpp/Utils/ByteBuffer.hpp"
#include "valdi_core/cpp/Utils/DiskUtils.hpp"

namespace snap::drawing {

class BytesLoadableTypeface : public LoadableTypeface {
public:
    BytesLoadableTypeface(const String& fontName, const Valdi::BytesView& bytes)
        : LoadableTypeface(fontName), _bytes(bytes) {
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
        // Registration is lazy. Retain an owner now, before the registering
        // caller can release a borrowed font buffer.
        if (_bytes.getSource() == nullptr && !_bytes.empty()) {
            _bytes = Valdi::makeShared<Valdi::ByteBuffer>(_bytes.begin(), _bytes.end())->toBytesView();
        }
#endif
    }
    ~BytesLoadableTypeface() override = default;

protected:
    Valdi::Result<Valdi::BytesView> loadFontData() override {
        return std::move(_bytes);
    }

private:
    Valdi::BytesView _bytes;
};

class FileLoadableTypeface : public LoadableTypeface {
public:
    FileLoadableTypeface(const String& fontName, const String& path) : LoadableTypeface(fontName), _path(path) {}
    ~FileLoadableTypeface() override = default;

protected:
    Valdi::Result<Valdi::BytesView> loadFontData() override {
        Valdi::Path path(_path.toStringView());
        return Valdi::DiskUtils::load(path);
    }

private:
    Valdi::StringBox _path;
};

LoadableTypeface::LoadableTypeface(const String& fontName) : _fontName(fontName) {}
LoadableTypeface::~LoadableTypeface() = default;

const Valdi::Result<Ref<Typeface>>& LoadableTypeface::get(NativeFontManager& fontMgr) {
    if (_loadedTypeface.empty()) {
        _loadedTypeface = loadTypeface(fontMgr);
    }
    return _loadedTypeface;
}

const String& LoadableTypeface::getName() const {
    return _fontName;
}

Valdi::Result<Ref<Typeface>> LoadableTypeface::loadTypeface(NativeFontManager& fontMgr) {
    auto fontDataResult = loadFontData();
    if (!fontDataResult) {
        return fontDataResult.error().rethrow("Could not load typeface data");
    }

    auto fontData = fontDataResult.moveValue();
#ifdef SNAP_DRAWING_TGFX_EXPERIMENT
    // MakeFromBytes takes an owned copy. A BytesView supplied without an owner
    // may disappear as soon as this call returns; TGFX never borrows that view.
    (void)fontMgr;
    auto skTypeface = tgfx::Typeface::MakeFromBytes(fontData.data(), fontData.size());
#else
    auto skData = skDataFromBytes(fontData, DataConversionModeCopyIfUnsafe);
    auto skTypeface = fontMgr.makeFromData(skData);
#endif

    if (skTypeface == nullptr) {
        return Valdi::Error("Could not load typeface from data");
    }

    return Valdi::makeShared<Typeface>(std::move(skTypeface), _fontName, true);
}

Ref<LoadableTypeface> LoadableTypeface::fromBytes(const String& fontName, const Valdi::BytesView& bytes) {
    return Valdi::makeShared<BytesLoadableTypeface>(fontName, bytes);
}

Ref<LoadableTypeface> LoadableTypeface::fromFile(const String& fontName, const Valdi::StringBox& filePath) {
    return Valdi::makeShared<FileLoadableTypeface>(fontName, filePath);
}

} // namespace snap::drawing
