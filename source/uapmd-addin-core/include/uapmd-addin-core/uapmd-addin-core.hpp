#pragma once

#include <span>
#include <string_view>

#if defined(UAPMD_BUILDING_ADDIN) && defined(_WIN32)
    #define UAPMD_ADDIN_EXPORT __declspec(dllexport)
#elif defined(UAPMD_BUILDING_ADDIN) && (defined(__GNUC__) || defined(__clang__))
    #define UAPMD_ADDIN_EXPORT __attribute__((visibility("default")))
#else
    #define UAPMD_ADDIN_EXPORT
#endif

namespace uapmd {

class AddinHost;

struct AddinIdentity {
    std::string_view package_id;
    std::string_view addin_id;
};

class Addin {
public:
    virtual ~Addin() = default;

    virtual AddinIdentity identity() const noexcept = 0;
    virtual std::string_view name() const noexcept = 0;
    virtual std::string_view path() const noexcept = 0;
    virtual bool initialize(AddinHost& host) noexcept = 0;
    virtual void cleanup(AddinHost& host) noexcept = 0;
};

class AddinEntry {
public:
    virtual ~AddinEntry() = default;

    virtual std::string_view packageId() const noexcept = 0;
    virtual std::span<Addin* const> addins() noexcept = 0;
};

class AddinHost {
public:
    virtual ~AddinHost() = default;

    // The returned pointer is owned by the path-specific host implementation.
    // It is valid only while the corresponding extension point remains active.
    virtual void* extensionPoint(std::string_view path) noexcept = 0;
};

using AddinEntryFunction = AddinEntry* (*)() noexcept;

} // namespace uapmd

// Every dynamically loaded addin library exports this symbol. Addin targets
// define UAPMD_BUILDING_ADDIN through add_uapmd_addin_library().
extern "C" UAPMD_ADDIN_EXPORT uapmd::AddinEntry* uapmd_addin_entry() noexcept;
