#pragma once

#include <algorithm>
#include <chrono>
#include <string_view>
#include <thread>
#include <vector>

#include <uapmd-engine/uapmd-engine.hpp>

namespace uapmd_mir {

enum class MirDiagnosticKind {
    Calls,
    Tempo,
    Beats,
    Onsets,
    Meter,
    Chords,
    Other,
    Count,
};

// Buffers every raw analyzer value produced during one run and emits them
// grouped by kind once the run is over. Logging them as they are produced
// swamps the logger; the analysis is off the audio thread but still hot.
class MirDiagnosticLog {
public:
    MirDiagnosticLog() {
        bytes_.reserve(1024 * 1024);
        records_.reserve(16384);
    }

    void append(std::string_view message) {
        const auto required = bytes_.size() + message.size() + 1;
        if (required > bytes_.capacity())
            bytes_.reserve(std::max(required, bytes_.capacity() * 2));
        const auto offset = bytes_.size();
        bytes_.insert(bytes_.end(), message.begin(), message.end());
        bytes_.push_back('\0');
        records_.push_back({kindOf(message), offset});
    }

    void flush() {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        size_t emitted = 0;
        for (int kind = 0; kind < static_cast<int>(MirDiagnosticKind::Count); ++kind)
            for (const auto& record : records_) {
                if (static_cast<int>(record.kind) != kind)
                    continue;
                remidy::Logger::global()->logInfo("%s", bytes_.data() + record.offset);
                if (++emitted % 32 == 0)
                    std::this_thread::sleep_for(std::chrono::milliseconds(25));
            }
        records_.clear();
        bytes_.clear();
    }

private:
    struct Record {
        MirDiagnosticKind kind;
        size_t offset;
    };

    static MirDiagnosticKind kindOf(std::string_view message) {
        if (message.starts_with("uapmd calling"))
            return MirDiagnosticKind::Calls;
        if (message.contains("BPM") || message.starts_with("uapmd tempo-map"))
            return MirDiagnosticKind::Tempo;
        if (message.contains("beat"))
            return MirDiagnosticKind::Beats;
        if (message.contains("onset"))
            return MirDiagnosticKind::Onsets;
        if (message.starts_with("uapmd meter-map"))
            return MirDiagnosticKind::Meter;
        if (message.contains("chord"))
            return MirDiagnosticKind::Chords;
        return MirDiagnosticKind::Other;
    }

    std::vector<char> bytes_;
    std::vector<Record> records_;
};

// Flushes the log even when the analysis throws or is cancelled.
struct MirDiagnosticLogGuard {
    MirDiagnosticLog& diagnostics;
    ~MirDiagnosticLogGuard() { diagnostics.flush(); }
};

} // namespace uapmd_mir
