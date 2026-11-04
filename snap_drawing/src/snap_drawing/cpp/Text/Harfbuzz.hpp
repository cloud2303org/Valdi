//
//  Harfbuzz.hpp
//  snap_drawing-macos
//
//  Created by Simon Corsin on 10/18/22.
//

#pragma once

#include "snap_drawing/cpp/Utils/Aliases.hpp"
#include "snap_drawing/cpp/Utils/DrawingBackend.hpp"
#include "utils/base/NonCopyable.hpp"

struct hb_blob_t;
struct hb_face_t;
struct hb_font_t;
struct hb_buffer_t;

namespace Valdi {
class CharacterSet;
}

namespace snap::drawing {

template<typename T>
class HBResource : public snap::NonCopyable {
public:
    HBResource() = default;
    explicit HBResource(T* ptr) : _ptr(ptr) {}

    T* get() const {
        return _ptr;
    }

protected:
    T* _ptr = nullptr;
};

class HBBlob : public HBResource<hb_blob_t> {
public:
    using HBResource<hb_blob_t>::HBResource;
    ~HBBlob();

    HBBlob(HBBlob&& other) noexcept;
    HBBlob& operator=(HBBlob&& other) noexcept;
};

class HBFace : public HBResource<hb_face_t> {
public:
    using HBResource<hb_face_t>::HBResource;
    ~HBFace();

    HBFace(HBFace&& other) noexcept;
    HBFace& operator=(HBFace&& other) noexcept;

    Ref<Valdi::CharacterSet> getCharacters();
};

class HBFont : public HBResource<hb_font_t> {
public:
    using HBResource<hb_font_t>::HBResource;
    ~HBFont();

    HBFont(HBFont&& other) noexcept;
    HBFont& operator=(HBFont&& other) noexcept;
};

class HBBuffer : public HBResource<hb_buffer_t> {
public:
    using HBResource<hb_buffer_t>::HBResource;
    ~HBBuffer();

    HBBuffer(HBBuffer&& other) noexcept;
    HBBuffer& operator=(HBBuffer&& other) noexcept;
};

class Harfbuzz {
public:
    static HBFace createFace(const NativeTypeface& typeface);
    static HBFont createMainFont(const NativeTypeface& typeface, const HBFace& hbFace);
    static HBFont createSubFont(const HBFont& mainFont, NativeFont* font);
};

} // namespace snap::drawing
