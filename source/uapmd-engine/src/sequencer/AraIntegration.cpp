#include "uapmd-engine/uapmd-engine.hpp"

#include <map>

namespace uapmd {

    namespace {
        class AraSessionImpl final : public AraSession {
            struct AttachedPlugin {
                std::unique_ptr<AraPluginDocument> document{};
            };

            ProjectDocumentView* document_view_{nullptr};
            ProjectDocumentEventSource* event_source_{nullptr};
            ProjectDocumentEventListenerToken listener_token_{0};
            std::map<int32_t, AttachedPlugin> plugins_{};

            bool bound() const {
                return document_view_ && event_source_;
            }

            void resyncAfterEvent(const ProjectDocumentEvent& event) {
                (void) event;
                resyncFromProjectDocument();
            }

        public:
            ~AraSessionImpl() override {
                unbindProjectDocument();
            }

            AraStatus bindProjectDocument(
                ProjectDocumentView& documentView,
                ProjectDocumentEventSource& eventSource) override {
                if (event_source_ == &eventSource && document_view_ == &documentView)
                    return AraStatus::Ok;

                unbindProjectDocument();

                document_view_ = &documentView;
                event_source_ = &eventSource;
                listener_token_ = event_source_->addProjectDocumentEventListener(*this);

                for (auto& [_, plugin] : plugins_) {
                    if (!plugin.document)
                        continue;
                    auto status = plugin.document->bindProjectDocument(documentView, eventSource);
                    if (status != AraStatus::Ok)
                        return status;
                }

                return resyncFromProjectDocument();
            }

            void unbindProjectDocument() override {
                for (auto& [_, plugin] : plugins_) {
                    if (plugin.document)
                        plugin.document->unbindProjectDocument();
                }

                if (event_source_ && listener_token_ != 0)
                    event_source_->removeProjectDocumentEventListener(listener_token_);

                listener_token_ = 0;
                event_source_ = nullptr;
                document_view_ = nullptr;
            }

            AraStatus attachPlugin(
                int32_t pluginInstanceId,
                AudioPluginInstanceAPI& pluginInstance) override {
                auto* extension = getAraPluginExtension(pluginInstance);
                if (!extension || !extension->supportsAraDocument())
                    return AraStatus::UnsupportedPlugin;

                auto document = extension->createAraPluginDocument();
                if (!document)
                    return AraStatus::BackendError;

                if (bound()) {
                    auto status = document->bindProjectDocument(*document_view_, *event_source_);
                    if (status != AraStatus::Ok)
                        return status;
                }

                plugins_[pluginInstanceId] = AttachedPlugin{std::move(document)};
                if (bound())
                    return plugins_[pluginInstanceId].document->resyncFromProjectDocument();
                return AraStatus::Ok;
            }

            void detachPlugin(int32_t pluginInstanceId) override {
                auto it = plugins_.find(pluginInstanceId);
                if (it == plugins_.end())
                    return;
                if (it->second.document)
                    it->second.document->unbindProjectDocument();
                plugins_.erase(it);
            }

            AraPluginDocument* pluginDocument(int32_t pluginInstanceId) override {
                auto it = plugins_.find(pluginInstanceId);
                if (it == plugins_.end())
                    return nullptr;
                return it->second.document.get();
            }

            std::vector<int32_t> attachedPluginInstanceIds() const override {
                std::vector<int32_t> result;
                result.reserve(plugins_.size());
                for (const auto& [instanceId, _] : plugins_)
                    result.push_back(instanceId);
                return result;
            }

            AraStatus resyncFromProjectDocument() override {
                if (!bound())
                    return AraStatus::InvalidDocument;

                for (auto& [_, plugin] : plugins_) {
                    if (!plugin.document)
                        continue;
                    auto status = plugin.document->resyncFromProjectDocument();
                    if (status != AraStatus::Ok)
                        return status;
                }
                return AraStatus::Ok;
            }

            void projectLoaded(const ProjectDocumentEvent& event) override { resyncAfterEvent(event); }
            void projectClosing(const ProjectDocumentEvent& event) override { resyncAfterEvent(event); }
            void projectSaved(const ProjectDocumentEvent& event) override { resyncAfterEvent(event); }
            void masterTrackChanged(const ProjectDocumentEvent& event) override { resyncAfterEvent(event); }
            void trackAdded(const ProjectDocumentEvent& event) override { resyncAfterEvent(event); }
            void trackRemoved(const ProjectDocumentEvent& event) override { resyncAfterEvent(event); }
            void trackChanged(const ProjectDocumentEvent& event) override { resyncAfterEvent(event); }
            void clipAdded(const ProjectDocumentEvent& event) override { resyncAfterEvent(event); }
            void clipRemoved(const ProjectDocumentEvent& event) override { resyncAfterEvent(event); }
            void clipChanged(const ProjectDocumentEvent& event) override { resyncAfterEvent(event); }
            void audioSourceAdded(const ProjectDocumentEvent& event) override { resyncAfterEvent(event); }
            void audioSourceRemoved(const ProjectDocumentEvent& event) override { resyncAfterEvent(event); }
            void audioSourceChanged(const ProjectDocumentEvent& event) override { resyncAfterEvent(event); }
            void pluginGraphChanged(const ProjectDocumentEvent& event) override { resyncAfterEvent(event); }
        };
    } // namespace

    std::unique_ptr<AraSession> AraSession::create() {
        return std::make_unique<AraSessionImpl>();
    }

} // namespace uapmd
