#include "valdi/callback_attribution_test_assets.hpp"
#include "valdi_core/cpp/Resources/StaticResourceRegistry.hpp"

#include "gtest/gtest.h"
#include "rules_cc/cc/runfiles/runfiles.h"

#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace {

// Only the focused Node-backed target links this environment. The broader
// integration suite supplies its own embedded modules.
class CallbackAttributionTestAssets final : public ::testing::Environment {
public:
    void SetUp() override {
        if (_registered) {
            return;
        }
        _assets.clear();
        std::string error;
        std::unique_ptr<rules_cc::cc::runfiles::Runfiles> runfiles(
            rules_cc::cc::runfiles::Runfiles::CreateForTest(&error));
        ASSERT_NE(nullptr, runfiles) << "Could not initialize runtime module runfiles: " << error;

        for (const auto* relative : ValdiRuntimeTestAssets::runfilePaths) {
            const auto path = runfiles->Rlocation(relative);
            std::ifstream input(path, std::ios::binary);
            ASSERT_TRUE(input) << "Could not load Node-compiled runtime module: " << relative << " (" << path << ")";
            Asset asset;
            asset.name = std::string(relative).substr(std::string(relative).find_last_of('/') + 1);
            asset.bytes.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
            ASSERT_FALSE(input.bad()) << "Could not read runtime module: " << path;
            ASSERT_FALSE(asset.bytes.empty()) << "Empty runtime module: " << path;
            _assets.push_back(std::move(asset));
        }

        // Load everything before registering borrowed pointers, so vector
        // growth cannot invalidate names and no partial registry is published.
        for (const auto& asset : _assets) {
            Valdi::StaticResourceRegistry::sharedInstance().registerResource(
                asset.name.c_str(), asset.bytes.data(), asset.bytes.size(), false);
        }
        _registered = true;
    }

private:
    struct Asset {
        std::string name;
        std::vector<uint8_t> bytes;
    };
    std::vector<Asset> _assets;
    bool _registered = false;
};

[[maybe_unused]] auto* const environment =
    ::testing::AddGlobalTestEnvironment(new CallbackAttributionTestAssets());

TEST(CallbackAttributionAssets, registrationIsStableAcrossRepeatedSetUp) {
    const auto count = Valdi::StaticResourceRegistry::sharedInstance().getResources().size();
    ASSERT_GE(count, 4u);
    environment->SetUp();
    EXPECT_EQ(count, Valdi::StaticResourceRegistry::sharedInstance().getResources().size());
    environment->SetUp();
    EXPECT_EQ(count, Valdi::StaticResourceRegistry::sharedInstance().getResources().size());
}

} // namespace
