// Tests for the lock-free shared-memory SPSC audio ring: FIFO order,
// wraparound, partial read/write, capacity rounding, error paths, in-process
// thread stress, and real cross-process tests (the test binary relaunches
// itself as a child producer/consumer over the same named ring).
#include "doctest.h"

#include "weft/ring.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace weft;
using namespace std::chrono_literals;

namespace {

std::string testName(const char* base) {
  // Per-run suffix so leftovers from other runs never collide.
  return std::string(base) + "_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
}

// Deterministic pattern: global frame g = startFrame + i, channel c -> (g*31 + c).
void patternFill(float* dst, int64_t startFrame, size_t frames, int ch) {
  for (size_t i = 0; i < frames; ++i)
    for (int c = 0; c < ch; ++c)
      dst[i * ch + c] =
          static_cast<float>((startFrame + static_cast<int64_t>(i)) * 31 + c);
}

bool patternCheck(const float* src, int64_t startFrame, size_t frames, int ch) {
  for (size_t i = 0; i < frames; ++i)
    for (int c = 0; c < ch; ++c)
      if (src[i * ch + c] !=
          static_cast<float>((startFrame + static_cast<int64_t>(i)) * 31 + c))
        return false;
  return true;
}

std::string selfPath() {
#ifdef _WIN32
  char buf[1024] = {0};
  ::GetModuleFileNameA(nullptr, buf, 1023);
  return buf;
#else
  char buf[1024] = {0};
  ssize_t n = ::readlink("/proc/self/exe", buf, 1023);
  return (n > 0) ? std::string(buf, static_cast<size_t>(n)) : std::string();
#endif
}

#ifdef _WIN32
struct ChildProc {
  HANDLE process = nullptr;
  HANDLE thread = nullptr;
  bool valid() const { return process; }
  ~ChildProc() {
    if (process) ::CloseHandle(process);
    if (thread) ::CloseHandle(thread);
  }
  int wait() const {
    ::WaitForSingleObject(process, INFINITE);
    DWORD code = 0;
    ::GetExitCodeProcess(process, &code);
    return static_cast<int>(code);
  }
};
#else
struct ChildProc {
  pid_t pid = -1;
  bool valid() const { return pid > 0; }
  int wait() const {
    int status = 0;
    if (::waitpid(pid, &status, 0) < 0) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  }
};
#endif

// Spawn self in --ring-child mode with the given role args, without waiting.
// The --ring-child flag is always prepended so the child runs only
// ringChildMain (never the full doctest suite) — preventing a fork bomb where
// each child would otherwise re-run the cross-process tests and spawn more.
// Returns an invalid handle on spawn error.
ChildProc spawnChild(const std::vector<std::string>& args) {
  const std::string exe = selfPath();
  ChildProc cp;
#ifdef _WIN32
  std::string cmd = "\"" + exe + "\" --ring-child";
  for (const auto& a : args) cmd += " \"" + a + "\"";
  std::vector<char> cmdBuf(cmd.begin(), cmd.end());
  cmdBuf.push_back(0);
  STARTUPINFOA si {};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi {};
  if (!::CreateProcessA(exe.c_str(), cmdBuf.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                        &si, &pi))
    return ChildProc {};
  cp.process = pi.hProcess;
  cp.thread = pi.hThread;
#else
  pid_t pid = ::fork();
  if (pid < 0) return ChildProc {};
  if (pid == 0) {
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(exe.c_str()));
    argv.push_back(const_cast<char*>("--ring-child"));
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    ::execv(exe.c_str(), argv.data());
    _exit(127);
  }
  cp.pid = pid;
#endif
  return cp;
}

// Launch self with args, wait for it, return its exit code (-1 on spawn error).
int launchChild(const std::vector<std::string>& args) {
  ChildProc cp = spawnChild(args);
  if (!cp.valid()) return -1;
  return cp.wait();
}

// Cross-process child roles:
//   producer: open ring, write `frames` pattern frames in <=64-frame chunks.
//   consumer: open ring, read `frames` pattern frames within 15 s, verify order.
// Exit 0 on success, nonzero on failure.
int ringChildMain(int argc, char** argv) {
  // argv layout (spawnChild always prepends --ring-child):
  //   [0] exe  [1] --ring-child  [2] name  [3] frames  [4] ch  [5] role
  if (argc < 6) return 2;
  const std::string name = argv[2];
  const size_t frames = static_cast<size_t>(std::atol(argv[3]));
  const int ch = std::atoi(argv[4]);
  const bool producer = (std::string(argv[5]) == "producer");

  std::string err;
  AudioRing ring = AudioRing::open(name, &err, 10000);
  if (!ring.valid()) {
    std::fprintf(stderr, "ring child: open failed: %s\n", err.c_str());
    return 2;
  }
  std::vector<float> buf(64 * ch);
  const auto deadline = std::chrono::steady_clock::now() + 15s;
  int64_t done = 0;
  bool ok = true;
  while (done < static_cast<int64_t>(frames) && std::chrono::steady_clock::now() < deadline) {
    size_t want = static_cast<size_t>(frames - done);
    if (want > 64) want = 64;
    if (producer) {
      patternFill(buf.data(), done, want, ch);
      size_t got = ring.write(buf.data(), want);
      if (got) done += static_cast<int64_t>(got);
      else std::this_thread::sleep_for(1ms);
    } else {
      size_t got = ring.read(buf.data(), want);
      if (got) {
        if (!patternCheck(buf.data(), done, got, ch)) {
          std::fprintf(stderr, "ring child: pattern mismatch at frame %lld\n",
                       static_cast<long long>(done));
          ok = false;
          break;
        }
        done += static_cast<int64_t>(got);
      } else {
        std::this_thread::sleep_for(1ms);
      }
    }
  }
  if (!ok || done < static_cast<int64_t>(frames)) {
    std::fprintf(stderr, "ring child: %s only %lld of %zu frames\n",
                 producer ? "wrote" : "read", static_cast<long long>(done), frames);
    return 1;
  }
  return 0;
}

}  // namespace

// Child-mode dispatch runs before doctest sees its own arguments.
int main(int argc, char** argv) {
  if (argc >= 2 && std::string(argv[1]) == "--ring-child")
    return ringChildMain(argc, argv);
  doctest::Context ctx;
  return ctx.run();
}

namespace {

TEST_CASE("ring: invalid create parameters") {
  std::string err;
  CHECK(AudioRing::create("", 64, 2, 48000, &err).valid() == false);
  CHECK(!err.empty());
  err.clear();
  CHECK(AudioRing::create("cap_" + testName("x"), 1, 2, 48000, &err).valid() == false);
  CHECK(!err.empty());
  err.clear();
  CHECK(AudioRing::create("ch_" + testName("x"), 64, 0, 48000, &err).valid() == false);
  CHECK(!err.empty());
}

TEST_CASE("ring: capacity rounding and header fields") {
  AudioRing r = AudioRing::create("cap_" + testName("x"), 7, 2, 48000);  // 7 -> 8
  REQUIRE(r.valid());
  CHECK(r.capacityFrames() == 8);
  CHECK(r.numChannels() == 2);
  CHECK(r.sampleRate() == 48000);
  CHECK(r.capacitySamples() == 16);
  CHECK(AudioRing::exists(r.name()));
  AudioRing r4 = AudioRing::create("cap4_" + testName("x"), 1024, 4, 44100);
  REQUIRE(r4.valid());
  CHECK(r4.capacityFrames() == 1024);
  CHECK(r4.numChannels() == 4);
}

TEST_CASE("ring: duplicate create fails") {
  const std::string name = "dup_" + testName("x");
  AudioRing a = AudioRing::create(name, 64, 2, 48000);
  REQUIRE(a.valid());
  std::string err;
  AudioRing b = AudioRing::create(name, 64, 2, 48000, &err);
  CHECK(b.valid() == false);
  CHECK(!err.empty());
}

TEST_CASE("ring: empty read / null args are no-ops") {
  AudioRing r = AudioRing::create("noop_" + testName("x"), 64, 2, 48000);
  REQUIRE(r.valid());
  float buf[8];
  CHECK(r.read(buf, 4) == 0);           // empty
  CHECK(r.write(nullptr, 4) == 0);      // null src
  CHECK(r.read(nullptr, 4) == 0);       // null dst
  CHECK(r.write(buf, 0) == 0);
  CHECK(r.read(buf, 0) == 0);
  AudioRing invalid;
  CHECK(invalid.write(buf, 4) == 0);
  CHECK(invalid.read(buf, 4) == 0);
  CHECK(invalid.dataAvailable() == 0);
  CHECK(invalid.spaceAvailable() == 0);
}

TEST_CASE("ring: chunked FIFO with full drain each chunk") {
  const size_t cap = 8, ch = 2;
  AudioRing r = AudioRing::create("fifo_" + testName("x"), cap, ch, 48000);
  REQUIRE(r.valid());
  const size_t total = 1000;
  std::vector<float> in(16 * ch), out(16 * ch);
  for (size_t i = 0; i < total; i += 3) {  // 3-frame chunks, cap 8: forces wrap
    size_t want = (total - i) < 3 ? (total - i) : 3;
    patternFill(in.data(), static_cast<int64_t>(i), want, ch);
    REQUIRE(r.write(in.data(), want) == want);
    // Ring was empty before this chunk, so drain exactly `want` frames.
    size_t gotTotal = 0;
    while (gotTotal < want) {
      size_t got = r.read(out.data(), want - gotTotal);
      REQUIRE(got > 0);
      gotTotal += got;
    }
    CHECK(patternCheck(out.data(), static_cast<int64_t>(i), want, ch));
  }
  CHECK(r.dataAvailable() == 0);
}

TEST_CASE("ring: exact sequence preserved under backpressure") {
  const size_t cap = 8, ch = 2, total = 1000;
  AudioRing r = AudioRing::create("seq_" + testName("x"), cap, ch, 48000);
  REQUIRE(r.valid());
  std::vector<float> in(16 * ch), out(16 * ch);
  int64_t verified = 0;  // global position of the next unverified frame
  for (size_t i = 0; i < total; i += 3) {
    size_t want = (total - i) < 3 ? (total - i) : 3;
    patternFill(in.data(), static_cast<int64_t>(i), want, ch);
    size_t written = 0;
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (written < want) {
      if (r.dataAvailable() >= cap) {  // full: drain and verify
        size_t avail = r.dataAvailable();
        size_t got = r.read(out.data(), avail);
        REQUIRE(got == avail);
        CHECK(patternCheck(out.data(), verified, got, ch));
        verified += static_cast<int64_t>(got);
        std::this_thread::sleep_for(50us);
        continue;
      }
      size_t got = r.write(in.data() + written * ch, want - written);
      if (got) {
        written += got;
      } else if (std::chrono::steady_clock::now() > deadline) {
        REQUIRE(false);
      } else {
        std::this_thread::sleep_for(10us);
      }
    }
  }
  // Final drain: everything else must come out in order.
  bool ok = true;
  while (verified < static_cast<int64_t>(total)) {
    size_t got = r.read(out.data(), 16);
    if (got == 0) break;
    if (!patternCheck(out.data(), verified, got, ch)) {
      ok = false;
      break;
    }
    verified += static_cast<int64_t>(got);
  }
  CHECK(ok);
  CHECK(verified == static_cast<int64_t>(total));
  CHECK(r.dataAvailable() == 0);
}

TEST_CASE("ring: write returns 0 when full") {
  const size_t cap = 4, ch = 1;
  AudioRing r = AudioRing::create("full_" + testName("x"), cap, ch, 48000);
  REQUIRE(r.valid());
  std::vector<float> in(8), out(8);
  patternFill(in.data(), 0, cap, ch);
  CHECK(r.write(in.data(), cap) == cap);
  CHECK(r.spaceAvailable() == 0);
  patternFill(in.data(), 100, 2, ch);
  CHECK(r.write(in.data(), 2) == 0);  // full
  float one = 0;
  CHECK(r.read(&one, 1) == 1);
  CHECK(one == 0.f);
  CHECK(r.spaceAvailable() == 1);
}

TEST_CASE("ring: partial write into nearly-full ring") {
  const size_t cap = 4, ch = 1;
  AudioRing r = AudioRing::create("partial_" + testName("x"), cap, ch, 48000);
  REQUIRE(r.valid());
  std::vector<float> in(4), out(4);
  patternFill(in.data(), 0, 3, ch);
  CHECK(r.write(in.data(), 3) == 3);
  patternFill(in.data(), 50, 2, ch);
  CHECK(r.write(in.data(), 2) == 1);  // only 1 slot left
  CHECK(r.dataAvailable() == cap);
  // Mid-drain frees all four slots. Re-fill with a 3-frame chunk and push one
  // more partial frame so the final write lands on the wrap boundary.
  std::vector<float> out2(cap * ch);
  CHECK(r.read(out2.data(), cap) == cap);  // drain, free all 4 slots
  patternFill(in.data(), 0, 3, ch);
  CHECK(r.write(in.data(), 3) == 3);
  patternFill(in.data(), 900, 2, ch);
  CHECK(r.write(in.data(), 2) == 1);  // only 1 slot left, before full
  CHECK(r.dataAvailable() == cap);
  // Full FIFO order must come out: the 3-frame chunk (global frames 0,1,2)
  // followed by the single partial frame (global frame 900 -> 900*31).
  std::vector<float> drain(cap);
  CHECK(r.read(drain.data(), cap) == cap);
  CHECK(drain[0] == 0.f);       // global frame 0
  CHECK(drain[1] == 31.f);      // global frame 1
  CHECK(drain[2] == 62.f);      // global frame 2
  CHECK(drain[3] == 27900.f);   // global frame 900 (900*31 + 0)
  CHECK(r.dataAvailable() == 0);
}

TEST_CASE("ring: exists/remove lifecycle") {
  const std::string name = "rm_" + testName("x");
  CHECK(AudioRing::exists(name) == false);
  AudioRing r = AudioRing::create(name, 16, 1, 48000);
  REQUIRE(r.valid());
  CHECK(AudioRing::exists(name) == true);
  r.close();
#ifdef _WIN32
  // All handles closed -> kernel object destroyed.
  CHECK(AudioRing::exists(name) == false);
#else
  CHECK(AudioRing::exists(name) == true);  // POSIX object persists after close
  AudioRing::remove(name);
  CHECK(AudioRing::exists(name) == false);
  AudioRing r2 = AudioRing::create(name, 16, 1, 48000);  // re-create works
  CHECK(r2.valid());
#endif
}

TEST_CASE("ring: move semantics") {
  AudioRing a = AudioRing::create("mv_" + testName("x"), 16, 2, 48000);
  REQUIRE(a.valid());
  AudioRing b = std::move(a);
  CHECK(b.valid());
  CHECK(a.valid() == false);
  AudioRing c;
  c = std::move(b);
  CHECK(c.valid());
  CHECK(b.valid() == false);
  float buf[4];
  CHECK(c.write(buf, 2) == 2);
  CHECK(c.read(buf, 2) == 2);
}

TEST_CASE("ring: open of nonexistent name times out") {
  std::string err;
  AudioRing r = AudioRing::open("nope_" + testName("x"), &err, 200);
  CHECK(r.valid() == false);
  CHECK(!err.empty());
}

TEST_CASE("ring: thread stress (in-process, 200k frames)") {
  const size_t cap = 32, ch = 2, total = 200000;
  AudioRing r = AudioRing::create("stress_" + testName("x"), cap, ch, 48000);
  REQUIRE(r.valid());
  std::atomic<bool> done{false};
  std::thread producer([&] {
    std::vector<float> in(64 * ch);
    int64_t i = 0;
    const auto deadline = std::chrono::steady_clock::now() + 60s;
    while (i < static_cast<int64_t>(total)) {
      size_t want = static_cast<size_t>(total - i);
      if (want > 64) want = 64;
      patternFill(in.data(), i, want, ch);
      size_t got = r.write(in.data(), want);
      if (got) {
        i += static_cast<int64_t>(got);
      } else {
        std::this_thread::yield();
        if (std::chrono::steady_clock::now() > deadline) return;
      }
    }
    done.store(true);
  });
  std::vector<float> out(64 * ch);
  int64_t pos = 0;
  const auto deadline = std::chrono::steady_clock::now() + 60s;
  bool ok = true;
  while (pos < static_cast<int64_t>(total)) {
    size_t want = static_cast<size_t>(total - pos);
    if (want > 64) want = 64;
    size_t got = r.read(out.data(), want);
    if (got) {
      if (!patternCheck(out.data(), pos, got, ch)) {
        ok = false;
        break;
      }
      pos += static_cast<int64_t>(got);
    } else if (!done.load() && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::yield();
    } else {
      break;
    }
  }
  producer.join();
  CHECK(ok);
  CHECK(done.load());
  CHECK(pos == static_cast<int64_t>(total));
  CHECK(r.dataAvailable() == 0);
}

TEST_CASE("ring: cross-process, child producer / parent consumer") {
  const std::string name = "xproc_" + testName("x");
  const size_t ch = 2, frames = 5000;
  AudioRing r = AudioRing::create(name, 256, ch, 48000);
  REQUIRE(r.valid());
  REQUIRE(selfPath().size() > 0);
  const int rc = launchChild({name, std::to_string(frames), std::to_string(ch), "producer"});
  REQUIRE(rc == 0);
  std::vector<float> out(64 * ch);
  int64_t pos = 0;
  const auto deadline = std::chrono::steady_clock::now() + 15s;
  while (pos < static_cast<int64_t>(frames)) {
    size_t want = static_cast<size_t>(frames - pos);
    if (want > 64) want = 64;
    size_t got = r.read(out.data(), want);
    if (got) {
      CHECK(patternCheck(out.data(), pos, got, ch));
      pos += static_cast<int64_t>(got);
    } else {
      if (std::chrono::steady_clock::now() > deadline) break;
      std::this_thread::sleep_for(1ms);
    }
  }
  CHECK(pos == static_cast<int64_t>(frames));
}

TEST_CASE("ring: cross-process, parent producer / child consumer") {
  const std::string name = "xproc2_" + testName("x");
  const size_t ch = 2, frames = 5000;
  AudioRing r = AudioRing::create(name, 256, ch, 48000);
  REQUIRE(r.valid());
  REQUIRE(selfPath().size() > 0);
  // Start the consumer (it blocks inside open() until the ring is ready and
  // keeps draining until it has read all `frames`), then feed it.
  ChildProc child = spawnChild({name, std::to_string(frames), std::to_string(ch), "consumer"});
  REQUIRE(child.valid());
  std::vector<float> in(64 * ch);
  int64_t i = 0;
  const auto deadline = std::chrono::steady_clock::now() + 15s;
  while (i < static_cast<int64_t>(frames)) {
    size_t want = static_cast<size_t>(frames - i);
    if (want > 64) want = 64;
    patternFill(in.data(), i, want, ch);
    size_t got = r.write(in.data(), want);
    if (got) {
      i += static_cast<int64_t>(got);
    } else {
      if (std::chrono::steady_clock::now() > deadline) break;
      std::this_thread::sleep_for(1ms);
    }
  }
  CHECK(i == static_cast<int64_t>(frames));
  // The child exits 0 only if it read all frames back in exact pattern order.
  CHECK(child.wait() == 0);
}

}  // namespace
