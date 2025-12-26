#pragma once

#include <imgui.h>
#include <vector>
#include <functional>

namespace uapmd::gui {
    class MidiKeyboard {
    public:
        struct KeyPress {
            int note;
            int velocity;
            bool isPressed;
        };

    private:
        int octaveStart_ = 4; // C4 as starting octave
        int numOctaves_ = 2;  // Show 2 octaves by default
        float keyWidth_ = 20.0f;
        float whiteKeyHeight_ = 80.0f;
        float blackKeyHeight_ = 50.0f;

        std::vector<bool> pressedKeys_;
        int mouseDownKey_ = -1;
        int highlightedKey_ = -1;

        std::function<void(int note, int velocity, bool isPressed)> onKeyEvent_;

        struct KeyInfo {
            int note;
            bool isBlack;
            float x;
            float width;
        };

        std::vector<KeyInfo> keys_;

    public:
        MidiKeyboard();

        void setOctaveRange(int startOctave, int numOctaves);
        void setKeySize(float width, float whiteHeight, float blackHeight);
        void setKeyEventCallback(std::function<void(int note, int velocity, bool isPressed)> callback);
        void shiftOctave(int delta);
        int octaveStart() const { return octaveStart_; }
        int numOctaves() const { return numOctaves_; }

        void render();
        void pressKey(int note, int velocity = 100);
        void releaseKey(int note);
        void releaseAllKeys();
        void setHighlightedKey(int note);

    private:
        void setupKeys();
        int getNoteFromPosition(float x, float y);
        bool isBlackKey(int note);
        const char* getNoteName(int note);
        void handleKeyEvent(int note, bool isPressed, int velocity = 100);
    };
}
