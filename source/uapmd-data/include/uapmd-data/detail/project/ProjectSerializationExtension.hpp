#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace uapmd {

    class ProjectSerializationWriteContext {
    public:
        virtual ~ProjectSerializationWriteContext() = default;

        virtual std::filesystem::path projectFile() const = 0;
        virtual std::filesystem::path projectDirectory() const = 0;

        virtual bool writeExtensionFile(
            std::string_view extensionId,
            const std::filesystem::path& relativePath,
            const std::vector<uint8_t>& data,
            std::string& error) = 0;
    };

    class ProjectSerializationReadContext {
    public:
        virtual ~ProjectSerializationReadContext() = default;

        virtual std::filesystem::path projectFile() const = 0;
        virtual std::filesystem::path projectDirectory() const = 0;

        virtual std::optional<std::vector<uint8_t>> readExtensionFile(
            std::string_view extensionId,
            const std::filesystem::path& relativePath,
            std::string& error) = 0;
    };

    class ProjectSerializationExtension {
    public:
        virtual ~ProjectSerializationExtension() = default;

        virtual std::string_view extensionId() const = 0;

        virtual bool saveProjectExtensionData(
            ProjectSerializationWriteContext& context,
            std::string& error) = 0;

        virtual bool loadProjectExtensionData(
            ProjectSerializationReadContext& context,
            std::string& error) = 0;
    };

} // namespace uapmd
