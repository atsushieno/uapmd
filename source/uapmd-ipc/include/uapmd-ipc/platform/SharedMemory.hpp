#pragma once

#include <cstddef>
#include <memory>
#include <string>

namespace uapmd::ipc {

// Cross-platform shared memory wrapper.
//
// POSIX  : shm_open + ftruncate + mmap  (name must start with '/')
// Windows: CreateFileMapping + MapViewOfFile  (name is plain string, no '/')
//
// Usage:
//   // Creator (host process):
//   auto shm = SharedMemory::create("/uapmd-12345", sizeof(SharedAudioRegion));
//   auto* region = static_cast<SharedAudioRegion*>(shm->data());
//
//   // Consumer (engine process):
//   auto shm = SharedMemory::open("/uapmd-12345", sizeof(SharedAudioRegion));
//   auto* region = static_cast<SharedAudioRegion*>(shm->data());
class SharedMemory {
public:
    // Create a new named segment and map it; destroys any pre-existing segment
    // with the same name.  Returns nullptr on failure.
    static std::unique_ptr<SharedMemory> create(const std::string& name, size_t size);

    // Open an existing named segment for read/write.  Returns nullptr on
    // failure (segment not yet created, or wrong size).
    static std::unique_ptr<SharedMemory> open(const std::string& name, size_t size);

    ~SharedMemory();

    // Non-copyable, non-movable (raw pointers / handles must stay stable)
    SharedMemory(const SharedMemory&)            = delete;
    SharedMemory& operator=(const SharedMemory&) = delete;

    void*             data() const { return data_; }
    size_t            size() const { return size_; }
    const std::string& name() const { return name_; }

private:
    enum class Mode { Create, Open };
    SharedMemory(const std::string& name, size_t size, Mode mode);

    std::string name_;
    size_t      size_{0};
    void*       data_{nullptr};

#ifdef _WIN32
    void* handle_{nullptr};  // HANDLE returned by CreateFileMapping
#else
    int fd_{-1};
#endif
};

} // namespace uapmd::ipc
