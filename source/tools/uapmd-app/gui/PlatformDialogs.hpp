#pragma once

#include <string>
#include <iostream>

#if !defined(__ANDROID__) && !defined(__EMSCRIPTEN__)
#include <portable-file-dialogs.h>
#endif

namespace uapmd::gui {

enum class PlatformDialogIcon {
    Info,
    Warning,
    Error
};

inline void showPlatformDialog(const std::string& title,
                               const std::string& message,
                               PlatformDialogIcon icon) {
#if !defined(__ANDROID__) && !defined(__EMSCRIPTEN__)
    pfd::icon dialogIcon = pfd::icon::info;
    switch (icon) {
        case PlatformDialogIcon::Warning: dialogIcon = pfd::icon::warning; break;
        case PlatformDialogIcon::Error: dialogIcon = pfd::icon::error; break;
        case PlatformDialogIcon::Info: default: break;
    }
    pfd::message(title, message, pfd::choice::ok, dialogIcon);
#else
    std::ostream& stream = (icon == PlatformDialogIcon::Error)
        ? std::cerr : std::cout;
    const char* level = (icon == PlatformDialogIcon::Error) ? "ERROR"
                        : (icon == PlatformDialogIcon::Warning) ? "WARNING"
                        : "INFO";
    stream << '[' << level << "] " << title << ": " << message << '\n';
#endif
}

inline void platformInfo(const std::string& title, const std::string& message) {
    showPlatformDialog(title, message, PlatformDialogIcon::Info);
}

inline void platformWarning(const std::string& title, const std::string& message) {
    showPlatformDialog(title, message, PlatformDialogIcon::Warning);
}

inline void platformError(const std::string& title, const std::string& message) {
    showPlatformDialog(title, message, PlatformDialogIcon::Error);
}

} // namespace uapmd::gui
