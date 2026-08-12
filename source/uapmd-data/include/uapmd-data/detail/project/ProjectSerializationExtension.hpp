#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ProjectDocumentEvents.hpp"

namespace uapmd {

    class UapmdProjectData;

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

        // These hooks own feature-specific data embedded in the main project
        // document. They are invoked before the document is written and after
        // it is read, respectively.
        virtual bool saveProjectData(
            UapmdProjectData&,
            std::string&) { return true; }

        virtual bool loadProjectData(
            UapmdProjectData&,
            std::string&) { return true; }

        virtual bool saveProjectExtensionData(
            ProjectSerializationWriteContext& context,
            std::string& error) { return true; }

        virtual bool loadProjectExtensionData(
            ProjectSerializationReadContext& context,
            std::string& error) { return true; }

        // Fragment hooks.
        //
        // A fragment is a detached copy of a single document object -- the
        // payload behind undo and the clipboard -- and carries one slot of
        // opaque state per extension, keyed by extensionId(). An extension that
        // owns state belonging to a clip contributes it here, so that the state
        // travels with the object instead of being lost when the object is
        // removed and later restored, or copied.
        //
        // These exist because the document layer cannot reach the code that
        // owns such state: capture and attach live in the sequencer engine,
        // while the owners are registered from outside it.
        //
        // An extension with nothing to contribute for this object leaves
        // `state` empty and returns true. Returning false means a real failure
        // and aborts the capture.
        virtual bool captureClipFragmentState(
            const ProjectObjectId& clipId,
            std::vector<uint8_t>& state,
            std::string& error) {
            (void) clipId;
            (void) state;
            (void) error;
            return true;
        }

        // `clipId` is the identity of the clip as it now exists, which is the
        // captured one when a fragment is restored and a freshly minted one
        // when it is pasted. `state` is whatever this extension contributed to
        // the fragment, and is empty when it contributed nothing.
        virtual bool restoreClipFragmentState(
            const ProjectObjectId& clipId,
            const std::vector<uint8_t>& state,
            std::string& error) {
            (void) clipId;
            (void) state;
            (void) error;
            return true;
        }

        // The same pair for a whole track. A track fragment carries its clips'
        // fragments, so an extension owning per-clip state contributes it
        // through the clip hooks above and needs these only for state that
        // belongs to the track itself.
        virtual bool captureTrackFragmentState(
            const ProjectObjectId& trackId,
            std::vector<uint8_t>& state,
            std::string& error) {
            (void) trackId;
            (void) state;
            (void) error;
            return true;
        }

        virtual bool restoreTrackFragmentState(
            const ProjectObjectId& trackId,
            const std::vector<uint8_t>& state,
            std::string& error) {
            (void) trackId;
            (void) state;
            (void) error;
            return true;
        }
    };

} // namespace uapmd
