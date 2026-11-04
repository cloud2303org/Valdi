cmake_minimum_required(VERSION 3.20)
project(SnapDrawingTGFXMobile LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

get_filename_component(EXPERIMENT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
get_filename_component(FRAMEWORK "${EXPERIMENT}/../../.." ABSOLUTE)
foreach(REQUIRED TGFX_SOURCE TGFX_LIBRARY FMT_INCLUDE)
    if(NOT EXISTS "${${REQUIRED}}")
        message(FATAL_ERROR "${REQUIRED} must point to a real existing input")
    endif()
endforeach()

set(DRAWING "${FRAMEWORK}/snap_drawing/src/snap_drawing/cpp")
add_library(snap_vector STATIC
    "${DRAWING}/Drawing/DrawingContext.cpp"
    "${DRAWING}/Drawing/Paint.cpp"
    "${DRAWING}/Drawing/LayerContent.cpp"
    "${DRAWING}/Utils/Geometry.cpp"
    "${DRAWING}/Utils/Matrix.cpp"
    "${DRAWING}/Utils/Path.cpp"
    "${DRAWING}/Utils/Color.cpp"
    "${DRAWING}/Utils/BorderRadius.cpp"
    "${DRAWING}/Utils/LazyPath.cpp"
    "${EXPERIMENT}/vector_checks.cpp"
)
target_compile_definitions(snap_vector PUBLIC SNAP_DRAWING_TGFX_EXPERIMENT FMT_HEADER_ONLY)
target_include_directories(snap_vector PUBLIC
    "${FRAMEWORK}/snap_drawing/src" "${FRAMEWORK}/libs/utils/src"
    "${TGFX_SOURCE}/include" "${FMT_INCLUDE}" "${EXPERIMENT}"
)
target_link_libraries(snap_vector PUBLIC "${TGFX_LIBRARY}")

if(IOS)
    enable_language(OBJCXX)
    add_executable(TGFXVectorProbe MACOSX_BUNDLE ios/main.mm)
    set_target_properties(TGFXVectorProbe PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_LIST_DIR}/ios/Info.plist"
        MACOSX_BUNDLE_GUI_IDENTIFIER io.valdiroutes.tgfxprobe
        XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED NO
    )
    target_link_libraries(TGFXVectorProbe PRIVATE snap_vector
        "-framework UIKit" "-framework Foundation" "-framework OpenGLES"
        "-framework CoreGraphics" "-framework CoreText" "-framework CoreVideo"
        "-framework QuartzCore" "-framework ImageIO" "-framework CoreMedia"
        "-framework VideoToolbox" "-framework AVFoundation" "-liconv"
    )
elseif(ANDROID)
    add_library(snap_tgfx_probe SHARED android/probe.cpp)
    target_link_libraries(snap_tgfx_probe PRIVATE snap_vector EGL GLESv2 GLESv3 android log jnigraphics)
    target_link_options(snap_tgfx_probe PRIVATE "-Wl,--no-undefined")
else()
    message(FATAL_ERROR "This target is only for iOS or Android")
endif()
