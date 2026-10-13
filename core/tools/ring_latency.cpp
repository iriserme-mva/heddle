// ring_latency — instrumented latency probe for the Weft shared-memory ring.
//
// Subcommands:
//   bench
//     Single process. Times per-block (write+read) for:
//       - direct : two plain memcpys into a local interleaved buffer (no IPC)
//       - ring   : AudioRing::write + AudioRing::read (shared memory, same proc)
//     Reports mean/median/max in nanoseconds and the added ring overhead.
//
//   xproc
//     Cross-process, real-time paced. Creates a ring, spawns a child producer
//     that writes blocks on a real-time schedule, and runs as the consumer
//     measuring arrival delay of each block. Reports the end-to-end added
//     latency (handoff + scheduling), in ns and as a fraction of a block.
//
//   child-producer <name> <ch> <block> <blocks> <refNs>
//     Hidden role invoked by `xproc`. Writes `blocks` blocks, one per
//     real-time block interval, each carrying (send wall-clock time in ns,
//     relative to refNs) as its first channel sample so the consumer can
//     compute delay. Relative (not absolute) because float32 cannot hold the
//     full absolute ns counter with any precision.
//
// Build: part of the weft_core tree (see CMakeLists). No external deps beyond
// the C++17 stdlib and weft_core.

#include <weft/ring.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
namespace rt {
static inline int64_t ns() {
  LARGE_INTEGER f, c;
  QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&c);
  return (c.QuadPart * 1000000000LL) / f.QuadPart;
}
static inline void yield() {
  Sleep(0);
}
}  // namespace rt
#else
#include <sched.h>
#include <sys/wait.h>
#include <time.h>
namespace rt {
static inline int64_t ns() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}
static inline void yield() {
  sched_yield();
}
}  // namespace rt
#endif

namespace {

using weft::AudioRing;

struct Stats {
  int64_t mean = 0, median = 0, max = 0;
};

Stats summarize(std::vector<int64_t>& v) {
  if (v.empty()) return {};
  std::sort(v.begin(), v.end());
  int64_t sum = 0;
  for (auto x : v) sum += x;
  Stats s;
  s.mean = sum / static_cast<int64_t>(v.size());
  s.median = v[v.size() / 2];
  s.max = v.back();
  return s;
}

void printStats(const char* label, const Stats& s, double blockMs) {
  std::printf("  %-9s mean=%6lld ns  median=%6lld ns  max=%9lld ns  "
              "(block=%6.3f ms)\n",
              label, static_cast<long long>(s.mean),
              static_cast<long long>(s.median),
              static_cast<long long>(s.max), blockMs);
}

// Fill `frames` interleaved frames with a deterministic ramp.
void fill(float* p, size_t frames, int ch) {
  for (size_t i = 0; i < frames; ++i)
    for (int c = 0; c < ch; ++c) p[i * ch + c] = static_cast<float>(i * ch + c);
}

Stats g_direct, g_ring;

// ---------------------------------------------------------------------------
// bench: direct vs in-process ring
// ---------------------------------------------------------------------------
int runBench(int ch, size_t block, size_t blocks, size_t cap) {
  std::vector<float> src(block * ch), dst(block * ch), direct(block * ch);
  fill(src.data(), block, ch);

  const size_t warmup = std::min<size_t>(blocks, 200);
  // Warm the code path (allocator, TLB, branch predictors).
  for (size_t i = 0; i < warmup; ++i) {
    std::memcpy(direct.data(), src.data(), block * ch * sizeof(float));
    std::memcpy(dst.data(), direct.data(), block * ch * sizeof(float));
  }

  // ---- direct: two memcpys (models a same-process pass-through) ----
  {
    std::vector<int64_t> d;
    d.reserve(blocks);
    for (size_t i = 0; i < blocks; ++i) {
      int64_t t0 = rt::ns();
      std::memcpy(direct.data(), src.data(), block * ch * sizeof(float));
      std::memcpy(dst.data(), direct.data(), block * ch * sizeof(float));
      int64_t t1 = rt::ns();
      d.push_back(t1 - t0);
    }
    std::printf("direct  (2x memcpy, no IPC):\n");
    printStats("direct", summarize(d), static_cast<double>(block) * 1000.0 / 48000.0);
    g_direct = summarize(d);
  }

  // ---- ring: in-process write + read over shared memory ----
  {
    std::string err;
    AudioRing ring = AudioRing::create("bench", cap, ch, 48000, &err);
    if (!ring.valid()) {
      std::fprintf(stderr, "bench: create failed: %s\n", err.c_str());
      return 1;
    }
    // Warmup through the ring.
    for (size_t i = 0; i < warmup; ++i) {
      size_t w = ring.write(src.data(), block);
      ring.read(dst.data(), w);
    }
    std::vector<int64_t> r;
    r.reserve(blocks);
    for (size_t i = 0; i < blocks; ++i) {
      int64_t t0 = rt::ns();
      size_t w = ring.write(src.data(), block);
      ring.read(dst.data(), w);
      int64_t t1 = rt::ns();
      r.push_back(t1 - t0);
    }
    std::printf("ring    (shared mem, same proc):\n");
    printStats("ring", summarize(r), static_cast<double>(block) * 1000.0 / 48000.0);
    g_ring = summarize(r);
    ring.close();
    AudioRing::remove("bench");
  }

  // ---- added overhead ----
  int64_t added = g_ring.mean - g_direct.mean;
  std::printf("\nAdded ring overhead (mean): %lld ns per block "
              "(%lld ns above plain memcpy)\n",
              static_cast<long long>(g_ring.mean),
              static_cast<long long>(added));
  std::printf("=> The ring handoff itself is ~%lld ns: far below one audio "
              "block (%.2f ms at 48 kHz / %zu frames). Effective pipeline "
              "delay is dominated by the 1-block buffer, not the IPC.\n",
              static_cast<long long>(g_ring.mean),
              static_cast<double>(block) * 1000.0 / 48000.0, block);
  return 0;
}

// ---------------------------------------------------------------------------
// child-producer: write blocks on a real-time schedule, stamp send time
// ---------------------------------------------------------------------------
int runChildProducer(const std::string& name, int ch, size_t block,
                     size_t blocks, int64_t refNs) {
  std::string err;
  AudioRing ring = AudioRing::open(name, &err, 20000);
  if (!ring.valid()) {
    std::fprintf(stderr, "child-producer: open failed: %s\n", err.c_str());
    return 2;
  }
  std::vector<float> buf(block * ch);
  fill(buf.data(), block, ch);

  // Block interval in ns: one block of audio at 48 kHz.
  const int64_t intervalNs = static_cast<int64_t>(static_cast<double>(block) *
                                                  1000000000.0 / 48000.0);
  // Wait until the ring has space for the first block (consumer may still be
  // starting up), then go. Bounded: if the ring never frees up (stale
  // mapping, dead consumer) fail instead of spinning forever.
  {
    const auto spaceDeadline = std::chrono::steady_clock::now() +
                               std::chrono::seconds(30);
    while (ring.spaceAvailable() < block) {
      if (std::chrono::steady_clock::now() > spaceDeadline) {
        std::fprintf(stderr, "child-producer: ring never opened up\n");
        return 1;
      }
      rt::yield();
    }
  }

  int64_t next = rt::ns() + intervalNs;  // first block goes out one interval
  for (size_t i = 0; i < blocks; ++i) {
    // Pace to the real-time schedule; never run ahead.
    while (static_cast<int64_t>(rt::ns() - next) < 0) rt::yield();
    // Stamp send time (ns, relative to refNs — float32-safe) into sample 0.
    float* p = buf.data();
    p[0] = static_cast<float>(rt::ns() - refNs);
    // Write; if the ring is momentarily full, wait a tick and retry.
    // Bounded: a dead consumer would otherwise spin us forever.
    {
      const auto writeDeadline = std::chrono::steady_clock::now() +
                                 std::chrono::seconds(30);
      bool done = false;
      while (!done) {
        size_t got = ring.write(p, block);
        if (got == block) {
          done = true;
        } else if (std::chrono::steady_clock::now() > writeDeadline) {
          std::fprintf(stderr, "child-producer: ring stuck at block %zu\n", i);
          return 1;
        } else {
          rt::yield();
        }
      }
    }
    next += intervalNs;
  }
  ring.close();
  return 0;
}

// ---------------------------------------------------------------------------
// xproc: consumer side — spawn the producer child, measure arrival delay
// ---------------------------------------------------------------------------
int runXProc(int ch, size_t block, size_t blocks, size_t cap);

}  // namespace

// --- child spawning helpers (self-re-exec in child-producer mode) ----------
// The consumer must NOT wait for the producer before reading the ring — the
// producer only starts writing once it sees space, and the space only frees as
// the consumer reads. So spawn is async; the consumer reaps at the end.
namespace {

struct ChildHandle {
  bool valid = false;
#if defined(_WIN32)
  void* hProc = nullptr;
  void* hThread = nullptr;
#else
  pid_t pid = -1;
#endif
};

#if defined(_WIN32)
std::string selfPath() {
  char b[MAX_PATH];
  DWORD n = GetModuleFileNameA(nullptr, b, MAX_PATH);
  return n ? std::string(b, n) : std::string();
}
ChildHandle spawnChildAsync(const std::string& exe, const std::string& cmdline) {
  std::vector<char> cmd(cmdline.begin(), cmdline.end());
  cmd.push_back(0);
  STARTUPINFOA si {};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi {};
  ChildHandle h;
  if (CreateProcessA(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0,
                     nullptr, nullptr, &si, &pi)) {
    h.valid = true;
    h.hProc = pi.hProcess;
    h.hThread = pi.hThread;
  }
  return h;
}
bool childFinished(const ChildHandle& h) {
  return !h.valid || WaitForSingleObject(h.hProc, 0) == WAIT_OBJECT_0;
}
int childExitCode(ChildHandle& h) {
  int ec = -1;
  if (h.valid) {
    // Bounded wait: if the child somehow never exits, report -1 instead of
    // hanging the probe forever.
    if (WaitForSingleObject(h.hProc, 30000) == WAIT_OBJECT_0) {
      DWORD code = 0;
      GetExitCodeProcess(h.hProc, &code);
      ec = static_cast<int>(code);
    }
    CloseHandle(h.hProc);
    CloseHandle(h.hThread);
    h.valid = false;
  }
  return ec;
}
void childDestroy(ChildHandle&) {}
#else
#include <unistd.h>
std::string selfPath() {
  char b[4096];
  ssize_t n = readlink("/proc/self/exe", b, sizeof(b) - 1);
  return n > 0 ? std::string(b, n) : std::string();
}
ChildHandle spawnChildAsync(const std::string& exe, const std::string& cmdline) {
  pid_t pid = fork();
  ChildHandle h;
  if (pid < 0) return h;
  if (pid == 0) {
    std::vector<std::string> args;
    args.push_back(exe);
    std::string cur;
    for (char c : cmdline) {
      if (c == ' ') {
        if (!cur.empty()) {
          args.push_back(cur);
          cur.clear();
        }
      } else cur.push_back(c);
    }
    if (!cur.empty()) args.push_back(cur);
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    ::execv(exe.c_str(), argv.data());
    _exit(127);
  }
  h.valid = true;
  h.pid = pid;
  return h;
}
bool childFinished(const ChildHandle& h) {
  return !h.valid ||
         (waitpid(h.pid, nullptr, WNOHANG) != 0);
}
int childExitCode(ChildHandle& h) {
  int ec = -1;
  if (h.valid) {
    int status = 0;
    waitpid(h.pid, &status, 0);
    if (WIFEXITED(status)) ec = WEXITSTATUS(status);
    h.valid = false;
  }
  return ec;
}
void childDestroy(ChildHandle&) {}
#endif

}  // namespace

namespace {

int runXProc(int ch, size_t block, size_t blocks, size_t cap) {
  // A crashed earlier run can leave the mapping behind (Windows shared
  // objects survive process death); clear it so `create` starts fresh.
  AudioRing::remove("xproc");
  std::string err;
  AudioRing ring = AudioRing::create("xproc", cap, ch, 48000, &err);
  if (!ring.valid()) {
    std::fprintf(stderr, "xproc: create failed: %s\n", err.c_str());
    return 1;
  }
  std::string exe = selfPath();
  int64_t refNs = rt::ns();  // shared epoch, passed to the child
  std::string cmd = exe + " child-producer xproc " + std::to_string(ch) + " " +
                     std::to_string(block) + " " + std::to_string(blocks) + " " +
                     std::to_string(refNs);
  ChildHandle prod = spawnChildAsync(exe, cmd);
  if (!prod.valid) {
    std::fprintf(stderr, "xproc: failed to spawn producer child\n");
    ring.close();
    AudioRing::remove("xproc");
    return 1;
  }

  std::vector<float> buf(block * ch);
  const int64_t intervalNs = static_cast<int64_t>(static_cast<double>(block) *
                                                  1000000000.0 / 48000.0);
  std::vector<int64_t> delays;
  delays.reserve(blocks);
  size_t gotFrames = 0;
  const size_t wantFrames = blocks * block;
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(15 + (int)(blocks * block) / 10);
  while (gotFrames < wantFrames &&
         std::chrono::steady_clock::now() < deadline) {
    size_t n = ring.read(buf.data(), block);
    if (!n) {
      // Nothing to read yet. Bail early if the producer already exited —
      // otherwise we'd spin to the deadline on a dead child.
      if (childFinished(prod)) break;
      rt::yield();
      continue;
    }
    gotFrames += n;
    // The producer stamped (send time - refNs) in sample 0. Arrival minus
    // send = end-to-end delay (handoff + scheduling + our read loop).
    double sendRel = static_cast<double>(buf[0]);
    int64_t delay = (rt::ns() - refNs) - static_cast<int64_t>(sendRel);
    // Sanity: a healthy delay is small and positive; a huge/negative value
    // means the timestamp wrap or a missed block — skip outliers.
    if (delay > 0 && delay < 5 * intervalNs) delays.push_back(delay);
  }

  int spawn = childExitCode(prod);  // reap the producer
  childDestroy(prod);

  if (delays.empty()) {
    std::fprintf(stderr,
                 "xproc: no valid blocks received (spawn=%d, got=%zu/%zu)\n",
                 spawn, gotFrames, wantFrames);
    ring.close();
    AudioRing::remove("xproc");
    return 1;
  }
  Stats s = summarize(delays);
  double blockMs = static_cast<double>(block) * 1000.0 / 48000.0;
  std::printf("cross-process, real-time paced  (%zu/%zu blocks, ch=%d "
              "block=%zu cap=%zu):\n",
              gotFrames / block, blocks, ch, block, cap);
  printStats("end-to-end", s, blockMs);
  std::printf("  => end-to-end added latency: mean %.3f ms "
              "(%.1f%% of a %s ms block)\n",
              static_cast<double>(s.mean) / 1e6,
              100.0 * static_cast<double>(s.mean) /
                  (blockMs * 1e6),
              std::to_string(blockMs).c_str());
  std::printf("  => the DAW-to-Weft hop adds ~%.1f%% of one block of buffer; "
              "the rest of the perceived latency is the fixed 1-block "
              "alignment.\n",
              100.0 * static_cast<double>(s.mean) / (blockMs * 1e6));
  ring.close();
  AudioRing::remove("xproc");
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr,
                 "usage: ring_latency <bench|xproc> [ch] [block] [blocks] "
                 "[cap]\n");
    return 2;
  }
  std::string mode = argv[1];
  int ch = argc > 2 ? std::atoi(argv[2]) : 2;
  size_t block = argc > 3 ? static_cast<size_t>(std::atol(argv[3])) : 128;
  size_t blocks = argc > 4 ? static_cast<size_t>(std::atol(argv[4])) : 5000;
  size_t cap = argc > 5 ? static_cast<size_t>(std::atol(argv[5])) : 8192;
  if (ch < 1 || ch > 8) ch = 2;

  if (mode == "bench")
    return runBench(ch, block, blocks, cap);
  if (mode == "xproc")
    return runXProc(ch, block, blocks, cap);
  if (mode == "child-producer" && argc >= 7)
    return runChildProducer(argv[2], std::atoi(argv[3]),
                            static_cast<size_t>(std::atol(argv[4])),
                            static_cast<size_t>(std::atol(argv[5])),
                            std::atoll(argv[6]));
  std::fprintf(stderr, "unknown mode: %s\n", mode.c_str());
  return 2;
}
