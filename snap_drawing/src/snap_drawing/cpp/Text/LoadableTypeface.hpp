//
//  LoadableTypeface.hpp
//  valdi-desktop-apple
//
//  Created by Simon Corsin on 9/2/20.
//

#pragma once

#include "snap_drawing/cpp/Text/Typeface.hpp"

#include "valdi_core/cpp/Utils/Bytes.hpp"
#include "valdi_core/cpp/Utils/Result.hpp"

#include "snap_drawing/cpp/Text/NativeFontManager.hpp"

namespace Valdi {
class Path;
}

namespace snap::drawing {

class LoadableTypeface : public Valdi::SimpleRefCountable {
public:
    explicit LoadableTypeface(const String& fontName);
    ~LoadableTypeface() override;

    const Valdi::Result<Ref<Typeface>>& get(NativeFontManager& fontMgr);

    const String& getName() const;

    static Ref<LoadableTypeface> fromBytes(const String& fontName, const Valdi::BytesView& bytes);
    static Ref<LoadableTypeface> fromFile(const String& fontName, const Valdi::StringBox& filePath);

protected:
    virtual Valdi::Result<Valdi::BytesView> loadFontData() = 0;

private:
    Valdi::Result<Ref<Typeface>> _loadedTypeface;
    String _fontName;

    Valdi::Result<Ref<Typeface>> loadTypeface(NativeFontManager& fontMgr);
};

} // namespace snap::drawing
