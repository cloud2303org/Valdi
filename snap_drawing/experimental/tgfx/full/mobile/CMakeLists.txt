cmake_minimum_required(VERSION 3.24)
project(TGFXFullProbe LANGUAGES CXX OBJCXX)

if(NOT IOS)
    message(FATAL_ERROR "This CMake project is the iOS app shell only; parent Bazel owns all native checks")
endif()

set(TGFX_FULL_CHECKS_ARCHIVE "" CACHE FILEPATH "Parent-built full mobile checks static archive")
set(TGFX_FULL_LINK_LIBRARIES "" CACHE STRING "Explicit parent-supplied transitive native link libraries")
set(TGFX_FULL_MEDIA_DATA "${CMAKE_CURRENT_LIST_DIR}/../tests/media-data" CACHE PATH "Real media fixtures")
set(TGFX_FULL_TEXT_DATA "${CMAKE_CURRENT_LIST_DIR}/../tests/text-data" CACHE PATH "Real text fixtures")

if(NOT IS_ABSOLUTE "${TGFX_FULL_CHECKS_ARCHIVE}" OR NOT EXISTS "${TGFX_FULL_CHECKS_ARCHIVE}" OR
   IS_DIRECTORY "${TGFX_FULL_CHECKS_ARCHIVE}")
    message(FATAL_ERROR "Supply an absolute TGFX_FULL_CHECKS_ARCHIVE built for this iOS SDK and architecture")
endif()
foreach(DATA_DIR IN ITEMS TGFX_FULL_MEDIA_DATA TGFX_FULL_TEXT_DATA)
    if(NOT IS_DIRECTORY "${${DATA_DIR}}")
        message(FATAL_ERROR "${DATA_DIR} must contain the parent's real fixtures")
    endif()
endforeach()
if(NOT EXISTS "${TGFX_FULL_MEDIA_DATA}/manifest.json" OR
   NOT EXISTS "${TGFX_FULL_MEDIA_DATA}/NotoSans-Regular.ttf" OR
   NOT EXISTS "${TGFX_FULL_TEXT_DATA}/RobotoFlex.ttf")
    message(FATAL_ERROR "Missing real fixture manifest/fonts; do not substitute empty resources")
endif()

add_library(tgfx_full_checks STATIC IMPORTED)
set_target_properties(tgfx_full_checks PROPERTIES IMPORTED_LOCATION "${TGFX_FULL_CHECKS_ARCHIVE}")

# Only the UIKit entry point is compiled here. The parent archive must already
# include full_checks.cpp, the real checks and the complete backend link closure.
add_executable(TGFXFullProbe MACOSX_BUNDLE ios/main.mm)
target_compile_features(TGFXFullProbe PRIVATE cxx_std_20)
set_target_properties(TGFXFullProbe PROPERTIES OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED YES)
target_compile_options(TGFXFullProbe PRIVATE "-fobjc-arc")
target_include_directories(TGFXFullProbe PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
set_target_properties(TGFXFullProbe PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_LIST_DIR}/ios/Info.plist"
    MACOSX_BUNDLE_GUI_IDENTIFIER io.valdiroutes.tgfxfullprobe
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER io.valdiroutes.tgfxfullprobe
    XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET 15.0
    XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED NO
)
target_link_libraries(TGFXFullProbe PRIVATE tgfx_full_checks ${TGFX_FULL_LINK_LIBRARIES}
    "-framework UIKit" "-framework Foundation" "-framework Metal" "-framework MetalKit"
    "-framework CoreGraphics" "-framework CoreText" "-framework CoreVideo"
    "-framework QuartzCore" "-framework ImageIO" "-framework CoreMedia"
    "-framework VideoToolbox" "-framework AVFoundation" "-framework Accelerate"
    "-framework OpenGLES" "-framework Security" "-liconv" "-lz" "-lc++"
)
# The entry point references every suite directly. Keep Objective-C categories
# without force-loading unused copies of transitive C dependencies.
target_link_options(TGFXFullProbe PRIVATE "-ObjC")

function(bundle_fixture_tree DIRECTORY NAME)
    file(GLOB_RECURSE FIXTURES CONFIGURE_DEPENDS LIST_DIRECTORIES false "${DIRECTORY}/*")
    list(FILTER FIXTURES EXCLUDE REGEX "\\.py$")
    foreach(FIXTURE IN LISTS FIXTURES)
        file(RELATIVE_PATH RELATIVE_FIXTURE "${DIRECTORY}" "${FIXTURE}")
        get_filename_component(SUBDIRECTORY "${RELATIVE_FIXTURE}" DIRECTORY)
        set_source_files_properties("${FIXTURE}" PROPERTIES
            MACOSX_PACKAGE_LOCATION "Resources/fixtures/${NAME}/${SUBDIRECTORY}"
        )
        target_sources(TGFXFullProbe PRIVATE "${FIXTURE}")
    endforeach()
endfunction()

bundle_fixture_tree("${TGFX_FULL_MEDIA_DATA}" media-data)
bundle_fixture_tree("${TGFX_FULL_TEXT_DATA}" text-data)
