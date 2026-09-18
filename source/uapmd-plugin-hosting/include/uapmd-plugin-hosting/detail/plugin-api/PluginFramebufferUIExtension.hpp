#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>
#include "AudioPluginInstanceAPI.hpp"

// A plugin editor that is a pixel buffer rather than a native window.
//
// The UI members of AudioPluginInstanceAPI describe a native view: the host passes a parent
// window handle and the plugin puts a view inside it. Some formats have no view to give.
// JSFX draws into a buffer the host supplies, and the host is then responsible for putting
// those pixels on screen and for sending input back.
//
// A format whose editor works that way registers this extension. The host draws the
// framebuffer wherever it likes -- for an immediate mode GUI that means uploading it as a
// texture each time the frame serial changes -- and reports input through deliverInput().
//
// hasUISupport() still reports whether there is an editor at all. A format that registers
// this extension reports true there and leaves createUI()/showUI() to the base class
// defaults, because there is no native view to create.
namespace uapmd_plugin_hosting {
    inline constexpr std::string_view kPluginFramebufferUIExtensionId =
        "dev.atsushieno.uapmd.plugin-instance.framebuffer-ui.v1";

    // A view of the most recently rendered frame. It is only valid for the duration of the
    // PluginFramebufferUI::readFrame() callback that produced it.
    struct FramebufferView {
        const uint8_t* pixels{nullptr};
        uint32_t width{0};
        uint32_t height{0};
        // Distance in bytes between the start of one row and the next.
        uint32_t stride{0};
        // Increases every time the plugin draws a new frame. A host that uploads the pixels
        // to a texture compares this against what it uploaded last, and skips the upload
        // when nothing has changed.
        uint64_t serial{0};
    };

    // Pixel order of FramebufferView::pixels. It is a property of the plugin, so a host
    // that needs a different order converts while it copies.
    enum class FramebufferPixelFormat {
        // Byte order in memory: B, G, R, A. This is what JSFX draws.
        BGRA8,
        RGBA8
    };

    // Modifier keys, as bits of FramebufferInput::modifiers and
    // FramebufferKeyEvent::modifiers. A host reports what its own toolkit calls these and
    // the format translates: a host should never have to know what the plugin encodes
    // them as, and the one that did got it wrong.
    enum FramebufferModifier : uint32_t {
        kFramebufferModifierShift   = 1u << 0,
        kFramebufferModifierControl = 1u << 1,
        kFramebufferModifierAlt     = 1u << 2,
        kFramebufferModifierSuper   = 1u << 3
    };

    // Pointer buttons, as bits of FramebufferInput::buttons.
    enum FramebufferButton : uint32_t {
        kFramebufferButtonLeft   = 1u << 0,
        kFramebufferButtonRight  = 1u << 1,
        kFramebufferButtonMiddle = 1u << 2
    };

    // Keys that have no character of their own. Anything typable arrives as a character
    // instead, so this names only what a code point cannot.
    enum class FramebufferKey : uint32_t {
        None = 0,
        Backspace, Tab, Enter, Escape, Space, Delete,
        Left, Right, Up, Down,
        PageUp, PageDown, Home, End, Insert,
        F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12
    };

    // One key press or release. Either `character` carries what was typed, as a Unicode
    // code point, or `key` names a key that has no code point -- never both.
    struct FramebufferKeyEvent {
        uint32_t modifiers{0};
        uint32_t character{0};
        FramebufferKey key{FramebufferKey::None};
        bool pressed{false};
    };

    // Everything the plugin should know about input since the last call. The host fills it
    // in each time it has something to report; it is a snapshot for the pointer state and a
    // queue for keys, because a pointer only has a current position while key presses must
    // not be dropped.
    struct FramebufferInput {
        // Pointer position in framebuffer pixels, with the origin at the top left.
        int32_t pointerX{0};
        int32_t pointerY{0};
        // Bitmask of the buttons currently held.
        uint32_t buttons{0};
        // Bitmask of the modifier keys currently held.
        uint32_t modifiers{0};
        // Scroll since the last call, in steps normalised to +/-1.0.
        double wheel{0.0};
        double horizontalWheel{0.0};
        bool hasFocus{false};
        bool visible{true};
        bool pointerOver{false};
        std::vector<FramebufferKeyEvent> keys{};
    };

    // One entry of a menu the plugin has asked for. A separator carries no label and no
    // id; an entry with children is a submenu and its own id is 0.
    //
    // The plugin hands over a menu already built, rather than whatever string its format
    // describes menus with: parsing that string means knowing the format's rules for
    // disabled items, submenus and -- worst -- how ids are numbered, none of which a host
    // has any business knowing.
    struct FramebufferMenuItem {
        std::string label{};
        int32_t id{0};
        bool disabled{false};
        bool checked{false};
        bool separator{false};
        std::vector<FramebufferMenuItem> children{};
    };

    // Cursor shapes in terms every toolkit has. The format maps its own identifiers onto
    // these; the host maps these onto its toolkit's.
    enum class FramebufferCursor {
        Arrow,
        IBeam,
        Crosshair,
        Hand,
        SizeHorizontal,
        SizeVertical,
        SizeNESW,
        SizeNWSE,
        SizeAll,
        Wait,
        NotAllowed
    };

    // Services the plugin needs from whoever is displaying it.
    //
    // None of these may block. A plugin format whose script asks for a menu has to wait for
    // the answer -- that is inherent to what a menu is -- but it waits on its own rendering
    // thread, never on the host's. So the host is asked to *open* a menu and report the
    // outcome later, which it can do from its own event loop without a modal loop and
    // without blocking anything.
    //
    // Putting the waiting on the plugin's side rather than the host's is deliberate: it is
    // the part that deadlocks if it is got wrong, and there is one implementation of it
    // rather than one per host.
    class FramebufferUIHost {
    public:
        virtual ~FramebufferUIHost() = default;

        // Opens a menu and returns immediately. The position is in framebuffer pixels.
        //
        // `completed` must be called exactly once, with the id of the chosen item, or 0 if
        // the user dismissed the menu without choosing. It may be called from any thread,
        // including from inside this call. A host that cannot show a menu at all still has
        // to complete with 0 rather than dropping the request.
        virtual void requestMenu(const std::vector<FramebufferMenuItem>& items,
                                 int32_t x, int32_t y,
                                 std::function<void(int32_t)> completed) = 0;

        // Requests a mouse cursor shape.
        virtual void setCursor(FramebufferCursor cursor) = 0;

        // Returns the path of a file dropped on the plugin, by index, or an empty string
        // when there is none. An index of -1 asks the host to forget the dropped files.
        virtual std::string droppedFile(int32_t index) = 0;
    };

    class PluginFramebufferUIExtension : public AudioPluginInstanceExtension {
    public:
        std::string_view extensionId() const override {
            return kPluginFramebufferUIExtensionId;
        }

        virtual FramebufferPixelFormat pixelFormat() const = 0;

        // The size the plugin would like when it is first shown, which the host may use to
        // size its panel. It is a hint and nothing more: after the first frame the host's
        // size decides, and the plugin draws to whatever surfaceSize() it was last given.
        // Returns false when the plugin has no preference.
        virtual bool preferredSize(uint32_t& width, uint32_t& height) = 0;

        // Sets the surface the plugin draws into. `scaleFactor` is the display scale, 1.0
        // for a conventional display and 2.0 for a doubled one; a plugin that does not ask
        // for high resolution output ignores it. Width and height are in framebuffer
        // pixels and already include the scale.
        //
        // Call it before the first readFrame() and whenever the panel changes size. The
        // change takes effect on the plugin's next frame, so a readFrame() immediately
        // afterwards can still return the previous size -- hosts should use the width and
        // height from FramebufferView rather than the ones they last set.
        //
        // on: UI thread
        virtual void surfaceSize(uint32_t width, uint32_t height, double scaleFactor) = 0;

        // Calls `consume` with the current frame, holding the plugin's frame lock for the
        // duration, and returns whether there was a frame to give. `consume` must not call
        // back into the plugin, and must not keep the pointer.
        //
        // on: UI thread
        virtual bool readFrame(const std::function<void(const FramebufferView&)>& consume) = 0;

        // Hands input to the plugin. Key events are consumed; pointer and window state
        // replace whatever was reported before.
        //
        // on: UI thread
        virtual void deliverInput(FramebufferInput input) = 0;

        // Sets who services requestMenu(), setCursor() and droppedFile(). Passing nullptr
        // detaches and abandons any menu still open, releasing whatever was waiting on it.
        // The host must keep the object alive until it has detached.
        //
        // Detaching is not required before destroying the plugin: an implementation has to
        // release its own waiters on the way down regardless, because the alternative is a
        // teardown that hangs.
        //
        // on: UI thread
        virtual void uiHost(FramebufferUIHost* host) = 0;

        // Tells the plugin whether anyone is looking. A plugin that is not being displayed
        // need not draw, and hosts should say so rather than discarding frames.
        //
        // on: UI thread
        virtual void displayed(bool value) = 0;
    };
}
