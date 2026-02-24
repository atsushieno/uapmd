#pragma once

#include <filesystem>
#include <functional>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace uapmd::dialog {

struct FileFilter {
    std::string label;
    std::string pattern;
};

inline std::vector<FileFilter> makeFilters(const std::initializer_list<const char*>& items) {
    std::vector<FileFilter> filters;
    filters.reserve(items.size() / 2);
    auto it = items.begin();
    while (it != items.end()) {
        const char* label = *it++;
        if (it == items.end())
            break;
        const char* pattern = *it++;
        filters.push_back({label ? label : "", pattern ? pattern : ""});
    }
    return filters;
}

enum class MessageIcon {
    Info,
    Warning,
    Error
};

struct SaveResult {
    std::filesystem::path path{};
    std::function<void()> finalize{};

    explicit operator bool() const { return !path.empty(); }
    const std::filesystem::path& filepath() const { return path; }
    void complete() const {
        if (finalize)
            finalize();
    }
};

std::vector<std::filesystem::path> openFile(std::string_view title,
                                            std::string_view defaultPath,
                                            const std::vector<FileFilter>& filters,
                                            bool allowMultiple = false);

SaveResult saveFile(std::string_view title,
                    std::string_view defaultName,
                    const std::vector<FileFilter>& filters);

void showMessage(std::string_view title,
                 std::string_view message,
                 MessageIcon icon);

} // namespace uapmd::dialog
