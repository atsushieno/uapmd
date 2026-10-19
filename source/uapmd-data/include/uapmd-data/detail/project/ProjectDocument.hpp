#pragma once

#include <memory>
#include "ProjectDocumentView.hpp"
#include "ProjectSelection.hpp"

namespace uapmd {
    class ProjectAddressBook;
    class ProjectCommands;
    class ProjectCommandManager;
    class MidiClipDocument;

    // Document contracts are independent of live sequencer tracks and plugins.
    class ProjectDocument {
    public:
        virtual ~ProjectDocument() = default;
        virtual const ProjectDocumentView& view() const = 0;
        virtual ProjectAddressBook& addresses() = 0;
        virtual const ProjectAddressBook& addresses() const = 0;
        virtual ProjectDocumentEventSource& events() = 0;
        virtual ProjectSelectionService& selections() = 0;
        virtual const ProjectSelectionService& selections() const = 0;
        virtual ProjectCommands& commands() = 0;
        virtual ProjectCommandManager& commandManager() = 0;
        virtual std::shared_ptr<MidiClipDocument> midiContent(ProjectObjectId clipId) = 0;
        virtual void beginTransaction() = 0;
        virtual void endTransaction() = 0;
    };

    class ScopedProjectDocumentTransaction {
        ProjectDocument& document_;
    public:
        explicit ScopedProjectDocumentTransaction(ProjectDocument& document) : document_(document) {
            document_.beginTransaction();
        }
        ~ScopedProjectDocumentTransaction() { document_.endTransaction(); }
        ScopedProjectDocumentTransaction(const ScopedProjectDocumentTransaction&) = delete;
        ScopedProjectDocumentTransaction& operator=(const ScopedProjectDocumentTransaction&) = delete;
    };
}
