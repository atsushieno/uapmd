#include "uapmd-format-jsfx/uapmd-format-jsfx.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <memory>
#include <sstream>
#include <system_error>

#include "choc/containers/choc_ZipFile.h"
#include "choc/text/choc_JSON.h"

namespace uapmd_jsfx {

    namespace {
        constexpr const char* kUserContentFolderName = "jsfx";
        constexpr const char* kFolderMirrorRootName = "jsfx-folders";
        constexpr const char* kFolderRegistryFileName = "jsfx-folders.json";

        // Rejects anything that would land outside the destination: absolute paths, drive
        // letters, and `..` segments. A zip is untrusted input, and an archive that writes
        // to `../../` is a real thing rather than a hypothetical one.
        bool isSafeRelativePath(const std::filesystem::path& relative) {
            if (relative.empty() || relative.is_absolute())
                return false;
            if (relative.has_root_name())
                return false;
            for (const auto& part : relative)
                if (part == "..")
                    return false;
            return true;
        }

        std::string lowercaseExtension(const std::filesystem::path& path) {
            auto ext = path.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return ext;
        }
    }

    std::filesystem::path jsfxUserContentDirectory() {
        const auto& root = uapmd_plugin_hosting::applicationDataDirectory();
        if (root.empty())
            return {};
        auto directory = root / kUserContentFolderName;
        std::error_code ec{};
        std::filesystem::create_directories(directory, ec);
        if (ec)
            return {};
        return directory;
    }

    std::filesystem::path jsfxFolderMirrorRoot() {
        const auto& root = uapmd_plugin_hosting::applicationDataDirectory();
        if (root.empty())
            return {};
        auto directory = root / kFolderMirrorRootName;
        std::error_code ec{};
        std::filesystem::create_directories(directory, ec);
        if (ec)
            return {};
        return directory;
    }

    namespace {
        std::filesystem::path folderRegistryFile() {
            const auto& root = uapmd_plugin_hosting::applicationDataDirectory();
            if (root.empty())
                return {};
            return root / kFolderRegistryFileName;
        }

        // A folder's name is also a directory name, so it has to survive being one.
        std::string sanitizeFolderName(const std::string& name) {
            std::string out;
            out.reserve(name.size());
            for (unsigned char c : name) {
                if (c == '/' || c == '\\' || c == ':' || c < 0x20)
                    out += '_';
                else
                    out += static_cast<char>(c);
            }
            while (!out.empty() && (out.front() == '.' || out.front() == ' '))
                out.erase(out.begin());
            while (!out.empty() && (out.back() == '.' || out.back() == ' '))
                out.pop_back();
            return out;
        }
    }

    std::vector<JsfxRegisteredFolder> jsfxRegisteredFolders() {
        std::vector<JsfxRegisteredFolder> folders{};
        const auto file = folderRegistryFile();
        if (file.empty())
            return folders;
        std::error_code ec{};
        if (!std::filesystem::exists(file, ec))
            return folders;
        try {
            std::ifstream stream{file};
            std::string text{std::istreambuf_iterator<char>(stream),
                             std::istreambuf_iterator<char>()};
            auto parsed = choc::json::parse(text);
            auto view = parsed.getView();
            if (!view.isArray())
                return folders;
            for (auto item : view) {
                auto token = item["token"];
                auto name = item["name"];
                if (token.isVoid() || name.isVoid())
                    continue;
                JsfxRegisteredFolder folder{token.toString(), name.toString()};
                if (!folder.token.empty() && !folder.name.empty())
                    folders.emplace_back(std::move(folder));
            }
        } catch (...) {
            // A registry we cannot read is a registry with nothing in it. The user
            // re-picks their folders; nothing else is lost.
            folders.clear();
        }
        return folders;
    }

    namespace {
        void saveRegisteredFolders(const std::vector<JsfxRegisteredFolder>& folders) {
            const auto file = folderRegistryFile();
            if (file.empty())
                return;
            auto array = choc::value::createEmptyArray();
            for (auto& folder : folders) {
                auto item = choc::value::createObject("folder");
                item.setMember("token", folder.token);
                item.setMember("name", folder.name);
                array.addArrayElement(item);
            }
            std::ofstream stream{file, std::ios::trunc};
            if (stream)
                stream << choc::json::toString(array, true);
        }
    }

    std::string registerJsfxFolder(const std::string& token, const std::string& name) {
        if (token.empty())
            return {};

        auto folders = jsfxRegisteredFolders();

        auto desired = sanitizeFolderName(name);
        if (desired.empty())
            desired = "Folder";

        // Re-registering a folder keeps the mirror it already has.
        for (auto& folder : folders)
            if (folder.token == token)
                return folder.name;

        // Two folders can easily be called the same thing -- two "Effects" folders in
        // different places -- and they cannot share a mirror.
        std::string chosen = desired;
        for (int suffix = 2; suffix < 1000; suffix++) {
            bool taken = false;
            for (auto& folder : folders)
                if (folder.name == chosen) {
                    taken = true;
                    break;
                }
            if (!taken)
                break;
            chosen = desired + " (" + std::to_string(suffix) + ")";
        }

        folders.push_back({token, chosen});
        saveRegisteredFolders(folders);
        return chosen;
    }

    void unregisterJsfxFolder(const std::string& token) {
        auto folders = jsfxRegisteredFolders();
        std::string name;
        for (auto& folder : folders)
            if (folder.token == token)
                name = folder.name;
        folders.erase(std::remove_if(folders.begin(), folders.end(),
                                     [&token](const JsfxRegisteredFolder& folder) {
                                         return folder.token == token;
                                     }),
                      folders.end());
        saveRegisteredFolders(folders);

        if (name.empty())
            return;
        const auto root = jsfxFolderMirrorRoot();
        if (root.empty())
            return;
        std::filesystem::path relative{name};
        if (!isSafeRelativePath(relative))
            return;
        std::error_code ec{};
        std::filesystem::remove_all(root / relative, ec);
    }

    bool clearJsfxFolderMirror(const std::string& name) {
        const auto root = jsfxFolderMirrorRoot();
        if (root.empty())
            return false;
        std::filesystem::path relative{name};
        if (!isSafeRelativePath(relative))
            return false;
        std::error_code ec{};
        std::filesystem::remove_all(root / relative, ec);
        if (ec)
            return false;
        std::filesystem::create_directories(root / relative, ec);
        return !ec;
    }

    JsfxImportResult writeJsfxFolderMirrorFile(const std::string& name,
                                               const std::string& relativePath,
                                               const uint8_t* data,
                                               size_t size) {
        JsfxImportResult result{};

        const auto root = jsfxFolderMirrorRoot();
        if (root.empty()) {
            result.error = "there is nowhere writable to mirror folders on this platform";
            return result;
        }

        std::filesystem::path folderName{name};
        std::filesystem::path relative{relativePath};
        if (!isSafeRelativePath(folderName) || !isSafeRelativePath(relative)) {
            result.error = "that file would be written outside the mirror: " + relativePath;
            return result;
        }

        const auto target = root / folderName / relative;
        std::error_code ec{};
        std::filesystem::create_directories(target.parent_path(), ec);
        if (ec) {
            result.error = "could not create " + target.parent_path().string();
            return result;
        }

        std::ofstream out{target, std::ios::binary | std::ios::trunc};
        if (!out) {
            result.error = "could not write " + target.string();
            return result;
        }
        out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
        out.close();
        if (!out) {
            result.error = "could not finish writing " + target.string();
            return result;
        }

        result.success = true;
        result.filesWritten = 1;
        result.destination = target.parent_path();
        return result;
    }

    bool looksLikeJsfxArchive(const std::string& fileName) {
        const auto ext = lowercaseExtension(std::filesystem::path{fileName});
        return ext == ".zip";
    }

    JsfxImportResult importJsfxContent(const std::string& fileName,
                                       const uint8_t* data,
                                       size_t size,
                                       const std::string& subdirectory) {
        JsfxImportResult result{};

        const auto root = jsfxUserContentDirectory();
        if (root.empty()) {
            result.error = "there is nowhere writable to keep imported effects on this platform";
            return result;
        }

        auto destination = root;
        if (!subdirectory.empty()) {
            std::filesystem::path relative{subdirectory};
            if (!isSafeRelativePath(relative)) {
                result.error = "the destination folder name is not usable";
                return result;
            }
            destination /= relative;
        }

        std::error_code ec{};
        std::filesystem::create_directories(destination, ec);
        if (ec) {
            result.error = "could not create " + destination.string();
            return result;
        }

        if (!data || size == 0) {
            result.error = "the file is empty";
            return result;
        }

        if (!looksLikeJsfxArchive(fileName)) {
            // A single file: keep the name it came with, and refuse a name that is really
            // a path, which is how a picked document can arrive on some platforms.
            std::filesystem::path leaf{fileName};
            leaf = leaf.filename();
            if (leaf.empty()) {
                result.error = "the file has no usable name";
                return result;
            }
            auto target = destination / leaf;
            std::ofstream out{target, std::ios::binary | std::ios::trunc};
            if (!out) {
                result.error = "could not write " + target.string();
                return result;
            }
            out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
            out.close();
            if (!out) {
                result.error = "could not finish writing " + target.string();
                return result;
            }
            result.success = true;
            result.filesWritten = 1;
            result.destination = destination;
            return result;
        }

        try {
            auto stream = std::make_shared<std::istringstream>(
                    std::string{reinterpret_cast<const char*>(data), size},
                    std::ios::binary);
            choc::zip::ZipFile archive{stream};

            auto isDirectoryEntry = [](const std::string& name) {
                // Directory entries carry a trailing separator and no content.
                return name.empty() || name.back() == '/' || name.back() == '\\';
            };

            // Every name is checked before anything is written. Refusing an entry
            // half-way through would leave whatever came before it on disk, which is
            // exactly what an archive built to escape the destination would rely on.
            for (auto& item : archive.items) {
                if (isDirectoryEntry(item.filename))
                    continue;
                if (!isSafeRelativePath(std::filesystem::path{item.filename})) {
                    result.error = "the archive contains an entry that would be written "
                                   "outside the destination: " + item.filename;
                    return result;
                }
            }

            uint32_t written = 0;
            for (auto& item : archive.items) {
                if (isDirectoryEntry(item.filename))
                    continue;
                if (!item.uncompressToFile(destination, true, false)) {
                    result.error = "could not extract " + item.filename;
                    return result;
                }
                written++;
            }

            if (written == 0) {
                result.error = "the archive holds no files";
                return result;
            }
            result.success = true;
            result.filesWritten = written;
            result.destination = destination;
            return result;
        } catch (const std::exception& error) {
            result.error = std::string{"the archive could not be read: "} + error.what();
            return result;
        } catch (...) {
            result.error = "the archive could not be read";
            return result;
        }
    }

}
