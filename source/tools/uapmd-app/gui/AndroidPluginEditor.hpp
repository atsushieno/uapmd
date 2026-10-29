#pragma once

#if defined(__ANDROID__)

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>
#include <jni.h>
#include <SDL3/SDL_system.h>
#include <GLES3/gl3.h>
#include <imgui.h>
#include <imgui_internal.h>

namespace uapmd_app_gui {

// ImGui draws editor chrome in its normal window order. A scissored alpha clear in
// that same draw order exposes the live Android surface, without copying its pixels.
class AndroidPluginEditor {
    jlong handle_;
    std::string title_;
    ImGuiWindow* window_{};
    std::array<jint, 6> frame_{};
    int content_width_{};
    int content_height_{};
    int size_request_revision_{-1};
    bool raise_requested_{true};

    static JNIEnv* env() {
        return static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    }

    static jclass manager(JNIEnv* jni) {
        static jclass cls{};
        if (!cls && jni) {
            auto local = jni->FindClass("dev/atsushieno/uapmd/ui/PluginUiOverlayManager");
            if (local) {
                cls = static_cast<jclass>(jni->NewGlobalRef(local));
                jni->DeleteLocalRef(local);
            }
        }
        return cls;
    }

    struct Aperture {
        GLint x, y, width, height;
    };

    static void clearAperture(const ImDrawList*, const ImDrawCmd* command) {
        const auto& area = *static_cast<const Aperture*>(command->UserCallbackData);
        GLint scissor[4];
        GLfloat clearColor[4];
        const auto scissorEnabled = glIsEnabled(GL_SCISSOR_TEST);
        glGetIntegerv(GL_SCISSOR_BOX, scissor);
        glGetFloatv(GL_COLOR_CLEAR_VALUE, clearColor);
        glEnable(GL_SCISSOR_TEST);
        glScissor(area.x, area.y, area.width, area.height);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        glClearColor(clearColor[0], clearColor[1], clearColor[2], clearColor[3]);
        glScissor(scissor[0], scissor[1], scissor[2], scissor[3]);
        if (!scissorEnabled)
            glDisable(GL_SCISSOR_TEST);
    }

public:
    AndroidPluginEditor(void* handle, std::string title)
        : handle_(reinterpret_cast<jlong>(handle)), title_(std::move(title)) {
        title_ += "###aap-editor-" + std::to_string(handle_);
    }

    void raise() { raise_requested_ = true; }

    bool render() {
        window_ = nullptr;
        auto* jni = env();
        auto cls = manager(jni);
        if (!cls)
            return true;
        static auto getInfo = jni->GetStaticMethodID(cls, "getOverlayInfo", "(J)[I");
        auto info = static_cast<jintArray>(jni->CallStaticObjectMethod(cls, getInfo, handle_));
        if (!info)
            return true;
        jint size[5]{};
        if (jni->GetArrayLength(info) >= 4)
            jni->GetIntArrayRegion(info, 0, std::min<jsize>(5, jni->GetArrayLength(info)), size);
        jni->DeleteLocalRef(info);
        if (size[0] <= 0 || size[1] <= 0)
            return true;

        const auto& io = ImGui::GetIO();
        const auto* viewport = ImGui::GetMainViewport();
        const float scaleX = std::max(0.01f, io.DisplayFramebufferScale.x);
        const float scaleY = std::max(0.01f, io.DisplayFramebufferScale.y);
        content_width_ = size[0];
        content_height_ = size[1];
        // Compose reports its layout size after a host relayout. Treat that as
        // content for scrolling, not as another request to grow the host window.
        if (size[4] != size_request_revision_) {
            size_request_revision_ = size[4];
            const auto& style = ImGui::GetStyle();
            const float requestedWidth = size[2] / scaleX;
            const float requestedHeight = size[3] / scaleY;
            const float verticalBar = size[1] > size[3] ? style.ScrollbarSize : 0.0f;
            const float horizontalBar = size[0] > size[2] ? style.ScrollbarSize : 0.0f;
            ImGui::SetNextWindowSize(ImVec2(
                std::min(requestedWidth + verticalBar, viewport->WorkSize.x * 0.85f),
                std::min(requestedHeight + ImGui::GetFrameHeight() + horizontalBar, viewport->WorkSize.y * 0.85f)));
        }
        if (raise_requested_) {
            ImGui::SetNextWindowFocus();
            raise_requested_ = false;
        }
        ImGui::SetNextWindowSizeConstraints(ImVec2(120, ImGui::GetFrameHeight() + 60), viewport->WorkSize);
        ImGui::SetNextWindowContentSize(ImVec2(content_width_ / scaleX, content_height_ / scaleY));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        bool open = true;
        const bool expanded = ImGui::Begin(title_.c_str(), &open,
            ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoSavedSettings);
        if (expanded && open) {
            window_ = ImGui::GetCurrentWindow();
            // Own the body so dragging to activate an editor never moves its window.
            ImGui::InvisibleButton("##native-editor", ImVec2(content_width_ / scaleX, content_height_ / scaleY));
            const auto& rect = window_->InnerRect;
            const int x = static_cast<int>(std::lround((rect.Min.x - viewport->Pos.x) * scaleX));
            const int y = static_cast<int>(std::lround((rect.Min.y - viewport->Pos.y) * scaleY));
            const int right = static_cast<int>(std::lround((rect.Max.x - viewport->Pos.x) * scaleX));
            const int bottom = static_cast<int>(std::lround((rect.Max.y - viewport->Pos.y) * scaleY));
            frame_ = {x, y, std::max(1, right - x), std::max(1, bottom - y),
                static_cast<int>(std::lround(ImGui::GetScrollX() * scaleX)),
                static_cast<int>(std::lround(ImGui::GetScrollY() * scaleY))};
            const auto clipped = window_->InnerClipRect;
            const int clipX = std::max(0, static_cast<int>(std::ceil((clipped.Min.x - viewport->Pos.x) * scaleX)));
            const int clipY = std::max(0, static_cast<int>(std::ceil((clipped.Min.y - viewport->Pos.y) * scaleY)));
            const int clipRight = std::min(static_cast<int>(io.DisplaySize.x * scaleX),
                static_cast<int>(std::floor((clipped.Max.x - viewport->Pos.x) * scaleX)));
            const int clipBottom = std::min(static_cast<int>(io.DisplaySize.y * scaleY),
                static_cast<int>(std::floor((clipped.Max.y - viewport->Pos.y) * scaleY)));
            Aperture area{clipX, static_cast<int>(io.DisplaySize.y * scaleY) - clipBottom,
                std::max(0, clipRight - clipX), std::max(0, clipBottom - clipY)};
            // Copy callback data into the draw list: closing/removing an editor later in
            // this frame must not leave a pointer to the editor in the renderer's queue.
            window_->DrawList->AddCallback(clearAperture, &area, sizeof(area));
            window_->DrawList->AddCallback(ImDrawCallback_ResetRenderState);
        }
        ImGui::End();
        ImGui::PopStyleVar();
        return open;
    }

    // Called after ImGui::Render(), once popup, child and focus ordering is final.
    static void sync(std::vector<AndroidPluginEditor*> editors) {
        auto* jni = env();
        auto cls = manager(jni);
        if (!cls)
            return;
        auto& context = *ImGui::GetCurrentContext();
        auto indexOf = [&](ImGuiWindow* window) {
            for (int index = 0; index < context.Windows.Size; ++index)
                if (context.Windows[index] == window)
                    return index;
            return -1;
        };
        std::erase_if(editors, [&](const auto* editor) {
            return !editor->window_ || !editor->window_->Active || editor->window_->Hidden ||
                editor->window_->LastFrameActive != context.FrameCount;
        });
        std::sort(editors.begin(), editors.end(), [&](const auto* left, const auto* right) {
            return indexOf(left->window_) < indexOf(right->window_);
        });
        std::vector<jlong> handles;
        std::vector<jint> frames;
        AndroidPluginEditor* active{};
        for (auto* editor : editors) {
            handles.push_back(editor->handle_);
            frames.insert(frames.end(), editor->frame_.begin(), editor->frame_.end());
            if (context.NavWindow && context.NavWindow->RootWindow == editor->window_ &&
                !ImGui::IsAnyMouseDown() && !ImGui::GetTopMostPopupModal())
                active = editor;
        }
        std::vector<jint> occluders;
        if (active) {
            const auto& io = ImGui::GetIO();
            const auto origin = ImGui::GetMainViewport()->Pos;
            for (int index = indexOf(active->window_) + 1; index < context.Windows.Size; ++index) {
                const auto* window = context.Windows[index];
                if (!window->Active || window->Hidden || window->RootWindow == active->window_)
                    continue;
                const auto& rect = window->OuterRectClipped;
                occluders.insert(occluders.end(), {
                    static_cast<jint>(std::floor((rect.Min.x - origin.x) * io.DisplayFramebufferScale.x)),
                    static_cast<jint>(std::floor((rect.Min.y - origin.y) * io.DisplayFramebufferScale.y)),
                    static_cast<jint>(std::ceil((rect.Max.x - origin.x) * io.DisplayFramebufferScale.x)),
                    static_cast<jint>(std::ceil((rect.Max.y - origin.y) * io.DisplayFramebufferScale.y))});
            }
            // Android rejects input through SDL even where its buffer is transparent.
            // A focused editor can receive input directly above SDL only while no host
            // window covers its content. Otherwise keep the composited ordering and let
            // ImGui handle activation/dismissal until the editor is unobscured again.
            const auto& frame = active->frame_;
            for (size_t offset = 0; offset + 4 <= occluders.size(); offset += 4)
                if (occluders[offset] < frame[0] + frame[2] && occluders[offset + 2] > frame[0] &&
                    occluders[offset + 1] < frame[1] + frame[3] && occluders[offset + 3] > frame[1]) {
                    active = nullptr;
                    break;
                }
        }
        auto jHandles = jni->NewLongArray(static_cast<jsize>(handles.size()));
        auto jFrames = jni->NewIntArray(static_cast<jsize>(frames.size()));
        auto jOccluders = jni->NewIntArray(static_cast<jsize>(occluders.size()));
        if (!handles.empty())
            jni->SetLongArrayRegion(jHandles, 0, static_cast<jsize>(handles.size()), handles.data());
        if (!frames.empty())
            jni->SetIntArrayRegion(jFrames, 0, static_cast<jsize>(frames.size()), frames.data());
        if (!occluders.empty())
            jni->SetIntArrayRegion(jOccluders, 0, static_cast<jsize>(occluders.size()), occluders.data());
        static auto syncFrames = jni->GetStaticMethodID(cls, "syncOverlayFrames", "([J[IJ[I)V");
        jni->CallStaticVoidMethod(cls, syncFrames, jHandles, jFrames, active ? active->handle_ : 0, jOccluders);
        jni->DeleteLocalRef(jHandles);
        jni->DeleteLocalRef(jFrames);
        jni->DeleteLocalRef(jOccluders);
    }
};

}

#endif
