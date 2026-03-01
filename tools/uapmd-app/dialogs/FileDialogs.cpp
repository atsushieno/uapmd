#include "FileDialogs.hpp"

#include <cstdlib>
#include <sstream>

#ifndef __EMSCRIPTEN__
#include <portable-file-dialogs.h>
#else
#include <emscripten.h>
#endif

namespace uapmd::dialog {

namespace {

std::vector<std::string> flattenFilters(const std::vector<FileFilter>& filters) {
    std::vector<std::string> flat;
    flat.reserve(filters.size() * 2);
    for (const auto& filter : filters) {
        flat.push_back(filter.label);
        flat.push_back(filter.pattern);
    }
    return flat;
}

#ifdef __EMSCRIPTEN__

std::string buildAcceptList(const std::vector<FileFilter>& filters) {
    std::ostringstream oss;
    bool first = true;
    for (const auto& filter : filters) {
        std::istringstream iss(filter.pattern);
        std::string token;
        while (iss >> token) {
            if (token == "*")
                continue;
            if (token.starts_with("*."))
                token.erase(0, 1);
            if (token.empty())
                continue;
            if (!first)
                oss << ',';
            first = false;
            oss << token;
        }
    }
    return oss.str();
}

EM_JS(int, uapmd_web_open_file_dialog, (int allowMultiple, const char* acceptPtr), {
  const accept = UTF8ToString(acceptPtr);
  return Asyncify.handleAsync(async () => {
    const input = document.createElement("input");
    input.type = "file";
    input.style.display = "none";
    if (allowMultiple) input.multiple = true;
    if (accept.length > 0) input.accept = accept;
    document.body.appendChild(input);
    const files = await new Promise((resolve) => {
        let settled = false;
        const cleanup = () => {
            input.removeEventListener("change", onChange);
            input.removeEventListener("cancel", onCancel);
            window.removeEventListener("focus", onWindowFocus, true);
        };
        const resolveOnce = (value) => {
            if (settled) return;
            settled = true;
            cleanup();
            resolve(value);
        };
        const onChange = () => resolveOnce(input.files);
        const onCancel = () => resolveOnce(null);
        const onWindowFocus = () => {
            setTimeout(() => {
                if (!settled && (!input.files || input.files.length === 0)) {
                    resolveOnce(null);
                }
            }, 0);
        };
        input.addEventListener("change", onChange, { once: true });
        input.addEventListener("cancel", onCancel, { once: true });
        window.addEventListener("focus", onWindowFocus, { once: true, capture: true });
        input.click();
    });
    document.body.removeChild(input);
    if (!files || files.length === 0) {
        return 0;
    }
    const ensureDir = (path) => {
        const parts = path.split("/").filter(Boolean);
        let current = "";
        for (const part of parts) {
            current += "/" + part;
            try { FS.mkdir(current); } catch (e) {}
        }
    };
    ensureDir("/browser/uploads");
    const paths = [];
    for (let i = 0; i < files.length; ++i) {
        const file = files[i];
        const buffer = new Uint8Array(await file.arrayBuffer());
        const safeName = file.name.replace(/[^A-Za-z0-9._-]/g, "_");
        const target = `/browser/uploads/${Date.now()}_${i}_${safeName}`;
        FS.writeFile(target, buffer);
        paths.push(target);
    }
    const joined = paths.join('\n');
    const len = lengthBytesUTF8(joined) + 1;
    const ptr = _malloc(len);
    stringToUTF8(joined, ptr, len);
    return ptr;
  });
});

EM_JS(int, uapmd_web_save_file_dialog, (const char* suggestedPtr, const char* acceptPtr), {
  const suggested = UTF8ToString(suggestedPtr);
  const accept = UTF8ToString(acceptPtr);
  return Asyncify.handleAsync(async () => {
    let defaultName = suggested && suggested.length ? suggested : "uapmd-output.dat";
    const promptText = "Enter file name to save:";
    const response = window.prompt(promptText, defaultName);
    if (!response) {
        return 0;
    }
    const safeName = response.replace(/[^A-Za-z0-9._-]/g, "_");
    const baseDir = "/browser/downloads";
    try { FS.mkdir(baseDir); } catch (e) {}
    const path = `${baseDir}/${Date.now()}_${safeName}`;
    const payload = `${path}\n${safeName}`;
    const len = lengthBytesUTF8(payload) + 1;
    const ptr = _malloc(len);
    stringToUTF8(payload, ptr, len);
    return ptr;
  });
});

EM_JS(void, uapmd_web_finalize_download, (const char* pathPtr, const char* namePtr), {
  const path = UTF8ToString(pathPtr);
  const name = UTF8ToString(namePtr);
  let data;
  try {
    data = FS.readFile(path);
  } catch (err) {
    console.error("Failed to read file for download", err);
    return;
  }
  const blob = new Blob([data], { type: "application/octet-stream" });
  const url = URL.createObjectURL(blob);
  const link = document.createElement("a");
  link.href = url;
  link.download = name || "download.dat";
  document.body.appendChild(link);
  link.click();
  document.body.removeChild(link);
  setTimeout(() => URL.revokeObjectURL(url), 1000);
});

#endif // __EMSCRIPTEN__

} // namespace

std::vector<std::filesystem::path> openFile(std::string_view title,
                                            std::string_view defaultPath,
                                            const std::vector<FileFilter>& filters,
                                            bool allowMultiple) {
#ifdef __EMSCRIPTEN__
    const auto accept = buildAcceptList(filters);
    auto ptr = uapmd_web_open_file_dialog(allowMultiple ? 1 : 0, accept.c_str());
    if (ptr == 0)
        return {};
    std::string joined = reinterpret_cast<char*>(ptr);
    free(reinterpret_cast<void*>(ptr));
    std::vector<std::filesystem::path> paths;
    std::istringstream iss(joined);
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty())
            paths.emplace_back(line);
    }
    return paths;
#else
    auto flat = flattenFilters(filters);
    auto result = pfd::open_file(std::string(title),
                                 std::string(defaultPath),
                                 flat,
                                 allowMultiple ? pfd::opt::multiselect : pfd::opt::none).result();
    std::vector<std::filesystem::path> paths;
    for (auto& entry : result)
        paths.emplace_back(entry);
    return paths;
#endif
}

SaveResult saveFile(std::string_view title,
                    std::string_view defaultName,
                    const std::vector<FileFilter>& filters) {
#ifdef __EMSCRIPTEN__
    const auto accept = buildAcceptList(filters);
    std::string defaultNameStr(defaultName);
    auto ptr = uapmd_web_save_file_dialog(defaultNameStr.c_str(), accept.c_str());
    if (ptr == 0)
        return {};
    std::string payload = reinterpret_cast<char*>(ptr);
    free(reinterpret_cast<void*>(ptr));
    const auto newline = payload.find('\n');
    if (newline == std::string::npos)
        return {};
    SaveResult result;
    std::string downloadName = payload.substr(newline + 1);
    result.path = payload.substr(0, newline);
    result.finalize = [path = result.path, downloadName]() {
        uapmd_web_finalize_download(path.string().c_str(), downloadName.c_str());
    };
    return result;
#else
    auto flat = flattenFilters(filters);
    auto value = pfd::save_file(std::string(title),
                                std::string(defaultName),
                                flat).result();
    SaveResult result;
    result.path = value;
    return result;
#endif
}

void showMessage(std::string_view title,
                 std::string_view message,
                 MessageIcon icon) {
#ifdef __EMSCRIPTEN__
    const char* level = nullptr;
    switch (icon) {
        case MessageIcon::Warning: level = "warn"; break;
        case MessageIcon::Error: level = "error"; break;
        default: level = "info"; break;
    }
    EM_ASM({
        const level = UTF8ToString($0);
        const title = UTF8ToString($1);
        const msg = UTF8ToString($2);
        const text = `${title}\n\n${msg}`;
        if (level === 'error')
            console.error(text);
        else if (level === 'warn')
            console.warn(text);
        else
            console.log(text);
        alert(text);
    }, level, title.data(), message.data());
#else
    pfd::icon iconType = pfd::icon::info;
    switch (icon) {
        case MessageIcon::Warning: iconType = pfd::icon::warning; break;
        case MessageIcon::Error: iconType = pfd::icon::error; break;
        default: break;
    }
    pfd::message(std::string(title),
                 std::string(message),
                 pfd::choice::ok,
                 iconType);
#endif
}

} // namespace uapmd::dialog
