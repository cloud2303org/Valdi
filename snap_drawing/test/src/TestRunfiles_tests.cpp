#include <gtest/gtest.h>

#include "TestRunfiles.hpp"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <unistd.h>

namespace snap::drawing {

namespace {

namespace fs = std::filesystem;

class TestRunfilesTest : public ::testing::Test {
protected:
    void SetUp() override {
        oldCwd = fs::current_path();
        for (size_t i = 0; i < environmentNames.size(); i++) {
            auto* value = std::getenv(environmentNames[i]);
            if (value != nullptr) {
                oldEnvironment[i] = value;
            }
            ::unsetenv(environmentNames[i]);
        }

        auto pattern = (fs::temp_directory_path() / "snap-runfiles-XXXXXX").string();
        auto* directory = ::mkdtemp(pattern.data());
        ASSERT_NE(nullptr, directory);
        root = fs::canonical(directory);
        fs::create_directories(root / "unrelated");
        fs::current_path(root / "unrelated");
    }

    void TearDown() override {
        fs::current_path(oldCwd);
        for (size_t i = 0; i < environmentNames.size(); i++) {
            if (oldEnvironment[i].has_value()) {
                ::setenv(environmentNames[i], oldEnvironment[i]->c_str(), 1);
            } else {
                ::unsetenv(environmentNames[i]);
            }
        }
        if (!root.empty()) {
            fs::remove_all(root);
        }
    }

    void write(const fs::path& path, const std::string& contents) {
        fs::create_directories(path.parent_path());
        std::ofstream stream(path, std::ios::binary);
        ASSERT_TRUE(stream.is_open()) << path;
        stream << contents;
        ASSERT_TRUE(stream.good()) << path;
    }

    void writeMapping(const fs::path& path, const std::string& canonicalRepository) {
        write(path, std::string(BAZEL_CURRENT_REPOSITORY) + ",valdi," + canonicalRepository + "\n");
    }

    void expectResource(const fs::path& expected) {
        std::string error;
        auto path = resolveTestDataFile("fixture.txt", error);
        ASSERT_EQ(expected.string(), path) << error;
        EXPECT_TRUE(error.empty()) << error;
        std::ifstream stream(path, std::ios::binary);
        const std::string contents{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
        EXPECT_EQ("resource contents\n", contents);
    }

    fs::path root;

private:
    const std::array<const char*, 3> environmentNames = {
        "TEST_SRCDIR", "RUNFILES_DIR", "RUNFILES_MANIFEST_FILE"};
    std::array<std::optional<std::string>, 3> oldEnvironment;
    fs::path oldCwd;
};

TEST_F(TestRunfilesTest, resolvesStandaloneCheckoutRoot) {
    auto resource = root / "checkout/snap_drawing/testdata/fixture.txt";
    write(resource, "resource contents\n");
    fs::current_path(root / "checkout");
    expectResource(resource);
}

TEST_F(TestRunfilesTest, resolvesMainRepositoryFromUnrelatedCwd) {
    auto runfiles = root / "main.runfiles";
    auto resource = runfiles / "_main/snap_drawing/testdata/fixture.txt";
    write(resource, "resource contents\n");
    writeMapping(runfiles / "_repo_mapping", "_main");
    ::setenv("TEST_SRCDIR", runfiles.c_str(), 1);
    expectResource(resource);
}

TEST_F(TestRunfilesTest, resolvesMappedExternalRepositoryFromUnrelatedCwd) {
    auto runfiles = root / "external.runfiles";
    auto resource = runfiles / "renamed_valdi+/snap_drawing/testdata/fixture.txt";
    write(resource, "resource contents\n");
    writeMapping(runfiles / "_repo_mapping", "renamed_valdi+");
    ::setenv("TEST_SRCDIR", runfiles.c_str(), 1);
    expectResource(resource);
}

TEST_F(TestRunfilesTest, resolvesRunfilesDirForDirectInvocation) {
    auto runfiles = root / "direct.runfiles";
    auto resource = runfiles / "valdi/snap_drawing/testdata/fixture.txt";
    write(resource, "resource contents\n");
    ::setenv("RUNFILES_DIR", runfiles.c_str(), 1);
    expectResource(resource);
}

TEST_F(TestRunfilesTest, resolvesIndividualManifestEntryWithoutDataDirectoryEntry) {
    auto resource = root / "physical files/fixture.txt";
    auto mapping = root / "mapping";
    auto manifest = root / "MANIFEST";
    write(resource, "resource contents\n");
    writeMapping(mapping, "external+");
    write(manifest, "_repo_mapping " + mapping.string() + "\n" +
                        "external+/snap_drawing/testdata/fixture.txt " + resource.string() + "\n");
    ::setenv("RUNFILES_MANIFEST_FILE", manifest.c_str(), 1);
    expectResource(resource);
}

TEST_F(TestRunfilesTest, resolvesManifestDirectoryEntry) {
    auto dataDirectory = root / "physical data";
    auto resource = dataDirectory / "fixture.txt";
    auto mapping = root / "mapping";
    auto manifest = root / "MANIFEST";
    write(resource, "resource contents\n");
    writeMapping(mapping, "external+");
    write(manifest, "_repo_mapping " + mapping.string() + "\n" +
                        "external+/snap_drawing/testdata " + dataDirectory.string() + "\n");
    ::setenv("RUNFILES_MANIFEST_FILE", manifest.c_str(), 1);
    expectResource(resource);
}

TEST_F(TestRunfilesTest, prefersRunfilesOverCheckoutFile) {
    auto runfiles = root / "test.runfiles";
    auto resource = runfiles / "valdi/snap_drawing/testdata/fixture.txt";
    write(resource, "resource contents\n");
    write(root / "unrelated/snap_drawing/testdata/fixture.txt", "wrong checkout\n");
    ::setenv("TEST_SRCDIR", runfiles.c_str(), 1);
    expectResource(resource);
}

TEST_F(TestRunfilesTest, reportsMissingPhysicalManifestResource) {
    auto manifest = root / "MANIFEST";
    auto missing = root / "missing.txt";
    write(manifest, "valdi/snap_drawing/testdata/fixture.txt " + missing.string() + "\n");
    ::setenv("RUNFILES_MANIFEST_FILE", manifest.c_str(), 1);

    std::string error;
    EXPECT_TRUE(resolveTestDataFile("fixture.txt", error).empty());
    EXPECT_NE(std::string::npos, error.find("Unable to resolve SnapDrawing test resource 'fixture.txt'"));
    EXPECT_NE(std::string::npos, error.find(missing.string()));
    EXPECT_NE(std::string::npos, error.find(manifest.string()));
}

TEST_F(TestRunfilesTest, doesNotSearchParentDirectories) {
    write(root / "snap_drawing/testdata/fixture.txt", "resource contents\n");
    std::string error;
    EXPECT_TRUE(resolveTestDataFile("fixture.txt", error).empty());
    EXPECT_NE(std::string::npos, error.find((root / "unrelated/snap_drawing/testdata/fixture.txt").string()));
    EXPECT_NE(std::string::npos, error.find("Runfiles initialization:"));
}

TEST_F(TestRunfilesTest, rejectsPathsOutsideTestData) {
    write(root / "outside.txt", "outside testdata\n");
    std::string error;
    EXPECT_TRUE(resolveTestDataFile("../outside.txt", error).empty());
    EXPECT_NE(std::string::npos, error.find("Invalid SnapDrawing test resource name"));
    EXPECT_TRUE(resolveTestDataFile((root / "outside.txt").string(), error).empty());
}

} // namespace

} // namespace snap::drawing
