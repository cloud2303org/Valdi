#pragma once

#include "valdi_core/cpp/Utils/Shared.hpp"

#if defined(SK_GANESH) && !defined(SNAP_DRAWING_TGFX_EXPERIMENT)

#include "include/gpu/ganesh/GrDirectContext.h"

namespace snap::drawing {

class IShaderCache : public GrContextOptions::PersistentCache, public Valdi::SharedPtrRefCountable {};

} // namespace snap::drawing

#else

#include "snap_drawing/cpp/Utils/DrawingBackend.hpp"

namespace snap::drawing {

class IShaderCache : public Valdi::SharedPtrRefCountable {
public:
    virtual NativeData load(const NativeDataObject& key) = 0;
    virtual void store(const NativeDataObject& key, const NativeDataObject& data) = 0;
};

} // namespace snap::drawing

#endif
