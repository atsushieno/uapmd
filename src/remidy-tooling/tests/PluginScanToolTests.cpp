#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include <remidy-tooling/PluginScanTool.hpp>

namespace {

std::string toLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::filesystem::path createCacheFilePath(const std::string& testName) {
    auto candidate = std::filesystem::temp_directory_path() / ("remidy-tooling-" + testName + "-plugins.json");
    std::error_code ec;
    std::filesystem::remove(candidate, ec);
    return candidate;
}

class PluginScanToolCatalogTest : public ::testing::Test {
protected:
    remidy_tooling::PluginScanTool scanner{};
    std::vector<remidy::PluginCatalogEntry*> entries{};
    std::filesystem::path cacheFile{};

    void SetUp() override {
        /*
        const auto* ciEnv = std::getenv("CI");
        if (ciEnv == nullptr || std::string{ciEnv}.empty()) {
            GTEST_SKIP() << "Audio plugin fixtures are not available outside CI.";
        }*/

        const auto* testInfo = ::testing::UnitTest::GetInstance()->current_test_info();
        cacheFile = createCacheFilePath(testInfo ? testInfo->name() : "plugin-scan");
        ASSERT_EQ(scanner.performPluginScanning(cacheFile), 0);
        entries = scanner.catalog.getPlugins();
    }

    void TearDown() override {
        if (!cacheFile.empty()) {
            std::error_code ec;
            std::filesystem::remove(cacheFile, ec);
        }
    }

    bool hasPluginMatching(std::initializer_list<const char*> candidates) const {
        for (const auto* rawName : candidates) {
            const auto needle = toLower(std::string{rawName});
            for (auto* entry : entries) {
                if (entry == nullptr)
                    continue;
                const auto haystack = toLower(entry->displayName());
                if (haystack.find(needle) != std::string::npos)
                    return true;
            }
        }
        return false;
    }
};

TEST_F(PluginScanToolCatalogTest, ScansSurgePlugin) {
    EXPECT_TRUE(hasPluginMatching({"surge"})) << "surge should be reported by the scanner";
}

TEST_F(PluginScanToolCatalogTest, ScansSixSinesPlugin) {
    EXPECT_TRUE(hasPluginMatching({"six-sines", "six sines"})) << "six-sines should be reported by the scanner";
}

} // namespace
