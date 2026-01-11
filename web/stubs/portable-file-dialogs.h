// Minimal stub of portable-file-dialogs for Web build
#pragma once
#include <string>
#include <vector>

namespace pfd {
enum class choice { ok, yes_no };
enum class icon { info, warning, error };

class message {
public:
    message(const std::string&, const std::string&, choice = choice::ok, icon = icon::info) {}
};

class save_file {
    std::string result_;
public:
    template<typename... Args>
    save_file(Args...) {}
    std::string result() const { return result_; }
};

class open_file {
    std::vector<std::string> results_;
public:
    template<typename... Args>
    open_file(Args...) {}
    std::vector<std::string> result() const { return results_; }
};
}

