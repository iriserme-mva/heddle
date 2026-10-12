#include "weft/ring.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <new>
#include <thread>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace weft {

namespace {

constexpr uint64_t kMagic = 0x5446575200010001ull;  // "RWF T" v1
constexpr uint32_t kVersion = 1;
// Header occupies cache lines 0..2; data starts at offset 192 (a multiple of
// 64, so head/tail each get their own line and never share one with the data).
constexpr size_t kDataOffset = 192;

// Shared layout. Plain fields are written by the creator before `ready` is
// published and only read after an acquire load of `ready`, so they need no
// atomicity of their own. head/tail are the only things touched in the hot
// path.
struct alignas(64) RingHeader {
  uint64_t magic = kMagic;
  uint32_t version = kVersion;
  uint32_t capacityFrames = 0;  // power of two
  uint32_t numChannels = 0;
  uint32_t sampleRate = 0;
  std::atomic<uint32_t> ready{0};
  uint32_t _pad0 = 0;
  uint32_t _pad1 = 0;

  alignas(64) std::atomic<uint64_t> head{0};  // producer cursor (frames)
  uint8_t _pad2[56];

  alignas(64) std::atomic<uint64_t> tail{0};  // consumer cursor (frames)
  uint8_t _pad3[56];
};
static_assert(sizeof(RingHeader) == kDataOffset, "header must be exactly 192 bytes");

std::string sanitizeName(const std::string& name) {
  std::string out;
  out.reserve(name.size());
  for (unsigned char c : name) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
        c == '.' || c == '_' || c == '-')
      out.push_back(static_cast<char>(c));
    else
      out.push_back('_');
  }
  if (out.empty()) out = "ring";
  return out;
}

size_t roundUpPow2(size_t v) {
  size_t p = 2;
  while (p < v) p <<= 1;
  return p;
}

}  // namespace

struct AudioRing::Impl {
  std::string name;        // sanitized logical name
  std::string platformName;
  void* addr = nullptr;
  size_t size = 0;
  bool isWin32 = false;
  void* handle = nullptr;  // Windows: mapping HANDLE
  int fd = -1;             // POSIX: shm fd

  RingHeader* header() { return static_cast<RingHeader*>(addr); }
  float* data() { return reinterpret_cast<float*>(static_cast<uint8_t*>(addr) + kDataOffset); }

  ~Impl() {
#ifdef _WIN32
    if (addr) UnmapViewOfFile(addr);
    if (handle) CloseHandle(handle);
#else
    if (addr) munmap(addr, size);
    if (fd >= 0) ::close(fd);
#endif
  }
};

AudioRing::AudioRing() = default;
AudioRing::~AudioRing() { close(); }
AudioRing::AudioRing(AudioRing&& o) noexcept : impl_(std::exchange(o.impl_, nullptr)) {}
AudioRing& AudioRing::operator=(AudioRing&& o) noexcept {
  if (this != &o) {
    close();
    impl_ = std::exchange(o.impl_, nullptr);
  }
  return *this;
}

bool AudioRing::valid() const { return impl_ && impl_->addr; }

namespace {
const std::string& emptyName() {
  static const std::string empty;
  return empty;
}
}  // namespace

const std::string& AudioRing::name() const { return impl_ ? impl_->name : emptyName(); }

size_t AudioRing::capacityFrames() const { return valid() ? impl_->header()->capacityFrames : 0; }
int AudioRing::numChannels() const { return valid() ? static_cast<int>(impl_->header()->numChannels) : 0; }
int AudioRing::sampleRate() const { return valid() ? static_cast<int>(impl_->header()->sampleRate) : 0; }
size_t AudioRing::capacitySamples() const {
  return valid() ? capacityFrames() * static_cast<size_t>(numChannels()) : 0;
}

void AudioRing::close() { delete std::exchange(impl_, nullptr); }

namespace {

// Wait until the header's `ready` flag is set or the timeout elapses.
bool waitReady(RingHeader* h, int timeoutMs) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  for (;;) {
    if (h->ready.load(std::memory_order_acquire)) return true;
    if (std::chrono::steady_clock::now() >= deadline) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
}

bool validateHeader(const RingHeader* h, std::string& err) {
  if (h->magic != kMagic) {
    err = "not a weft audio ring (bad magic)";
    return false;
  }
  if (h->version != kVersion) {
    err = "weft audio ring version mismatch";
    return false;
  }
  const uint32_t cap = h->capacityFrames;
  if (cap < 2 || (cap & (cap - 1)) != 0) {
    err = "corrupt ring header: capacity not a power of two";
    return false;
  }
  if (h->numChannels < 1) {
    err = "corrupt ring header: invalid channel count";
    return false;
  }
  const uint64_t head = h->head.load(std::memory_order_acquire);
  const uint64_t tail = h->tail.load(std::memory_order_acquire);
  if (tail > head || head - tail > cap) {
    err = "corrupt ring header: cursor order invalid";
    return false;
  }
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// create
// ---------------------------------------------------------------------------
AudioRing AudioRing::create(const std::string& name, size_t capacityFrames, int numChannels,
                            int sampleRate, std::string* err) {
  AudioRing ring;
  if (name.empty()) {
    if (err) *err = "ring name must not be empty";
    return ring;
  }
  if (numChannels < 1 || numChannels > 64) {
    if (err) *err = "numChannels out of range [1,64]";
    return ring;
  }
  if (capacityFrames < 2) {
    if (err) *err = "capacityFrames must be >= 2";
    return ring;
  }
  // Fail before touching any shared memory: a ring whose cursors aren't
  // lock-free would corrupt SPSC ordering for the other process.
  {
    std::atomic<uint64_t> probe;
    if (!probe.is_lock_free()) {
      if (err) *err = "std::atomic<uint64_t> is not lock-free on this platform";
      return ring;
    }
  }
  const size_t cap = roundUpPow2(capacityFrames);
  const size_t dataBytes = cap * static_cast<size_t>(numChannels) * sizeof(float);
  const size_t total = kDataOffset + dataBytes;

  auto impl = std::make_unique<Impl>();
  impl->name = sanitizeName(name);

#ifdef _WIN32
  impl->isWin32 = true;
  // Plain (session) namespace name: Weft and the Ableton bridge run in the
  // same user session, and leading-backslash ("global") forms are rejected
  // with ERROR_INVALID_PARAMETER by CreateFileMappingW on this platform.
  const std::string flat = "weft.ring." + impl->name;
  impl->platformName = flat;
  const int wlen = ::MultiByteToWideChar(CP_UTF8, 0, flat.c_str(), -1, nullptr, 0);
  std::wstring wname;
  if (wlen > 0) {
    wname.resize(wlen);
    ::MultiByteToWideChar(CP_UTF8, 0, flat.c_str(), -1, wname.data(), wlen);
  }
  impl->handle = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                      static_cast<DWORD>(total >> 32), static_cast<DWORD>(total & 0xffffffff),
                                      wname.c_str());
  if (!impl->handle) {
    if (err) *err = "CreateFileMapping failed (error " + std::to_string(::GetLastError()) + ")";
    return AudioRing();
  }
  // On success CreateFileMappingW returns a handle to the EXISTING object and
  // sets the last error to ERROR_ALREADY_EXISTS; a fresh object leaves it clear.
  {
    const DWORD gl = ::GetLastError();
    if (gl == ERROR_ALREADY_EXISTS) {
      ::CloseHandle(impl->handle);
      impl->handle = nullptr;
      if (err) *err = "a ring named '" + impl->name + "' already exists";
      return AudioRing();
    }
  }
  impl->addr = ::MapViewOfFile(impl->handle, FILE_MAP_ALL_ACCESS, 0, 0, total);
  if (!impl->addr) {
    if (err) *err = "MapViewOfFile failed (error " + std::to_string(GetLastError()) + ")";
    return AudioRing();
  }
#else
  impl->platformName = "/weft.ring." + impl->name;
  if (impl->platformName.size() > 120) impl->platformName.resize(120);
  impl->fd = ::shm_open(impl->platformName.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
  if (impl->fd < 0) {
    if (errno == EEXIST) {
      ::shm_unlink(impl->platformName.c_str());
      if (err) *err = "a ring named '" + impl->name + "' already exists";
    } else {
      if (err) *err = "shm_open failed: " + std::string(std::strerror(errno));
    }
    return AudioRing();
  }
  if (::ftruncate(impl->fd, static_cast<off_t>(total)) != 0) {
    if (err) *err = "ftruncate failed: " + std::string(std::strerror(errno));
    ::shm_unlink(impl->platformName.c_str());
    return AudioRing();
  }
  impl->addr = ::mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_SHARED, impl->fd, 0);
  if (impl->addr == MAP_FAILED) {
    impl->addr = nullptr;
    if (err) *err = "mmap failed: " + std::string(std::strerror(errno));
    ::shm_unlink(impl->platformName.c_str());
    return AudioRing();
  }
#endif

  impl->size = total;
  RingHeader* h = new (impl->addr) RingHeader();
  h->capacityFrames = static_cast<uint32_t>(cap);
  h->numChannels = static_cast<uint32_t>(numChannels);
  h->sampleRate = static_cast<uint32_t>(sampleRate);
  h->head.store(0, std::memory_order_relaxed);
  h->tail.store(0, std::memory_order_relaxed);
  h->ready.store(1, std::memory_order_release);

  ring.impl_ = impl.release();
  return ring;
}

// ---------------------------------------------------------------------------
// open
// ---------------------------------------------------------------------------
AudioRing AudioRing::open(const std::string& name, std::string* err, int timeoutMs) {
  AudioRing ring;
  if (name.empty()) {
    if (err) *err = "ring name must not be empty";
    return ring;
  }
  auto impl = std::make_unique<Impl>();
  impl->name = sanitizeName(name);

  size_t total = 0;
#ifdef _WIN32
  impl->isWin32 = true;
  const std::string flat = "weft.ring." + impl->name;
  const int wlen = ::MultiByteToWideChar(CP_UTF8, 0, flat.c_str(), -1, nullptr, 0);
  std::wstring wname;
  if (wlen > 0) {
    wname.resize(wlen);
    ::MultiByteToWideChar(CP_UTF8, 0, flat.c_str(), -1, wname.data(), wlen);
  }
  impl->platformName = flat;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  for (;;) {
    impl->handle = ::OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, wname.c_str());
    if (impl->handle) break;
    const DWORD gl = GetLastError();
    if (gl != ERROR_FILE_NOT_FOUND) {
      if (err) *err = "OpenFileMapping failed (error " + std::to_string(gl) + ")";
      return AudioRing();
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      if (err) *err = "no ring named '" + impl->name + "' found (waited " + std::to_string(timeoutMs) + " ms)";
      return AudioRing();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  // Map the header to learn the size (creator sized the mapping up front,
  // but we still don't know the capacity here), then re-map with full access.
  void* probe = ::MapViewOfFile(impl->handle, FILE_MAP_READ, 0, 0, 0);
  if (!probe) {
    if (err) *err = "MapViewOfFile failed (error " + std::to_string(GetLastError()) + ")";
    return AudioRing();
  }
  auto* ph = static_cast<RingHeader*>(probe);
  if (!waitReady(ph, timeoutMs)) {
    if (err) *err = "ring '" + impl->name + "' not ready in time";
    ::UnmapViewOfFile(probe);
    return AudioRing();
  }
  total = kDataOffset + static_cast<size_t>(ph->capacityFrames) * ph->numChannels * sizeof(float);
  ::UnmapViewOfFile(probe);
  impl->addr = ::MapViewOfFile(impl->handle, FILE_MAP_ALL_ACCESS, 0, 0, total);
  if (!impl->addr) {
    if (err) *err = "MapViewOfFile failed (error " + std::to_string(GetLastError()) + ")";
    return AudioRing();
  }
#else
  impl->platformName = "/weft.ring." + impl->name;
  if (impl->platformName.size() > 120) impl->platformName.resize(120);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  for (;;) {
    impl->fd = ::shm_open(impl->platformName.c_str(), O_RDWR);
    if (impl->fd >= 0) break;
    if (errno != ENOENT) {
      if (err) *err = "shm_open failed: " + std::string(std::strerror(errno));
      return AudioRing();
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      if (err) *err = "no ring named '" + impl->name + "' found (waited " + std::to_string(timeoutMs) + " ms)";
      return AudioRing();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  // The creator ftruncates the object after creating it; wait until it is at
  // least header-sized so the probe map below cannot race a 0-byte object.
  const auto sizedDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  for (;;) {
    struct stat st {};
    if (::fstat(impl->fd, &st) == 0 && (size_t)st.st_size >= kDataOffset) break;
    if (std::chrono::steady_clock::now() >= sizedDeadline) {
      if (err) *err = "ring '" + impl->name + "' not sized in time";
      return AudioRing();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  // First read the header to learn the size.
  void* p = ::mmap(nullptr, kDataOffset, PROT_READ, MAP_SHARED, impl->fd, 0);
  if (p == MAP_FAILED) {
    if (err) *err = "mmap failed: " + std::string(std::strerror(errno));
    return AudioRing();
  }
  auto* h = static_cast<RingHeader*>(p);
  if (!waitReady(h, timeoutMs)) {
    if (err) *err = "ring '" + impl->name + "' not ready in time";
    ::munmap(p, kDataOffset);
    return AudioRing();
  }
  total = kDataOffset + static_cast<size_t>(h->capacityFrames) * h->numChannels * sizeof(float);
  ::munmap(p, kDataOffset);
  impl->addr = ::mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_SHARED, impl->fd, 0);
  if (impl->addr == MAP_FAILED) {
    impl->addr = nullptr;
    if (err) *err = "mmap failed: " + std::string(std::strerror(errno));
    return AudioRing();
  }
#endif
  impl->size = total;
  std::string verr;
  if (!validateHeader(impl->header(), verr)) {
    if (err) *err = verr;
    return AudioRing();
  }
  ring.impl_ = impl.release();
  return ring;
}

bool AudioRing::exists(const std::string& name) {
  if (name.empty()) return false;
  const std::string s = sanitizeName(name);
#ifdef _WIN32
  const std::string flat = "weft.ring." + s;
  const int wlen = ::MultiByteToWideChar(CP_UTF8, 0, flat.c_str(), -1, nullptr, 0);
  std::wstring wname;
  if (wlen > 0) {
    wname.resize(wlen);
    ::MultiByteToWideChar(CP_UTF8, 0, flat.c_str(), -1, wname.data(), wlen);
  }
  HANDLE h = ::OpenFileMappingW(FILE_MAP_READ, FALSE, wname.c_str());
  if (h) {
    ::CloseHandle(h);
    return true;
  }
  return false;
#else
  std::string p = "/weft.ring." + s;
  if (p.size() > 120) p.resize(120);
  int fd = ::shm_open(p.c_str(), O_RDWR);
  if (fd >= 0) {
    ::close(fd);
    return true;
  }
  return false;
#endif
}

void AudioRing::remove(const std::string& name) {
  if (name.empty()) return;
  const std::string s = sanitizeName(name);
#ifndef _WIN32
  // POSIX: unlink removes the name; any process that already mapped it keeps
  // a valid view until it unmaps.
  std::string p = "/weft.ring." + s;
  if (p.size() > 120) p.resize(120);
  ::shm_unlink(p.c_str());
#else
  // Windows: a named mapping is ref-counted and cannot be deleted by name;
  // it is destroyed when the last handle to it is closed. No-op.
  (void)s;
#endif
}

// ---------------------------------------------------------------------------
// hot path
// ---------------------------------------------------------------------------
size_t AudioRing::write(const float* src, size_t frames) {
  if (!valid() || !src || frames == 0) return 0;
  RingHeader* h = impl_->header();
  const uint64_t cap = h->capacityFrames;
  const uint64_t nch = h->numChannels;
  const uint64_t mask = cap - 1;
  const uint64_t head = h->head.load(std::memory_order_relaxed);
  const uint64_t tail = h->tail.load(std::memory_order_acquire);
  const uint64_t space = cap - (head - tail);
  if (space == 0) return 0;
  float* data = impl_->data();
  uint64_t done = 0;
  while (done < frames && done < space) {
    const uint64_t pos = (head + done) & mask;
    const uint64_t run = cap - pos;  // consecutive frames until wrap
    uint64_t take = frames - done;
    if (take > run) take = run;
    if (take > space - done) take = space - done;
    ::memcpy(data + pos * nch, src + done * nch, take * nch * sizeof(float));
    done += take;
  }
  if (done) h->head.store(head + done, std::memory_order_release);
  return static_cast<size_t>(done);
}

size_t AudioRing::read(float* dst, size_t frames) {
  if (!valid() || !dst || frames == 0) return 0;
  RingHeader* h = impl_->header();
  const uint64_t cap = h->capacityFrames;
  const uint64_t nch = h->numChannels;
  const uint64_t mask = cap - 1;
  const uint64_t tail = h->tail.load(std::memory_order_relaxed);
  const uint64_t head = h->head.load(std::memory_order_acquire);
  const uint64_t avail = head - tail;
  if (avail == 0) return 0;
  const float* data = impl_->data();
  uint64_t done = 0;
  while (done < frames && done < avail) {
    const uint64_t pos = (tail + done) & mask;
    const uint64_t run = cap - pos;
    uint64_t take = frames - done;
    if (take > run) take = run;
    if (take > avail - done) take = avail - done;
    ::memcpy(dst + done * nch, data + pos * nch, take * nch * sizeof(float));
    done += take;
  }
  if (done) h->tail.store(tail + done, std::memory_order_release);
  return static_cast<size_t>(done);
}

size_t AudioRing::spaceAvailable() const {
  if (!valid()) return 0;
  const RingHeader* h = impl_->header();
  const uint64_t cap = h->capacityFrames;
  const uint64_t head = h->head.load(std::memory_order_acquire);
  const uint64_t tail = h->tail.load(std::memory_order_acquire);
  return static_cast<size_t>(cap - (head - tail));
}

size_t AudioRing::dataAvailable() const {
  if (!valid()) return 0;
  const RingHeader* h = impl_->header();
  const uint64_t head = h->head.load(std::memory_order_acquire);
  const uint64_t tail = h->tail.load(std::memory_order_acquire);
  return static_cast<size_t>(head - tail);
}

}  // namespace weft
