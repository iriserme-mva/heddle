// weft-bridge-test -- t5 bridge round-trip test (headless, cross-platform).
//
// Drives the shared-memory ring IPC end-to-end WITHOUT loading the WeftBridge
// VST3 (the plugin is a CI compile/instantiate check; the real audio path is
// the ring pair, which is what this exercises). It is therefore the "bridge"
// for testing: it writes DAW-style audio into weft.bridge.in, reads the chain
// output from weft.bridge.out, and talks OSC to the host.
//
// Usage:
//   weft-bridge-test <weft-host> <smoke-vst3>
//
//   weft-host      path to the weft-host executable
//   smoke-vst3     path to the WeftSmokePlugin VST3 (the chain's Gain slot)
//
// Steps:
//   1. Write a Gain-0.5 chain.json next to weft-host.
//   2. Spawn:  weft-host <chain.json> --audio ring --osc-port <port>
//   3. Wait for both rings (weft.bridge.in / .out) to appear, open them.
//   4. Audio round-trip: write 8192 frames of a 0.5 constant into .in, read
//      8192 frames from .out, assert mean amplitude ~= 0.25  (= 0.5 * Gain 0.5).
//   5. OSC round-trip: /chain/query -> /reply/chain (slot list), then
//      /chain/quit -> /reply/status "quitting".
//   6. Reap the host (expect exit 0), clean up rings + chain file.
//
// Exit 0 = PASS, non-zero = FAIL (reason on stderr).
//
// NOTE: this binary links only weft_core (ring + OSC). It does NOT link JUCE,
// so no GUI/audio-device/Xvfb is needed to run it; the child weft-host runs
// headless (--audio ring, no --gui), and JUCE's X11 init fails gracefully on a
// display-less Linux runner (no crash).

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "weft/ring.hpp"
#include "weft/osc.hpp"
#include "weft/osc_io.hpp"

#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
#else
  #include <signal.h>
  #include <sys/wait.h>
  #include <unistd.h>
#endif

namespace {

// ---------------------------------------------------------------------------
// Child spawning / reaping (self-contained copy of the idiom from
// core/tools/ring_latency.cpp: CreateProcessA on Windows, fork+execv on
// POSIX). The host is spawned async and reaped at the end.
struct ChildHandle {
  bool valid = false;
#if defined(_WIN32)
  void* hProc = nullptr;
  void* hThread = nullptr;
#else
  pid_t pid = -1;
#endif
};

ChildHandle spawnChild(const std::string& exe, const std::vector<std::string>& argv) {
#if defined(_WIN32)
  // Build a mutable command line from argv, which follows the execv
  // convention: argv[0] is the executable path, argv[1..] are the arguments.
  // (This matches the POSIX branch below and the caller, which passes
  // {hostExe, chainPath, ...}. Quoting each token that contains a
  // space/quote/backslash for the Win32 CreateProcessA command line.)
  std::string cmd;
  for (const std::string& a : argv) {
    if (!cmd.empty()) cmd += " ";
    if (a.find_first_of(" \\\"") == std::string::npos) {
      cmd += a;
    } else {
      cmd += "\"";
      size_t backslashes = 0;
      for (char c : a) {
        if (c == '\\') { backslashes++; continue; }
        cmd.append(backslashes, '\\');
        if (c == '"') cmd.push_back('\\');
        backslashes = 0;
        cmd.push_back(c);
      }
      cmd.append(backslashes, '\\');
      cmd.push_back('\\');
      cmd.push_back('"');
    }
  }
  std::vector<char> cmdBuf(cmd.begin(), cmd.end());
  cmdBuf.push_back(0);
  STARTUPINFOA si {};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi {};
  ChildHandle h;
  if (CreateProcessA(exe.c_str(), cmdBuf.data(), nullptr, nullptr, FALSE, 0,
                     nullptr, nullptr, &si, &pi)) {
    h.valid = true;
    h.hProc = pi.hProcess;
    h.hThread = pi.hThread;
  }
  return h;
#else
  pid_t pid = fork();
  ChildHandle h;
  if (pid < 0) return h;
  if (pid == 0) {
    std::vector<char*> cargv;
    for (const std::string& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
    cargv.push_back(nullptr);
    ::execv(exe.c_str(), cargv.data());
    _exit(127);
  }
  h.valid = true;
  h.pid = pid;
  return h;
#endif
}

bool childFinished(const ChildHandle& h) {
  if (!h.valid) return true;
#if defined(_WIN32)
  return WaitForSingleObject(h.hProc, 0) == WAIT_OBJECT_0;
#else
  return waitpid(h.pid, nullptr, WNOHANG) != 0;
#endif
}

int childReap(ChildHandle& h) {
  int ec = -1;
  if (!h.valid) return ec;
#if defined(_WIN32)
  if (WaitForSingleObject(h.hProc, 30000) == WAIT_OBJECT_0) {
    DWORD code = 0;
    GetExitCodeProcess(h.hProc, &code);
    ec = static_cast<int>(code);
  }
  CloseHandle(h.hProc);
  CloseHandle(h.hThread);
#else
  int status = 0;
  waitpid(h.pid, &status, 0);
  if (WIFEXITED(status)) ec = WEXITSTATUS(status);
#endif
  h.valid = false;
  return ec;
}

void childKill(ChildHandle& h) {
  if (!h.valid) return;
#if defined(_WIN32)
  TerminateProcess(h.hProc, 1);
#else
  ::kill(h.pid, SIGKILL);
#endif
}

// ---------------------------------------------------------------------------
using Clock = std::chrono::steady_clock;

std::string dirOf(const std::string& p) {
#if defined(_WIN32)
  size_t i = p.find_last_of("/\\");
#else
  size_t i = p.find_last_of('/');
#endif
  if (i == std::string::npos) return ".";
  return p.substr(0, i + 1);
}

std::string jsonEscape(const std::string& s) {
  std::string out;
  for (char c : s) {
    if (c == '\\' || c == '"') out.push_back('\\');
    out.push_back(c);
  }
  return out;
}

void fail(ChildHandle& child, const std::string& chainPath,
          const char* inName, const char* outName, const std::string& why) {
  std::fprintf(stderr, "FAIL: %s\n", why.c_str());
  childKill(child);
  if (child.valid) childReap(child);
  weft::AudioRing::remove(inName);
  weft::AudioRing::remove(outName);
  std::remove(chainPath.c_str());
  // Must actually terminate: a non-exiting fail() lets the rest of the test
  // run against an already-killed host, producing cascading, misleading
  // failures (e.g. an OSC timeout that is not a real OSC bug).
  std::exit(1);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: weft-bridge-test <weft-host> <smoke-vst3>\n");
    return 2;
  }
  const std::string hostExe = argv[1];
  const std::string smokeVst3 = argv[2];

  const char* inName = "weft.bridge.in";
  const char* outName = "weft.bridge.out";

  // Deterministic, per-process OSC port (avoids collisions between runs).
  long seed;
#if defined(_WIN32)
  seed = static_cast<long>(GetCurrentProcessId());
#else
  seed = static_cast<long>(getpid());
#endif
  const int hostPort = 20000 + static_cast<int>(seed % 4000);

  // 1) Write the Gain-0.5 chain next to weft-host (the build dir is writable
  //    on all three CI runners; /tmp is not relied on).
  const std::string chainPath = dirOf(hostExe) + "weft_bridge_test_chain.json";
  // Default-constructed (invalid) child so the fail() calls that fire BEFORE
  // spawn still have a ChildHandle& to pass. fail() no-ops kill/reap on an
  // invalid child, so this is safe pre-launch. Assigned after spawnChild().
  ChildHandle child;
  {
    std::string json =
        "{\n"
        "  \"sample_rate\": 48000,\n"
        "  \"block_size\": 1024,\n"
        "  \"chain\": [\n"
        "    {\n"
        "      \"id\": \"smoke\",\n"
        "      \"plugin\": \"" + jsonEscape(smokeVst3) + "\",\n"
        "      \"enabled\": true,\n"
        "      \"params\": { \"Gain\": 0.5, \"PWM pct\": 1.0 }\n"
        "    }\n"
        "  ]\n"
        "}\n";
    FILE* f = std::fopen(chainPath.c_str(), "wb");
    if (!f) fail(child, chainPath, inName, outName, "cannot write chain file: " + chainPath);
    std::fwrite(json.data(), 1, json.size(), f);
    std::fclose(f);
  }

  // Best-effort removal of any stale rings (POSIX shm_unlink; no-op on
  // Windows -- the host also self-cleans on create).
  weft::AudioRing::remove(inName);
  weft::AudioRing::remove(outName);

  // 2) Spawn the host headless in ring mode.
  std::vector<std::string> hargv = {
      hostExe, chainPath, "--audio", "ring",
      "--osc-port", std::to_string(hostPort)};
  child = spawnChild(hostExe, hargv);
  if (!child.valid) fail(child, chainPath, inName, outName, "failed to spawn weft-host");

  // 3) Wait for both rings to appear (the host loads the VST3 first, then
  //    creates the rings), then open them.
  const auto ringDeadline = Clock::now() + std::chrono::seconds(30);
  while (Clock::now() < ringDeadline &&
         !(weft::AudioRing::exists(inName) && weft::AudioRing::exists(outName))) {
    if (childFinished(child))
      fail(child, chainPath, inName, outName,
           "weft-host exited before creating the rings");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  if (!(weft::AudioRing::exists(inName) && weft::AudioRing::exists(outName)))
    fail(child, chainPath, inName, outName, "rings did not appear within 30 s");

  std::string err;
  weft::AudioRing ringIn = weft::AudioRing::open(inName, &err, 3000);
  weft::AudioRing ringOut = weft::AudioRing::open(outName, &err, 3000);
  if (!ringIn.valid() || !ringOut.valid())
    fail(child, chainPath, inName, outName, "open rings failed: " + err);

  const size_t frames = 8192;  // = 8 x 1024 host blocks
  const size_t samples = frames * 2;

  // 4a) Write a 0.5 constant (stereo) into the input ring.
  std::vector<float> inBuf(samples, 0.5f);
  const size_t written = ringIn.write(inBuf.data(), frames);
  if (written < frames)
    fail(child, chainPath, inName, outName,
         "ringIn.write short (" + std::to_string(written) + "/" +
             std::to_string(frames) + ")");

  // 4b) Read 8192 frames from the output ring; assert mean ~= 0.25.
  std::vector<float> outBuf(samples, 0.0f);
  size_t got = 0;
  const auto audioDeadline = Clock::now() + std::chrono::seconds(20);
  while (got < frames && Clock::now() < audioDeadline) {
    const size_t n = ringOut.read(outBuf.data() + got * 2, frames - got);
    if (n > 0) {
      got += n;
    } else {
      if (childFinished(child))
        fail(child, chainPath, inName, outName,
             "weft-host exited before producing output");
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  if (got < frames)
    fail(child, chainPath, inName, outName,
         "only " + std::to_string(got) + "/" + std::to_string(frames) +
             " output frames arrived");

  double sumOut = 0.0;
  for (size_t i = 0; i < samples; ++i) sumOut += outBuf[i];
  const double meanOut = sumOut / static_cast<double>(samples);
  const double meanIn = 0.5;
  const double ratio = meanOut / meanIn;
  // Per-channel means + a small value histogram to disambiguate a dead
  // channel (0.25 / 0.0) from a uniformly-halved gain (0.125 / 0.125).
  double chSum[2] = {0.0, 0.0};
  std::map<double, long> hist;
  for (size_t i = 0; i < samples; ++i) {
    const int ch = static_cast<int>(i % 2);
    const float v = outBuf[i];
    chSum[ch] += v;
    // Quantize to 4 decimals so float noise collapses into buckets.
    hist[std::round(static_cast<double>(v) * 10000.0) / 10000.0]++;
  }
  const double chMean0 = chSum[0] / static_cast<double>(frames);
  const double chMean1 = chSum[1] / static_cast<double>(frames);
  std::printf("bridge-test: meanOut=%.6f  meanIn=%.6f  out/in=%.4f  (frames=%zu)\n",
              meanOut, meanIn, ratio, got);
  std::printf("bridge-test: ch0 mean=%.6f  ch1 mean=%.6f\n", chMean0, chMean1);
  std::printf("bridge-test: out value histogram: ");
  for (const auto& kv : hist)
    std::printf("%s%.4f=%ld ", (kv.first == hist.begin()->first ? "" : "  "), kv.first, kv.second);
  std::printf("\n");
  if (std::fabs(meanOut - 0.25) > 0.01)
    fail(child, chainPath, inName, outName,
         "gain round-trip: expected mean ~= 0.25, got " +
             std::to_string(meanOut).substr(0, 6));

  // 5) OSC round-trip on a single full-duplex socket.
  weft::OscUdp client;
  if (!client.bind("127.0.0.1", 0, &err))
    fail(child, chainPath, inName, outName, "client bind failed: " + err);
  if (!client.openOut("127.0.0.1", hostPort, &err))
    fail(child, chainPath, inName, outName, "client openOut failed: " + err);

  // 5a) /chain/query -> /reply/chain (slot list, one string).
  client.send(weft::OscMsg::make("/chain/query"));
  weft::OscMsg m;
  std::string chainBody;
  bool gotChain = false;
  const auto oscDeadline = Clock::now() + std::chrono::seconds(5);
  while (Clock::now() < oscDeadline && !gotChain) {
    if (client.poll(m)) {
      if (m.addr == "/reply/chain" && !m.strs.empty()) {
        gotChain = true;
        chainBody = m.strs[0];
      }
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  if (!gotChain)
    fail(child, chainPath, inName, outName, "no /reply/chain for /chain/query");
  if (chainBody.find("smoke") == std::string::npos)
    fail(child, chainPath, inName, outName,
         "/reply/chain missing 'smoke' slot: " + chainBody);
  std::printf("bridge-test: /reply/chain -> %s\n", chainBody.c_str());

  // 5b) /chain/quit -> /reply/status "quitting", then the host exits 0.
  client.send(weft::OscMsg::make("/chain/quit"));
  bool gotQuit = false;
  std::string statusBody;
  const auto quitDeadline = Clock::now() + std::chrono::seconds(5);
  while (Clock::now() < quitDeadline && !gotQuit) {
    if (client.poll(m)) {
      if (m.addr == "/reply/status" && !m.strs.empty()) {
        statusBody = m.strs[0];
        if (statusBody.find("quitting") != std::string::npos) gotQuit = true;
      }
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  if (!gotQuit)
    fail(child, chainPath, inName, outName, "no 'quitting' for /chain/quit");

  // 6) Reap (expect clean exit 0) and clean up.
  const int exitCode = childReap(child);
  ringIn.close();
  ringOut.close();
  client.close();
  weft::AudioRing::remove(inName);
  weft::AudioRing::remove(outName);
  std::remove(chainPath.c_str());

  if (exitCode != 0) {
    std::fprintf(stderr, "FAIL: weft-host exit code %d (expected 0)\n", exitCode);
    return 1;
  }

  std::printf("bridge-test: PASS (ring round-trip ratio=%.4f, OSC query+quit OK)\n",
              ratio);
  return 0;
}
