#include <gtest/gtest.h>

#include "TestDataUtils.hpp"

#include <cstdlib>
#include <string_view>

namespace snap::drawing {

TEST(TestDataUtils, loadsDeclaredLottieAndFontResources) {
    auto lottie = getTestData("lottie_loading.json");
    ASSERT_TRUE(lottie.success()) << lottie.description();
    std::string_view json(reinterpret_cast<const char*>(lottie.value().data()), lottie.value().size());
    EXPECT_NE(std::string_view::npos, json.find("\"layers\""));

    auto font = getTestData("montserrat_regular.ttf");
    ASSERT_TRUE(font.success()) << font.description();
    ASSERT_GE(font.value().size(), 4u);
    EXPECT_EQ(0, font.value().data()[0]);
    EXPECT_EQ(1, font.value().data()[1]);
    EXPECT_EQ(0, font.value().data()[2]);
    EXPECT_EQ(0, font.value().data()[3]);
}

TEST(TestDataUtils, missingResourceExitsWithDiagnosticInsteadOfCrashing) {
    EXPECT_EXIT(
        { (void)getTestData("missing-snap-drawing-test-resource"); },
        ::testing::ExitedWithCode(EXIT_FAILURE),
        "Unable to resolve SnapDrawing test resource 'missing-snap-drawing-test-resource'");
}

} // namespace snap::drawing
