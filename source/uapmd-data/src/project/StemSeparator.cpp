#include "uapmd-data/uapmd-data.hpp"

#include <algorithm>
#include <utility>

namespace uapmd::import {

StemSeparatorRegistry::Lease::Lease(StemSeparatorRegistry& registry, StemSeparator& separator) noexcept
    : registry_(&registry), separator_(&separator) {
}

StemSeparatorRegistry::Lease::~Lease() {
    release();
}

StemSeparatorRegistry::Lease::Lease(Lease&& other) noexcept
    : registry_(std::exchange(other.registry_, nullptr)),
      separator_(std::exchange(other.separator_, nullptr)) {
}

StemSeparatorRegistry::Lease& StemSeparatorRegistry::Lease::operator=(Lease&& other) noexcept {
    if (this != &other) {
        release();
        registry_ = std::exchange(other.registry_, nullptr);
        separator_ = std::exchange(other.separator_, nullptr);
    }
    return *this;
}

bool StemSeparatorRegistry::Lease::withdrawn() const noexcept {
    return registry_ && separator_ && registry_->isWithdrawn(*separator_);
}

void StemSeparatorRegistry::Lease::release() noexcept {
    if (registry_ && separator_)
        registry_->releaseLease(*separator_);
    registry_ = nullptr;
    separator_ = nullptr;
}

const StemSeparatorRegistry::Entry* StemSeparatorRegistry::findEntry(const StemSeparator& separator) const {
    const auto position = std::ranges::find(entries_, &separator, &Entry::separator);
    return position == entries_.end() ? nullptr : &*position;
}

StemSeparatorRegistry::Entry* StemSeparatorRegistry::findEntry(const StemSeparator& separator) {
    const auto position = std::ranges::find(entries_, &separator, &Entry::separator);
    return position == entries_.end() ? nullptr : &*position;
}

void StemSeparatorRegistry::add(StemSeparator& separator) {
    std::scoped_lock lock(mutex_);
    if (!findEntry(separator))
        entries_.push_back(Entry{&separator});
}

bool StemSeparatorRegistry::remove(StemSeparator& separator) noexcept {
    std::unique_lock lock(mutex_);
    auto* entry = findEntry(separator);
    if (!entry)
        return false;
    // Marked first so that leases observe the withdrawal and stop their runs;
    // otherwise this wait would last as long as a whole separation.
    entry->withdrawn = true;
    lease_released_.wait(lock, [this, &separator] {
        const auto* current = findEntry(separator);
        return !current || current->leases == 0;
    });
    const auto position = std::ranges::find(entries_, &separator, &Entry::separator);
    if (position == entries_.end())
        return false;
    entries_.erase(position);
    return true;
}

std::vector<StemSeparator*> StemSeparatorRegistry::separators() const {
    std::scoped_lock lock(mutex_);
    std::vector<StemSeparator*> result;
    result.reserve(entries_.size());
    for (const auto& entry : entries_)
        if (!entry.withdrawn)
            result.push_back(entry.separator);
    return result;
}

StemSeparator* StemSeparatorRegistry::get(std::string_view id) const {
    std::scoped_lock lock(mutex_);
    for (const auto& entry : entries_)
        if (!entry.withdrawn && entry.separator && entry.separator->id() == id)
            return entry.separator;
    return nullptr;
}

bool StemSeparatorRegistry::empty() const noexcept {
    std::scoped_lock lock(mutex_);
    return std::ranges::all_of(entries_, [](const Entry& entry) { return entry.withdrawn; });
}

StemSeparatorRegistry::Lease StemSeparatorRegistry::acquire(std::string_view id) {
    std::scoped_lock lock(mutex_);
    for (auto& entry : entries_) {
        if (entry.withdrawn || !entry.separator || entry.separator->id() != id)
            continue;
        ++entry.leases;
        return Lease{*this, *entry.separator};
    }
    return {};
}

void StemSeparatorRegistry::releaseLease(StemSeparator& separator) noexcept {
    {
        std::scoped_lock lock(mutex_);
        if (auto* entry = findEntry(separator); entry && entry->leases > 0)
            --entry->leases;
    }
    lease_released_.notify_all();
}

bool StemSeparatorRegistry::isWithdrawn(const StemSeparator& separator) const noexcept {
    std::scoped_lock lock(mutex_);
    const auto* entry = findEntry(separator);
    return !entry || entry->withdrawn;
}

} // namespace uapmd::import
