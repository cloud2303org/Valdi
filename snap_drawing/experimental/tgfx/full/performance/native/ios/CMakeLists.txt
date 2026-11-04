cmake_minimum_required(VERSION 3.24)
project(NativeRenderPerformance LANGUAGES CXX OBJCXX)

if(NOT IOS)
    message(FATAL_ERROR "This project is the iOS UIKit shell; Bazel owns the native benchmark/backend")
endif()

set(RENDER_PERFORMANCE_ARCHIVE "" CACHE FILEPATH "Absolute parent-built merged native archive for the selected SDK")
set(RENDER_PERFORMANCE_LINK_LIBRARIES "" CACHE STRING "Explicit remaining native link closure, if not already merged")
set(RENDER_PERFORMANCE_BACKEND "skia" CACHE STRING "skia or tgfx; must match the imported native archive")
set(RENDER_PERFORMANCE_FIXTURES "${CMAKE_CURRENT_LIST_DIR}/../../../tests/media-data" CACHE PATH "Unchanged pinned fixtures")
set(RENDER_PERFORMANCE_SIGNING_ALLOWED "NO" CACHE STRING "Parent can set YES and provide its signing identity/team")
if(NOT RENDER_PERFORMANCE_BACKEND MATCHES "^(skia|tgfx)$")
    message(FATAL_ERROR "RENDER_PERFORMANCE_BACKEND must be skia or tgfx")
endif()
if(NOT IS_ABSOLUTE "${RENDER_PERFORMANCE_ARCHIVE}" OR NOT EXISTS "${RENDER_PERFORMANCE_ARCHIVE}" OR
   IS_DIRECTORY "${RENDER_PERFORMANCE_ARCHIVE}")
    message(FATAL_ERROR "Supply a real parent-built RENDER_PERFORMANCE_ARCHIVE for this SDK/architecture")
endif()

set(FIXTURE_NAMES NotoSans-Regular.ttf NotoSans-LICENSE.txt pattern.png pattern.svg moving-solid.json
    manifest.json PROVENANCE.md)
foreach(NAME IN LISTS FIXTURE_NAMES)
    if(NOT EXISTS "${RENDER_PERFORMANCE_FIXTURES}/${NAME}")
        message(FATAL_ERROR "Missing real benchmark fixture ${NAME}")
    endif()
endforeach()

# A generated Xcode project exposes Release only. No native backend source is
# rebuilt here; optimization of the imported archive is checked in its report.
set(CMAKE_CONFIGURATION_TYPES "Release" CACHE STRING "" FORCE)
set(CMAKE_BUILD_TYPE "Release" CACHE STRING "" FORCE)
add_library(native_render_performance STATIC IMPORTED)
set_target_properties(native_render_performance PROPERTIES IMPORTED_LOCATION "${RENDER_PERFORMANCE_ARCHIVE}")
add_executable(NativeRenderPerformance MACOSX_BUNDLE main.mm)
target_compile_features(NativeRenderPerformance PRIVATE cxx_std_20)
target_compile_options(NativeRenderPerformance PRIVATE "-fobjc-arc")
set_target_properties(NativeRenderPerformance PROPERTIES
    OBJCXX_STANDARD 20
    OBJCXX_STANDARD_REQUIRED YES
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_LIST_DIR}/Info.plist"
    MACOSX_BUNDLE_GUI_IDENTIFIER io.valdiroutes.renderperformance
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER io.valdiroutes.renderperformance
    XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET 15.0
    XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED "${RENDER_PERFORMANCE_SIGNING_ALLOWED}"
    XCODE_ATTRIBUTE_ENABLE_TESTABILITY NO
    XCODE_ATTRIBUTE_DEBUG_INFORMATION_FORMAT "dwarf-with-dsym"
)
if(RENDER_PERFORMANCE_BACKEND STREQUAL "tgfx")
    target_compile_definitions(NativeRenderPerformance PRIVATE RENDER_PERFORMANCE_EXPECTED_BACKEND="full_tgfx")
else()
    target_compile_definitions(NativeRenderPerformance PRIVATE RENDER_PERFORMANCE_EXPECTED_BACKEND="skia")
endif()
target_link_libraries(NativeRenderPerformance PRIVATE native_render_performance ${RENDER_PERFORMANCE_LINK_LIBRARIES}
    "-framework UIKit" "-framework Foundation" "-framework Metal" "-framework MetalKit"
    "-framework CoreGraphics" "-framework CoreText" "-framework CoreVideo" "-framework QuartzCore"
    "-framework ImageIO" "-framework CoreMedia" "-framework VideoToolbox" "-framework AVFoundation"
    "-framework Accelerate" "-framework OpenGLES" "-framework Security" "-liconv" "-lz" "-lc++"
)
target_link_options(NativeRenderPerformance PRIVATE "-ObjC")
foreach(NAME IN LISTS FIXTURE_NAMES)
    set(FIXTURE "${RENDER_PERFORMANCE_FIXTURES}/${NAME}")
    set_source_files_properties("${FIXTURE}" PROPERTIES MACOSX_PACKAGE_LOCATION "Resources/fixtures")
    target_sources(NativeRenderPerformance PRIVATE "${FIXTURE}")
endforeach()
