#include <cstdint>
#include <utility>
#include <type_traits>
#include <vector>
#include <memory>
#include <mutex>
#include <sstream>
#include <algorithm>

//---- imports from JS implementation ----

__attribute__((import_module("env"), import_name("memcpyToOther32")))
extern void memcpyToOther32(uint32_t destP32, const void *src, uint32_t count);
__attribute__((import_module("env"), import_name("memcpyFromOther32")))
extern void memcpyFromOther32(void *dest, uint32_t srcP32, uint32_t count);
__attribute__((import_module("env"), import_name("procExit")))
extern void procExit(uint32_t code);
__attribute__((import_module("env"), import_name("stdoutLine")))
extern void sendStdoutLine(const char *chars, size_t length);
__attribute__((import_module("env"), import_name("stderrLine")))
extern void sendStderrLine(const char *chars, size_t length);
__attribute__((import_module("env"), import_name("getRandom64")))
extern uint64_t getRandom64();
__attribute__((import_module("env"), import_name("getClockMs")))
extern double getClockMs(uint32_t clockId);
__attribute__((import_module("env"), import_name("getClockResNs")))
extern uint32_t getClockResNs(uint32_t clockId);

// Pointer to remote memory
template<class T>
struct P32 {
	uint32_t remotePointer;
	
	P32(uint32_t remotePointer=0) : remotePointer(remotePointer) {}
	
	std::remove_cv_t<T> get(size_t index=0) const {
		std::remove_cv_t<T> result;
		memcpyFromOther32(&result, remotePointer + uint32_t(index*sizeof(T)), uint32_t(sizeof(T)));
		return result;
	}
	template<class T2>
	void set(T2 &&convert, size_t index=0) const {
		T value = convert;
		memcpyToOther32(remotePointer + uint32_t(index*sizeof(T)), &value, uint32_t(sizeof(T)));
	}
	
	P32 operator+(int32_t delta) {
		return {remotePointer + delta*sizeof(T)};
	}
	P32 operator+=(int32_t delta) {
		remotePointer += delta*sizeof(T);
	}
};

std::string getString(P32<const char> path, uint32_t pathLength) {
	std::string pathStr;
	pathStr.resize(pathLength);
	memcpyFromOther32(pathStr.data(), path.remotePointer, pathLength);
	return pathStr;
}

//---- relevant POSIX types ----

using result_t = uint16_t;

struct fdstat {
	uint8_t filetype = 4;
	uint16_t flags = 0;
	uint64_t rightsBase = 0;
	uint64_t rightsInheriting = 0;
};

struct filestat {
	uint64_t device = 0;
	uint64_t inode = 0;
	uint8_t filetype = 0;
	uint64_t linkCount = 1;
	uint64_t size = 0;
	// accessed, modified, created
	uint64_t aTime = 0;
	uint64_t mTime = 0;
	uint64_t cTime = 0;
};

struct prestat {
	uint8_t type;
	struct {
		uint32_t nameLength;
	} directory;
};

struct iovec32 {
	P32<void> buffer;
	uint32_t length;
};

struct subscription32 {
	uint64_t userData;
	uint8_t eventType;
	union {
		struct {
			uint32_t clockId;
			uint64_t timestamp;
			uint64_t precision;
			uint16_t subClockFlags;
		} clock;
		struct {
			uint32_t fd;
		} fileReadWrite;
	};
};

struct event32 {
	uint64_t userData;
	result_t error;
	uint8_t eventType;
	struct {
		uint64_t numBytes;
		uint16_t flags;
	} fileReadWrite;
};

//---- in-memory VFS ----

template<class V>
void logExpr(const char *prefix, V &&value) {
	std::stringstream stream;
	stream << prefix << " = " << value;
	auto result = stream.str();
	sendStdoutLine(result.c_str(), result.size());
}
#define LOG_EXPR(expr) logExpr(#expr, (expr));

struct VfsNode {
	bool isDir = true;
	std::string name;
	std::vector<char> fileContents;
	std::vector<std::unique_ptr<VfsNode>> dirContents;
	
	VfsNode(const std::string &name="") : name(name) {}

	VfsNode * get(const std::string &name, bool createIfMissing) {
		for (auto &v : dirContents) {
			if (v->name == name) return v.get();
		}
		if (createIfMissing) {
			dirContents.emplace_back(std::unique_ptr<VfsNode>{new VfsNode(name)});
			return dirContents.back().get();
		}
		return nullptr;
	}

	result_t allocate(size_t offset, size_t length) {
		if (isDir) return EISDIR;
		fileContents.insert(fileContents.begin() + offset, length, 0);
		return 0;
	}

	result_t setSize(size_t size) {
		if (isDir) return EISDIR;
		fileContents.resize(size);
		return 0;
	}

	filestat & stat() {
		fstat.filetype = (isDir ? 4 : 3);
		return fstat;
	}
private:
	filestat fstat;
};
struct VfsHandle {
	result_t error = 0;
	VfsNode *node = nullptr;

	fdstat stat;
	uint64_t position = 0;

	VfsHandle(result_t error) : error(error) {}
	VfsHandle(VfsNode &node) : node(&node) {}
		
	operator bool() const {
		return node != nullptr;
	}
	VfsNode * operator->() const {
		return node;
	}
};

static std::recursive_mutex vfsMutex;
static VfsNode vfsRoot;
// handles are addressed by index, retained with a NULL `node` when closed (and can be re-used)
static std::vector<VfsHandle> vfsHandles{EINVAL, EINVAL, EINVAL, vfsRoot};

size_t vfsObtainFileHandle(VfsNode &node, fdstat stat) {
	for (size_t i = 4; i < vfsHandles.size(); ++i) {
		auto &handle = vfsHandles[i];
		if (!handle) {
			handle = {node};
			handle.stat = stat;
			return i;
		}
	}
	auto index = vfsHandles.size();
	VfsHandle handle{node};
	handle.stat = stat;
	vfsHandles.emplace_back(handle);
	return index;
}

VfsNode * vfsGet(const std::string &path, bool createIfMissing=false) {
	auto *node = &vfsRoot;
	std::istringstream pathStream(path);
	std::string name;
	while (node && std::getline(pathStream, name, '/')) {
		node = node->get(name, createIfMissing);
	}
	return node;
}

std::string pendingPath;
static char dummyChar;
extern "C" {
	__attribute__((export_name("vfs_setPath")))
	char * vfs_setPath(size_t size) {
		std::lock_guard<std::recursive_mutex> lock{vfsMutex};
		pendingPath.resize(size);
		return pendingPath.data();
	}
	__attribute__((export_name("vfs_createFile")))
	char * vfs_createFile(size_t size) {
		std::lock_guard<std::recursive_mutex> lock{vfsMutex};
		if (pendingPath[0] != '/') return nullptr;
		auto node = vfsGet(pendingPath.substr(1), true);
		node->isDir = false;
		node->dirContents.clear();
		node->fileContents.resize(size);
		return (size > 0) ? node->fileContents.data() : &dummyChar; // The JS will fill this with the file data
	}
}

//---- WASI implementation ----

std::recursive_mutex iovecBufferMutex;
std::vector<char> iovecBuffer;

template<class Fn>
void forEachIoVecRead(P32<const iovec32> ioBufferList, uint32_t ioBufferCount, Fn &&fn) {
	std::lock_guard<std::recursive_mutex> lock{iovecBufferMutex};
	for (uint32_t i = 0; i < ioBufferCount; ++i) {
		auto vec = (ioBufferList + i).get();
		if (!vec.length) continue; // odd but possible: https://github.com/emscripten-core/emscripten/issues/19244
		iovecBuffer.resize(vec.length);
		memcpyFromOther32(iovecBuffer.data(), vec.buffer.remotePointer, vec.length);
		fn(iovecBuffer.data(), iovecBuffer.size());
	}
}
template<class Fn>
void forEachIoVecFill(P32<const iovec32> ioBufferList, uint32_t ioBufferCount, Fn &&fn) {
	std::lock_guard<std::recursive_mutex> lock{iovecBufferMutex};
	for (uint32_t i = 0; i < ioBufferCount; ++i) {
		auto vec = (ioBufferList + i).get();
		if (!vec.length) continue;
		iovecBuffer.resize(vec.length);
		fn(iovecBuffer.data(), iovecBuffer.size());
		memcpyToOther32(vec.buffer.remotePointer, iovecBuffer.data(), vec.length);
	}
}

std::vector<char> stdoutLineBuffer, stderrLineBuffer;

static VfsHandle invalidHandle{EBADF};
VfsHandle & getHandle(uint32_t fd) {
	if (fd >= vfsHandles.size()) return invalidHandle;
	return vfsHandles[fd];
}

extern "C" {
	__attribute__((export_name("wasi32_snapshot_preview1__args_sizes_get")))
	result_t wasi32_snapshot_preview1__args_sizes_get(P32<size_t> count, P32<size_t> bufferSize) {
		count.set(0);
		bufferSize.set(0);
		return 0;
	}
	__attribute__((export_name("wasi32_snapshot_preview1__args_get")))
	result_t wasi32_snapshot_preview1__args_get(P32<P32<const char>> args, P32<char> buffer) {
		return 0;
	}
	__attribute__((export_name("wasi32_snapshot_preview1__clock_res_get")))
	result_t wasi32_snapshot_preview1__clock_res_get(uint32_t clock_id, P32<uint64_t> resolution) {
		auto res = getClockResNs(clock_id);
		if (!res) return ENOTCAPABLE;
		resolution.set(uint64_t(res));
		return 0;
	}
	__attribute__((export_name("wasi32_snapshot_preview1__clock_time_get")))
	result_t wasi32_snapshot_preview1__clock_time_get(uint32_t clock_id, uint64_t withResolution, P32<uint64_t> time) {
		double ms = getClockMs(clock_id);
		auto ns = uint64_t(ms*1000);
		time.set(ns);
		return 0;
	}
	__attribute__((export_name("wasi32_snapshot_preview1__environ_sizes_get")))
	result_t wasi32_snapshot_preview1__environ_sizes_get(P32<size_t> items, P32<size_t> totalSize) {
		items.set(0);
		totalSize.set(0);
		return 0;
	}
	__attribute__((export_name("wasi32_snapshot_preview1__environ_get")))
	result_t wasi32_snapshot_preview1__environ_get(P32<P32<const char>> env, P32<char> buffer) {
		return 0;
	}

	__attribute__((export_name("wasi32_snapshot_preview1__fd_advise")))
	result_t wasi32_snapshot_preview1__fd_advise(uint32_t fd, int64_t offset, int64_t len, uint8_t advice) {
		std::lock_guard<std::recursive_mutex> lock{vfsMutex};
		return ENOTCAPABLE;
	}
	__attribute__((export_name("wasi32_snapshot_preview1__fd_allocate")))
	result_t wasi32_snapshot_preview1__fd_allocate(uint32_t fd, int64_t offset, int64_t len) {
		std::lock_guard<std::recursive_mutex> lock{vfsMutex};
		auto &handle = getHandle(fd);
		if (!handle) return handle.error;
		return handle->allocate(size_t(offset), size_t(len));
	}
	__attribute__((export_name("wasi32_snapshot_preview1__fd_close")))
	result_t wasi32_snapshot_preview1__fd_close(uint32_t fd) {
		std::lock_guard<std::recursive_mutex> lock{vfsMutex};
		auto &handle = getHandle(fd);
		if (!handle) return handle.error;
		return handle = invalidHandle;
	}
	__attribute__((export_name("wasi32_snapshot_preview1__fd_datasync")))
	result_t wasi32_snapshot_preview1__fd_datasync(uint32_t fd) {
		std::lock_guard<std::recursive_mutex> lock{vfsMutex};
		return ENOTCAPABLE; // all virtual for now
	}
	__attribute__((export_name("wasi32_snapshot_preview1__fd_fdstat_get")))
	result_t wasi32_snapshot_preview1__fd_fdstat_get(uint32_t fd, P32<fdstat> stat) {
		std::lock_guard<std::recursive_mutex> lock{vfsMutex};
		auto &handle = getHandle(fd);
		if (!handle) return handle.error;
		stat.set(handle.stat);
		return 0;
	}
	__attribute__((export_name("wasi32_snapshot_preview1__fd_fdstat_set_flags")))
	result_t wasi32_snapshot_preview1__fd_fdstat_set_flags(uint32_t fd, uint16_t flags) {
		std::lock_guard<std::recursive_mutex> lock{vfsMutex};
		auto &handle = getHandle(fd);
		if (!handle) return handle.error;
		handle.stat.flags = flags;
		return 0;
	}
	__attribute__((export_name("wasi32_snapshot_preview1__fd_fdstat_set_rights")))
	result_t wasi32_snapshot_preview1__fd_fdstat_set_rights(uint32_t fd, uint64_t rightsBase, uint64_t rightsInheriting) {
		std::lock_guard<std::recursive_mutex> lock{vfsMutex};
		auto &handle = getHandle(fd);
		if (!handle) return handle.error;
		handle.stat.rightsBase = rightsBase;
		handle.stat.rightsInheriting = rightsInheriting;
		return 0;
	}
	__attribute__((export_name("wasi32_snapshot_preview1__fd_filestat_get")))
	result_t wasi32_snapshot_preview1__fd_filestat_get(uint32_t fd, P32<filestat> stat) {
		std::lock_guard<std::recursive_mutex> lock{vfsMutex};
		auto &handle = getHandle(fd);
		if (!handle) return handle.error;
		stat.set(handle->stat());
		return 0;
	}
	__attribute__((export_name("wasi32_snapshot_preview1__fd_filestat_set_size")))
	result_t wasi32_snapshot_preview1__fd_filestat_set_size(uint32_t fd, uint64_t size) {
		std::lock_guard<std::recursive_mutex> lock{vfsMutex};
		auto &handle = getHandle(fd);
		if (!handle) return handle.error;
		if (handle.position > size) handle.position = size;
		return handle->setSize(size);
	}
	__attribute__((export_name("wasi32_snapshot_preview1__fd_filestat_set_times")))
	result_t wasi32_snapshot_preview1__fd_filestat_set_times(uint32_t fd, uint64_t aTime, uint64_t mTime, uint16_t flags) {
		std::lock_guard<std::recursive_mutex> lock{vfsMutex};
		auto &handle = getHandle(fd);
		if (!handle) return handle.error;
		auto &stat = handle->stat();
		if (flags&1) stat.aTime = aTime;
		if (flags&2) stat.mTime = mTime;
		return 0;
	}
	__attribute__((export_name("wasi32_snapshot_preview1__fd_pread")))
	result_t wasi32_snapshot_preview1__fd_pread(uint32_t fd, P32<const iovec32> ioBufferList, uint32_t ioBufferCount, uint64_t offset, P32<uint32_t> bytesRead) {
		std::lock_guard<std::recursive_mutex> lock{vfsMutex};
		return ENOTCAPABLE;
	}
	__attribute__((export_name("wasi32_snapshot_preview1__fd_prestat_get")))
	result_t wasi32_snapshot_preview1__fd_prestat_get(uint32_t fd, P32<prestat> stat) {
		std::lock_guard<std::recursive_mutex> lock{vfsMutex};
		if (fd != 3) return EBADF;
		stat.set(prestat{
			.type=0, // pre-opened directory
			.directory={.nameLength=1}
		});
		return 0;
	}
	__attribute__((export_name("wasi32_snapshot_preview1__fd_prestat_dir_name")))
	result_t wasi32_snapshot_preview1__fd_prestat_dir_name(uint32_t fd, P32<char> path, uint32_t pathLength) {
		std::lock_guard<std::recursive_mutex> lock{vfsMutex};
		if (fd != 3) return EBADF;
		auto bytes = std::min<size_t>(pathLength, 2);
		memcpyToOther32(path.remotePointer, "/", bytes);
		return 0;
	}
	__attribute__((export_name("wasi32_snapshot_preview1__fd_pwrite")))
	result_t wasi32_snapshot_preview1__fd_pwrite(uint32_t fd, P32<const iovec32> ioBufferList, uint32_t ioBufferCount, uint64_t offset, P32<uint32_t> bytesWritten) {
		std::lock_guard<std::recursive_mutex> lock{vfsMutex};
		return ENOTCAPABLE;
	}
	__attribute__((export_name("wasi32_snapshot_preview1__fd_read")))
	result_t wasi32_snapshot_preview1__fd_read(uint32_t fd, P32<const iovec32> ioBufferList, uint32_t ioBufferCount, P32<uint32_t> bytesRead) {
		std::lock_guard<std::recursive_mutex> lock{vfsMutex};
		auto &handle = getHandle(fd);
		if (!handle) return handle.error;
		if (handle->isDir) return EISDIR;

		uint32_t total = 0;
		forEachIoVecFill(ioBufferList, ioBufferCount, [&](char *bytes, size_t length){
			if (handle.position + length > handle->fileContents.size()) {
				length = handle->fileContents.size() - handle.position;
			}
			std::memcpy(bytes, handle->fileContents.data() + handle.position, length);
			handle.position += length;
			total += length;
		});
		bytesRead.set(total);
		return 0;
	}
	__attribute__((export_name("wasi32_snapshot_preview1__fd_readdir")))
	result_t wasi32_snapshot_preview1__fd_readdir(uint32_t fd, P32<void> buffer, uint32_t bufferSize, uint64_t cookie, P32<uint32_t> bytesUsed) {
		std::lock_guard<std::recursive_mutex> lock{vfsMutex};
		return ENOTCAPABLE;
	}
	__attribute__((export_name("wasi32_snapshot_preview1__fd_renumber")))
	result_t wasi32_snapshot_preview1__fd_renumber(uint32_t fdFrom, uint32_t fdTo) {
		std::lock_guard<std::recursive_mutex> lock{vfsMutex};
		return ENOTCAPABLE;
	}
	__attribute__((export_name("wasi32_snapshot_preview1__fd_seek")))
	result_t wasi32_snapshot_preview1__fd_seek(uint32_t fd, int64_t delta, uint8_t whence, P32<uint64_t> newOffset) {
		std::lock_guard<std::recursive_mutex> lock{vfsMutex};
		auto &handle = getHandle(fd);
		if (!handle) return handle.error;
		if (handle->isDir) return EISDIR;
		
		if (whence == 1) {
			handle.position += delta;
		} else if (whence == 2) {
			handle.position = int64_t(handle->fileContents.size()) + delta;
		} else {
			handle.position = delta;
		}
		if (handle.position > handle->fileContents.size()) {
			handle.position = handle->fileContents.size();
		}
		newOffset.set(handle.position);
		return 0;
	}
	__attribute__((export_name("wasi32_snapshot_preview1__fd_sync")))
	result_t wasi32_snapshot_preview1__fd_sync(uint32_t fd) {
		std::lock_guard<std::recursive_mutex> lock{vfsMutex};
		return ENOTCAPABLE;
	}
	__attribute__((export_name("wasi32_snapshot_preview1__fd_tell")))
	result_t wasi32_snapshot_preview1__fd_tell(uint32_t fd, P32<uint64_t> offset) {
		std::lock_guard<std::recursive_mutex> lock{vfsMutex};
		auto &handle = getHandle(fd);
		if (!handle) return handle.error;
		offset.set(handle.position);
		return 0;
	}
	__attribute__((export_name("wasi32_snapshot_preview1__fd_write")))
	result_t wasi32_snapshot_preview1__fd_write(uint32_t fd, P32<const iovec32> ioBufferList, uint32_t ioBufferCount, P32<uint32_t> bytesWritten) {
		std::lock_guard<std::recursive_mutex> lock{vfsMutex};
		if (fd == 1) {
			size_t total = 0;
			forEachIoVecRead(ioBufferList, ioBufferCount, [&](const char *bytes, size_t length){
				for (size_t i = 0; i < length; ++i) {
					if (bytes[i] == '\n') {
						sendStdoutLine(stdoutLineBuffer.data(), stdoutLineBuffer.size());
						stdoutLineBuffer.resize(0);
					} else {
						stdoutLineBuffer.push_back(bytes[i]);
					}
				}
				total += length;
			});
			bytesWritten.set(uint32_t(total));
			return 0;
		} else if (fd == 2) {
			size_t total = 0;
			forEachIoVecRead(ioBufferList, ioBufferCount, [&](const char *bytes, size_t length){
				for (size_t i = 0; i < length; ++i) {
					if (bytes[i] == '\n') {
						sendStderrLine(stderrLineBuffer.data(), stderrLineBuffer.size());
						stderrLineBuffer.resize(0);
					} else {
						stderrLineBuffer.push_back(bytes[i]);
					}
				}
				total += length;
			});
			bytesWritten.set(uint32_t(total));
			return 0;
		}
		return ENOTCAPABLE; // TODO
	}
	__attribute__((export_name("wasi32_snapshot_preview1__path_create_directory")))
	result_t wasi32_snapshot_preview1__path_create_directory(uint32_t fd, P32<const char> path, uint32_t pathLength) {
		std::lock_guard<std::recursive_mutex> lock{vfsMutex};
		auto &dir = getHandle(fd);
		if (!dir) return dir.error;
		
		auto pathStr = getString(path, pathLength);
		for (auto c : pathStr) {
			if (c == '/' || c == '\0') return EINVAL; // could be more strict than this
		}
		
		auto node = dir->get(pathStr, false);
		if (node) return EEXIST;
		dir->get(pathStr, true);
		return 0;
	}
	__attribute__((export_name("wasi32_snapshot_preview1__path_filestat_get")))
	result_t wasi32_snapshot_preview1__path_filestat_get(uint32_t fd, uint32_t lookupFlags, P32<const char> path, uint32_t pathLength, P32<filestat> stat) {
		std::lock_guard<std::recursive_mutex> lock{vfsMutex};
		auto &dir = getHandle(fd);
		if (!dir) return dir.error;
		
		auto fileNode = vfsGet(getString(path, pathLength));
		if (!fileNode) return ENOENT;
		stat.set(fileNode->stat());
		return 0;
	}
	__attribute__((export_name("wasi32_snapshot_preview1__path_filestat_set_times")))
	result_t wasi32_snapshot_preview1__path_filestat_set_times(uint32_t fd, uint32_t lookupFlags, P32<const char> path, uint32_t pathLength, uint64_t aTime, uint64_t mTime, uint16_t flags) {
		std::lock_guard<std::recursive_mutex> lock{vfsMutex};
		auto fileNode = vfsGet(getString(path, pathLength));
		if (!fileNode) return ENOENT;
		auto &stat = fileNode->stat();
		if (flags&1) stat.aTime = aTime;
		if (flags&2) stat.mTime = mTime;
		return 0;
	}
	__attribute__((export_name("wasi32_snapshot_preview1__path_link")))
	result_t wasi32_snapshot_preview1__path_link(uint32_t oldFd, uint32_t oldLookupFlags, P32<const char> oldPath, uint32_t oldPathLength, uint32_t newFd, P32<const char> newPath, uint32_t newPathLength) {
		std::lock_guard<std::recursive_mutex> lock{vfsMutex};
		return ENOTCAPABLE; // no symlinks
	}
	__attribute__((export_name("wasi32_snapshot_preview1__path_open")))
	result_t wasi32_snapshot_preview1__path_open(uint32_t dirFd, uint32_t dirLookupFlags, P32<const char> path, uint32_t pathLength, uint16_t openFlags, uint64_t rightsBase, uint64_t rightsInheriting, uint16_t fsFlags, P32<uint32_t> newFd) {
		std::lock_guard<std::recursive_mutex> lock{vfsMutex};
		auto &dir = getHandle(dirFd);
		if (!dir) return dir.error;
		if (!dir.node->isDir) return EINVAL;
		
		fdstat stat{3/*file*/, fsFlags, rightsBase, rightsInheriting};

		auto pathStr = getString(path, pathLength);
		if (openFlags&1) { // create
			auto *fileNode = vfsGet(pathStr, true);
			newFd.set(vfsObtainFileHandle(*fileNode, stat));
			return 0;
		}
		auto *fileNode = vfsGet(pathStr, false);
		if (openFlags&2) {
			if (!fileNode) return ENOENT;
			if (!fileNode->isDir) return ENOTDIR;
		}
		if (openFlags&4) {
			if (fileNode) return EEXIST;
		} else {
			if (!fileNode) return ENOENT;
		}
		if (openFlags&8) {
			if (!fileNode) return ENOENT;
			if (fileNode->isDir) return EISDIR;
			fileNode->setSize(0);
		}
		stat.filetype = (fileNode->isDir ? 4 : 3);
		auto fd = vfsObtainFileHandle(*fileNode, stat);
		newFd.set(fd);
		if (fsFlags&1) { // append - seek to end
			auto &handle = getHandle(fd);
			if (handle) handle.position = handle->fileContents.size();
		}
		return 0;
	}
	__attribute__((export_name("wasi32_snapshot_preview1__path_readlink")))
	result_t wasi32_snapshot_preview1__path_readlink(uint32_t dirFd, P32<const char> path, uint32_t pathLength, P32<char> buffer, uint32_t bufferLength, P32<uint32_t> bytesUsed) {
		std::lock_guard<std::recursive_mutex> lock{vfsMutex};
		if (dirFd < 3) return EINVAL;
		return EINVAL; // no symlinks
	}
	__attribute__((export_name("wasi32_snapshot_preview1__path_remove_directory")))
	result_t wasi32_snapshot_preview1__path_remove_directory(uint32_t dirFd, P32<const char> path, uint32_t pathLength) {
		std::lock_guard<std::recursive_mutex> lock{vfsMutex};
		return ENOTCAPABLE;
	}
	__attribute__((export_name("wasi32_snapshot_preview1__path_rename")))
	result_t wasi32_snapshot_preview1__path_rename(uint32_t oldFd, P32<const char> oldPath, uint32_t oldPathLength, uint32_t newFd, P32<const char> newPath, uint32_t newPathLength) {
		std::lock_guard<std::recursive_mutex> lock{vfsMutex};
		return ENOTCAPABLE;
	}
	__attribute__((export_name("wasi32_snapshot_preview1__path_symlink")))
	result_t wasi32_snapshot_preview1__path_symlink(P32<const char> oldPath, uint32_t oldPathLength, uint32_t newFd, P32<const char> newPath, uint32_t newPathLength) {
		std::lock_guard<std::recursive_mutex> lock{vfsMutex};
		return ENOTCAPABLE;
	}
	__attribute__((export_name("wasi32_snapshot_preview1__path_unlink_file")))
	result_t wasi32_snapshot_preview1__path_unlink_file(uint32_t fd, P32<const char> path, uint32_t pathLength) {
		std::lock_guard<std::recursive_mutex> lock{vfsMutex};
		return ENOTCAPABLE;
	}
	__attribute__((export_name("wasi32_snapshot_preview1__poll_oneoff")))
	result_t wasi32_snapshot_preview1__poll_oneoff(P32<subscription32> subs, P32<event32> out, uint32_t subCount, P32<uint32_t> eventCount) {
		return ENOTCAPABLE;
	}
	__attribute__((export_name("wasi32_snapshot_preview1__proc_exit")))
	void wasi32_snapshot_preview1__proc_exit(uint32_t code) {
		procExit(code);
	}
	__attribute__((export_name("wasi32_snapshot_preview1__proc_raise")))
	result_t wasi32_snapshot_preview1__proc_raise(uint8_t signalType) {
		return ENOTCAPABLE;
	}
	__attribute__((export_name("wasi32_snapshot_preview1__random_get")))
	result_t wasi32_snapshot_preview1__random_get(P32<void> buffer, uint32_t length) {
		for (uint32_t offset = 0; offset < length; offset += 8) {
			uint64_t v64 = getRandom64();
			auto bytes = (length - offset);
			memcpyToOther32(buffer.remotePointer + offset, &v64, bytes);
		}
		return 0;
	}
	__attribute__((export_name("wasi32_snapshot_preview1__sched_yield")))
	result_t wasi32_snapshot_preview1__sched_yield() {
		return ENOTCAPABLE;
	}
	__attribute__((export_name("wasi32_snapshot_preview1__sock_accept")))
	result_t wasi32_snapshot_preview1__sock_accept(uint32_t sd, uint16_t flags, uint32_t fd) {
		return ENOTCAPABLE;
	}
	__attribute__((export_name("wasi32_snapshot_preview1__sock_recv")))
	result_t wasi32_snapshot_preview1__sock_recv(uint32_t sd, P32<const iovec32> riList, uint32_t riCount, uint16_t riFlags, P32<uint32_t> roDataLength, P32<uint16_t> roFlags) {
		return ENOTCAPABLE;
	}
	__attribute__((export_name("wasi32_snapshot_preview1__sock_send")))
	result_t wasi32_snapshot_preview1__sock_send(uint32_t sd, P32<const iovec32> dataList, uint32_t dataCount, uint16_t flags, P32<uint32_t> sentDataLength) {
		return ENOTCAPABLE;
	}
	__attribute__((export_name("wasi32_snapshot_preview1__sock_shutdown")))
	result_t wasi32_snapshot_preview1__sock_shutdown(uint32_t sd, uint8_t how) {
		return ENOTCAPABLE;
	}
}
