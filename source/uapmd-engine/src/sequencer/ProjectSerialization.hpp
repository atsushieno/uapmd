#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "uapmd-engine/uapmd-engine.hpp"

namespace uapmd::sequencer_detail {

    std::filesystem::path makeRelativePath(
        const std::filesystem::path& baseDir,
        const std::filesystem::path& target);

    std::vector<ClipData> sortedTrackClips(TimelineTrack& timelineTrack);

    std::string writePluginStateBlob(
        const std::filesystem::path& projectDir,
        const std::filesystem::path& pluginStateDir,
        const std::string& scopeLabel,
        size_t pluginOrder,
        int32_t instanceId,
        const std::vector<uint8_t>& stateData,
        std::string& error);

    bool serializeProjectClip(
        TimelineTrack& timelineTrack,
        const ClipData& clip,
        UapmdProjectTrackData& projectTrack,
        std::unordered_map<std::string, UapmdProjectClipData*>& serializedClipLookup,
        const std::filesystem::path& clipDir,
        const std::filesystem::path& projectDir,
        const std::string& midiExportNamePrefix,
        const std::string& audioCopyNamePrefix,
        const std::string& clipContextLabel,
        bool includeTimelineMeta,
        size_t& midiExportCounter,
        std::string& error);

    class FilesystemProjectSerializationWriteContext final : public ProjectSerializationWriteContext {
        std::filesystem::path project_file_;
        std::filesystem::path project_dir_;

    public:
        FilesystemProjectSerializationWriteContext(std::filesystem::path projectFile, std::filesystem::path projectDir);

        std::filesystem::path projectFile() const override;
        std::filesystem::path projectDirectory() const override;

        bool writeExtensionFile(
            std::string_view extensionId,
            const std::filesystem::path& relativePath,
            const std::vector<uint8_t>& data,
            std::string& error) override;
    };

    class FilesystemProjectSerializationReadContext final : public ProjectSerializationReadContext {
        std::filesystem::path project_file_;
        std::filesystem::path project_dir_;

    public:
        FilesystemProjectSerializationReadContext(std::filesystem::path projectFile, std::filesystem::path projectDir);

        std::filesystem::path projectFile() const override;
        std::filesystem::path projectDirectory() const override;

        std::optional<std::vector<uint8_t>> readExtensionFile(
            std::string_view extensionId,
            const std::filesystem::path& relativePath,
            std::string& error) override;
    };

} // namespace uapmd::sequencer_detail
