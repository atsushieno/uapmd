#include "MidiKeyboard.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace uapmd::gui {

MidiKeyboard::MidiKeyboard() {
    pressedKeys_.resize(128, false);
    setupKeys();
}

void MidiKeyboard::setOctaveRange(int startOctave, int numOctaves) {
    numOctaves_ = std::clamp(numOctaves, 1, 10);
    const int maxStart = std::max(0, 10 - numOctaves_);
    octaveStart_ = std::clamp(startOctave, 0, maxStart);
    setupKeys();
}

void MidiKeyboard::setKeySize(float width, float whiteHeight, float blackHeight) {
    keyWidth_ = width;
    whiteKeyHeight_ = whiteHeight;
    blackKeyHeight_ = blackHeight;
    setupKeys();
}

void MidiKeyboard::setKeyEventCallback(std::function<void(int note, int velocity, bool isPressed)> callback) {
    onKeyEvent_ = callback;
}

void MidiKeyboard::shiftOctave(int delta) {
    if (delta == 0) {
        return;
    }
    releaseAllKeys();
    const int maxStart = std::max(0, 10 - numOctaves_);
    octaveStart_ = std::clamp(octaveStart_ + delta, 0, maxStart);
    setupKeys();
}

void MidiKeyboard::setupKeys() {
    keys_.clear();

    float currentX = 0.0f;
    int startNote = octaveStart_ * 12; // C of the starting octave
    int endNote = startNote + (numOctaves_ * 12);

    for (int note = startNote; note < endNote; ++note) {
        int noteInOctave = note % 12;
        bool isBlack = isBlackKey(noteInOctave);

        if (!isBlack) {
            keys_.push_back({note, false, currentX, keyWidth_});
            currentX += keyWidth_;
        }
    }

    // Add black keys
    currentX = 0.0f;
    for (int note = startNote; note < endNote; ++note) {
        int noteInOctave = note % 12;
        bool isBlack = isBlackKey(noteInOctave);

        if (!isBlack) {
            currentX += keyWidth_;
        } else {
            // Position black key between white keys
            float blackKeyX = currentX - keyWidth_ * 0.3f;
            keys_.push_back({note, true, blackKeyX, keyWidth_ * 0.6f});
        }
    }

    // Sort keys so black keys are drawn on top
    std::sort(keys_.begin(), keys_.end(), [](const KeyInfo& a, const KeyInfo& b) {
        return !a.isBlack && b.isBlack;
    });
}

void MidiKeyboard::render() {
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 canvasPos = ImGui::GetCursorScreenPos();
    float keyboardWidth = (numOctaves_ * 7) * keyWidth_;
    float sideButtonWidth = keyWidth_;
    float totalWidth = keyboardWidth + (sideButtonWidth * 2.0f);
    ImVec2 canvasSize = ImVec2(totalWidth, whiteKeyHeight_);

    // Reserve space for the keyboard plus octave buttons
    ImGui::InvisibleButton("##keyboard", canvasSize);

    bool isHovered = ImGui::IsItemHovered();
    ImVec2 mousePos = ImGui::GetMousePos();
    bool mouseDown = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    bool mouseClicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    float relativeXFull = mousePos.x - canvasPos.x;
    float relativeY = mousePos.y - canvasPos.y;
    float keyboardStartX = canvasPos.x + sideButtonWidth;
    float relativeX = mousePos.x - keyboardStartX;
    float keyboardEndX = keyboardStartX + keyboardWidth;

    bool inLeftButton = relativeXFull >= 0.0f && relativeXFull < sideButtonWidth &&
                        relativeY >= 0.0f && relativeY < whiteKeyHeight_;
    bool inRightButton = relativeXFull >= (sideButtonWidth + keyboardWidth) &&
                         relativeXFull < totalWidth &&
                         relativeY >= 0.0f && relativeY < whiteKeyHeight_;
    bool leftHovered = isHovered && inLeftButton;
    bool rightHovered = isHovered && inRightButton;

    // Handle octave shift clicks
    if (isHovered && mouseClicked) {
        if (leftHovered) {
            shiftOctave(-1);
        } else if (rightHovered) {
            shiftOctave(1);
        }
    }

    // Handle mouse input for keys
    if (isHovered && relativeX >= 0.0f && relativeX < keyboardWidth &&
        relativeY >= 0.0f && relativeY < whiteKeyHeight_) {
        int hoveredNote = getNoteFromPosition(relativeX, relativeY);

        if (mouseDown && hoveredNote != -1) {
            if (mouseDownKey_ != hoveredNote) {
                if (mouseDownKey_ != -1) {
                    releaseKey(mouseDownKey_);
                }
                pressKey(hoveredNote);
                mouseDownKey_ = hoveredNote;
            }
        } else if (!mouseDown && mouseDownKey_ != -1) {
            releaseKey(mouseDownKey_);
            mouseDownKey_ = -1;
        }
    } else if (!mouseDown && mouseDownKey_ != -1) {
        releaseKey(mouseDownKey_);
        mouseDownKey_ = -1;
    }

    // Draw octave shift buttons as key-like shapes
    auto drawShiftButton = [&](ImVec2 pos, const char* glyph, bool hovered) {
        ImVec2 size(sideButtonWidth, whiteKeyHeight_);
        ImU32 keyColor = hovered ? IM_COL32(210, 210, 230, 255) : IM_COL32(235, 235, 235, 255);
        ImU32 borderColor = IM_COL32(100, 100, 100, 255);
        drawList->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), keyColor);
        drawList->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), borderColor);

        ImVec2 textSize = ImGui::CalcTextSize(glyph);
        ImVec2 textPos = ImVec2(pos.x + (size.x - textSize.x) * 0.5f,
                                pos.y + (size.y - textSize.y) * 0.5f);
        drawList->AddText(textPos, IM_COL32(0, 0, 0, 255), glyph);
    };

    drawShiftButton(canvasPos, "<", leftHovered);
    drawShiftButton(ImVec2(canvasPos.x + sideButtonWidth + keyboardWidth, canvasPos.y), ">",
                    rightHovered);

    // Draw keys
    for (const auto& key : keys_) {
        ImVec2 keyPos = ImVec2(keyboardStartX + key.x, canvasPos.y);
        ImVec2 keySize = ImVec2(key.width, key.isBlack ? blackKeyHeight_ : whiteKeyHeight_);

        bool isPressed = pressedKeys_[key.note];
        ImU32 keyColor;
        ImU32 borderColor = IM_COL32(100, 100, 100, 255);

        if (key.isBlack) {
            keyColor = isPressed ? IM_COL32(100, 100, 100, 255) : IM_COL32(50, 50, 50, 255);
        } else {
            keyColor = isPressed ? IM_COL32(200, 200, 255, 255) : IM_COL32(255, 255, 255, 255);
        }

        // Draw key
        drawList->AddRectFilled(keyPos, ImVec2(keyPos.x + keySize.x, keyPos.y + keySize.y), keyColor);
        drawList->AddRect(keyPos, ImVec2(keyPos.x + keySize.x, keyPos.y + keySize.y), borderColor);

        // Draw note name only for C notes on white keys
        if (!key.isBlack && (key.note % 12) == 0) {
            const char* noteName = getNoteName(key.note);
            ImVec2 textSize = ImGui::CalcTextSize(noteName);
            ImVec2 textPos = ImVec2(
                keyPos.x + (keySize.x - textSize.x) * 0.5f,
                keyPos.y + keySize.y - textSize.y - 5.0f
            );
            drawList->AddText(textPos, IM_COL32(0, 0, 0, 255), noteName);
        }
    }
}

void MidiKeyboard::pressKey(int note, int velocity) {
    if (note >= 0 && note < 128 && !pressedKeys_[note]) {
        pressedKeys_[note] = true;
        handleKeyEvent(note, true, velocity);
    }
}

void MidiKeyboard::releaseKey(int note) {
    if (note >= 0 && note < 128 && pressedKeys_[note]) {
        pressedKeys_[note] = false;
        handleKeyEvent(note, false, 0);
    }
}

void MidiKeyboard::releaseAllKeys() {
    for (int i = 0; i < 128; ++i) {
        if (pressedKeys_[i]) {
            releaseKey(i);
        }
    }
}

int MidiKeyboard::getNoteFromPosition(float x, float y) {
    // Check black keys first (they're on top)
    for (const auto& key : keys_) {
        if (key.isBlack && x >= key.x && x < key.x + key.width && y < blackKeyHeight_) {
            return key.note;
        }
    }

    // Check white keys
    for (const auto& key : keys_) {
        if (!key.isBlack && x >= key.x && x < key.x + key.width && y < whiteKeyHeight_) {
            return key.note;
        }
    }

    return -1;
}

bool MidiKeyboard::isBlackKey(int noteInOctave) {
    return noteInOctave == 1 || noteInOctave == 3 || noteInOctave == 6 ||
           noteInOctave == 8 || noteInOctave == 10;
}

const char* MidiKeyboard::getNoteName(int note) {
    static const char* noteNames[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    static char buffer[8];
    int octave = note / 12;
    int noteInOctave = note % 12;
    snprintf(buffer, sizeof(buffer), "%s%d", noteNames[noteInOctave], octave);
    return buffer;
}

void MidiKeyboard::handleKeyEvent(int note, bool isPressed, int velocity) {
    if (onKeyEvent_) {
        onKeyEvent_(note, velocity, isPressed);
    }
}

}
