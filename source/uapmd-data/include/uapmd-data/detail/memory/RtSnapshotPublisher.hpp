#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

namespace uapmd {

    // Publishes immutable snapshots to a fixed number of real-time readers.
    // Readers only perform lock-free pointer atomics. Snapshot allocation,
    // retirement, reclamation, and destruction remain on the publishing thread.
    //
    // Each concurrently active reader must use a distinct reader index. Reader
    // indices are fixed so entering the real-time path never registers a thread,
    // allocates memory, or acquires a lock.
    template<typename T, size_t ReaderCount = 1>
    class RtSnapshotPublisher {
        static_assert(ReaderCount > 0);
        static_assert(std::atomic<const T*>::is_always_lock_free,
            "RtSnapshotPublisher requires lock-free pointer atomics");

    public:
        class Guard {
        public:
            Guard() = default;
            Guard(const Guard&) = delete;
            Guard& operator=(const Guard&) = delete;

            Guard(Guard&& other) noexcept
                : owner_(std::exchange(other.owner_, nullptr))
                , reader_index_(other.reader_index_)
                , snapshot_(std::exchange(other.snapshot_, nullptr)) {}

            Guard& operator=(Guard&& other) noexcept {
                if (this == &other)
                    return *this;
                reset();
                owner_ = std::exchange(other.owner_, nullptr);
                reader_index_ = other.reader_index_;
                snapshot_ = std::exchange(other.snapshot_, nullptr);
                return *this;
            }

            ~Guard() { reset(); }

            const T* get() const noexcept { return snapshot_; }
            const T& operator*() const noexcept { return *snapshot_; }
            const T* operator->() const noexcept { return snapshot_; }
            explicit operator bool() const noexcept { return snapshot_ != nullptr; }

        private:
            friend class RtSnapshotPublisher;

            Guard(const RtSnapshotPublisher* owner, size_t readerIndex, const T* snapshot) noexcept
                : owner_(owner)
                , reader_index_(readerIndex)
                , snapshot_(snapshot) {}

            void reset() noexcept {
                if (!owner_)
                    return;
                owner_->hazards_[reader_index_].store(nullptr, std::memory_order_seq_cst);
                owner_ = nullptr;
                snapshot_ = nullptr;
            }

            const RtSnapshotPublisher* owner_{};
            size_t reader_index_{};
            const T* snapshot_{};
        };

        RtSnapshotPublisher()
            : RtSnapshotPublisher(std::make_unique<const T>()) {}

        explicit RtSnapshotPublisher(std::unique_ptr<const T> initialSnapshot)
            : current_owner_(std::move(initialSnapshot)) {
            current_.store(current_owner_.get(), std::memory_order_seq_cst);
        }

        RtSnapshotPublisher(const RtSnapshotPublisher&) = delete;
        RtSnapshotPublisher& operator=(const RtSnapshotPublisher&) = delete;
        RtSnapshotPublisher(RtSnapshotPublisher&&) = delete;
        RtSnapshotPublisher& operator=(RtSnapshotPublisher&&) = delete;

        ~RtSnapshotPublisher() {
            current_.store(nullptr, std::memory_order_seq_cst);
        }

        [[nodiscard]] Guard protect(size_t readerIndex = 0) const noexcept {
            if (readerIndex >= ReaderCount)
                return {};

            const T* snapshot;
            do {
                snapshot = current_.load(std::memory_order_seq_cst);
                hazards_[readerIndex].store(snapshot, std::memory_order_seq_cst);
            } while (snapshot != current_.load(std::memory_order_seq_cst));
            return Guard(this, readerIndex, snapshot);
        }

        // Publishing thread only. The old snapshot is reclaimed here when no
        // reader protects it; otherwise it remains retired until a later publish
        // or explicit reclaimRetired() call on the publishing thread.
        void publish(std::unique_ptr<const T> snapshot) {
            retired_.push_back(std::move(current_owner_));
            current_owner_ = std::move(snapshot);
            current_.store(current_owner_.get(), std::memory_order_seq_cst);
            reclaimRetired();
        }

        void reclaimRetired() {
            const auto protectedByReader = [this](const std::unique_ptr<const T>& candidate) {
                if (!candidate)
                    return false;
                return std::any_of(hazards_.begin(), hazards_.end(), [&candidate](const auto& hazard) {
                    return hazard.load(std::memory_order_seq_cst) == candidate.get();
                });
            };
            std::erase_if(retired_, [&protectedByReader](const auto& candidate) {
                return !protectedByReader(candidate);
            });
        }

        // Publishing thread only. This is intended for non-RT code that already
        // serializes snapshot publication and therefore needs no hazard slot.
        const T* currentOnPublisherThread() const noexcept { return current_owner_.get(); }

    private:
        std::atomic<const T*> current_{nullptr};
        mutable std::array<std::atomic<const T*>, ReaderCount> hazards_{};
        std::unique_ptr<const T> current_owner_;
        std::vector<std::unique_ptr<const T>> retired_;
    };

} // namespace uapmd
