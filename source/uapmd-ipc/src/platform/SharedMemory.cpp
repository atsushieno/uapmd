#include <uapmd-ipc/platform/SharedMemory.hpp>

#include <stdexcept>
#include <system_error>

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
#else
    #include <fcntl.h>
    #include <sys/mman.h>
    #include <sys/stat.h>
    #include <unistd.h>
#endif

namespace uapmd::ipc {

// ── POSIX implementation ──────────────────────────────────────────────────

#ifndef _WIN32

SharedMemory::SharedMemory(const std::string& name, size_t size, Mode mode)
    : name_(name), size_(size) {
    if (mode == Mode::Create) {
        // Unlink any stale segment so ftruncate always works.
        ::shm_unlink(name.c_str());
        fd_ = ::shm_open(name.c_str(), O_CREAT | O_RDWR, 0600);
        if (fd_ < 0)
            throw std::system_error(errno, std::system_category(), "shm_open(create)");
        if (::ftruncate(fd_, static_cast<off_t>(size)) != 0) {
            ::close(fd_);
            ::shm_unlink(name.c_str());
            throw std::system_error(errno, std::system_category(), "ftruncate");
        }
    } else {
        fd_ = ::shm_open(name.c_str(), O_RDWR, 0);
        if (fd_ < 0)
            throw std::system_error(errno, std::system_category(), "shm_open(open)");
    }
    data_ = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (data_ == MAP_FAILED) {
        data_ = nullptr;
        ::close(fd_);
        if (mode == Mode::Create)
            ::shm_unlink(name.c_str());
        throw std::system_error(errno, std::system_category(), "mmap");
    }
}

SharedMemory::~SharedMemory() {
    if (data_)
        ::munmap(data_, size_);
    if (fd_ >= 0)
        ::close(fd_);
    // Creator is responsible for unlinking; unlink in create mode only.
    // We can't know which mode we are here without storing it, so the
    // creator (RemoteEngineProxy) calls shm_unlink explicitly on shutdown.
}

// ── Windows implementation ────────────────────────────────────────────────

#else // _WIN32

SharedMemory::SharedMemory(const std::string& name, size_t size, Mode mode)
    : name_(name), size_(size) {
    const DWORD hi = static_cast<DWORD>(size >> 32);
    const DWORD lo = static_cast<DWORD>(size & 0xFFFFFFFF);

    if (mode == Mode::Create) {
        handle_ = ::CreateFileMappingA(
            INVALID_HANDLE_VALUE, nullptr,
            PAGE_READWRITE, hi, lo,
            name.c_str());
        if (!handle_ || handle_ == INVALID_HANDLE_VALUE)
            throw std::system_error(static_cast<int>(::GetLastError()),
                                    std::system_category(), "CreateFileMapping");
    } else {
        handle_ = ::OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name.c_str());
        if (!handle_ || handle_ == INVALID_HANDLE_VALUE)
            throw std::system_error(static_cast<int>(::GetLastError()),
                                    std::system_category(), "OpenFileMapping");
    }

    data_ = ::MapViewOfFile(handle_, FILE_MAP_ALL_ACCESS, 0, 0, size);
    if (!data_) {
        ::CloseHandle(handle_);
        handle_ = nullptr;
        throw std::system_error(static_cast<int>(::GetLastError()),
                                std::system_category(), "MapViewOfFile");
    }
}

SharedMemory::~SharedMemory() {
    if (data_)
        ::UnmapViewOfFile(data_);
    if (handle_ && handle_ != INVALID_HANDLE_VALUE)
        ::CloseHandle(handle_);
}

#endif // _WIN32

// ── Factory methods (shared) ──────────────────────────────────────────────

std::unique_ptr<SharedMemory> SharedMemory::create(const std::string& name, size_t size) {
    try {
        return std::unique_ptr<SharedMemory>(new SharedMemory(name, size, Mode::Create));
    } catch (...) {
        return nullptr;
    }
}

std::unique_ptr<SharedMemory> SharedMemory::open(const std::string& name, size_t size) {
    try {
        return std::unique_ptr<SharedMemory>(new SharedMemory(name, size, Mode::Open));
    } catch (...) {
        return nullptr;
    }
}

} // namespace uapmd::ipc
