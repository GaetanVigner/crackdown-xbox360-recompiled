// A freeze leaves nothing in the log: Windows only reports "stopped responding" with no stack. This watchdog writes
// the call stack of every thread when the window stops answering messages or the title stops producing frames, so
// the thread that holds things up and what it waits on can be read afterwards.

#include "hang_watchdog.h"

#include <atomic>
#include <cstdint>
#include <filesystem>

#include <rex/cvar.h>

#if defined(_WIN32)
#include <algorithm>
#include <chrono>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// clang-format off
#include <dbghelp.h>
#include <tlhelp32.h>
// clang-format on

#include <fmt/format.h>
#include <rex/logging.h>

#include "platform.h"
#endif

REXCVAR_DEFINE_BOOL(crackdown_debug_profiler, false, "Crackdown",
                    "Developer diagnostics: F6 samples the busy threads for 10 s and writes the functions they spent "
                    "their time in to profile_<n>.txt in the session logs folder");

namespace crackdown {
namespace {

std::atomic<uint64_t> g_guest_frames{0};
// The session's logs folder (StartHangWatchdog), shared by the developer diagnostics.
std::filesystem::path g_logs_folder;

#if defined(_WIN32)
constexpr DWORD kWindowTimeoutMs = 5000;
constexpr auto kFrameStall = std::chrono::seconds(10);
constexpr auto kPoll = std::chrono::seconds(1);
constexpr int kMaxReports = 3;
constexpr size_t kMaxFrames = 48;

struct ThreadStack {
  DWORD id = 0;
  std::wstring name;
  size_t count = 0;
  DWORD64 frames[kMaxFrames] = {};
};

bool Readable(DWORD64 address) {
  MEMORY_BASIC_INFORMATION info;
  if (VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)) != sizeof(info)) {
    return false;
  }
  constexpr DWORD kReadable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                              PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
  return info.State == MEM_COMMIT && (info.Protect & kReadable) && !(info.Protect & PAGE_GUARD) &&
         address + 64 <= reinterpret_cast<DWORD64>(info.BaseAddress) + info.RegionSize;
}

// Unwinds a suspended thread's context into return addresses. Allocation-free: the thread may hold the heap lock.
size_t Unwind(CONTEXT context, DWORD64* frames) {
  size_t count = 0;
  while (count < kMaxFrames && context.Rip != 0) {
    frames[count++] = context.Rip;
    if (!Readable(context.Rsp)) {
      break;
    }
    const DWORD64 rsp = context.Rsp;
    DWORD64 image_base = 0;
    if (PRUNTIME_FUNCTION entry = RtlLookupFunctionEntry(context.Rip, &image_base, nullptr)) {
      void* handler_data = nullptr;
      DWORD64 frame = 0;
      RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, entry, &context, &handler_data, &frame, nullptr);
    } else {
      // A leaf function: the return address is on top of the stack.
      context.Rip = *reinterpret_cast<const DWORD64*>(context.Rsp);
      context.Rsp += 8;
    }
    if (context.Rsp <= rsp) {
      break;
    }
  }
  return count;
}

std::vector<ThreadStack> CaptureThreads() {
  std::vector<ThreadStack> stacks;
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snapshot == INVALID_HANDLE_VALUE) {
    return stacks;
  }
  const DWORD process = GetCurrentProcessId();
  const DWORD self = GetCurrentThreadId();
  std::vector<DWORD> ids;
  THREADENTRY32 entry{};
  entry.dwSize = sizeof(entry);
  for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
    if (entry.th32OwnerProcessID == process && entry.th32ThreadID != self) {
      ids.push_back(entry.th32ThreadID);
    }
  }
  CloseHandle(snapshot);
  stacks.reserve(ids.size());
  for (DWORD id : ids) {
    HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE, id);
    if (!thread) {
      continue;
    }
    ThreadStack& stack = stacks.emplace_back();
    stack.id = id;
    PWSTR description = nullptr;
    if (SUCCEEDED(GetThreadDescription(thread, &description)) && description) {
      stack.name = description;
      LocalFree(description);
    }
    if (SuspendThread(thread) != DWORD(-1)) {
      CONTEXT context{};
      context.ContextFlags = CONTEXT_FULL;
      if (GetThreadContext(thread, &context)) {
        stack.count = Unwind(context, stack.frames);
      }
      ResumeThread(thread);
    }
    CloseHandle(thread);
  }
  return stacks;
}

// NameLen is the symbol's full length even when Name was cut at MaxNameLen: a long C++ name read past the buffer.
std::string SymbolName(const SYMBOL_INFOW& symbol) {
  return Utf8(std::wstring(symbol.Name, wcsnlen(symbol.Name, std::min<size_t>(symbol.NameLen, symbol.MaxNameLen - 1))));
}

std::string Describe(HANDLE process, DWORD64 address) {
  alignas(SYMBOL_INFOW) uint8_t buffer[sizeof(SYMBOL_INFOW) + 256 * sizeof(wchar_t)] = {};
  auto* symbol = reinterpret_cast<SYMBOL_INFOW*>(buffer);
  symbol->SizeOfStruct = sizeof(SYMBOL_INFOW);
  symbol->MaxNameLen = 255;
  std::string text = fmt::format("{:016X}", address);
  IMAGEHLP_MODULEW64 module{};
  module.SizeOfStruct = sizeof(module);
  if (SymGetModuleInfoW64(process, address, &module)) {
    text += " " + Utf8(module.ModuleName);
  }
  DWORD64 displacement = 0;
  if (SymFromAddrW(process, address, &displacement, symbol)) {
    text += "!" + SymbolName(*symbol) +
            fmt::format("+0x{:X}", displacement);
  }
  IMAGEHLP_LINEW64 line{};
  line.SizeOfStruct = sizeof(line);
  DWORD column = 0;
  if (SymGetLineFromAddrW64(process, address, &column, &line)) {
    text += fmt::format(" ({}:{})", Utf8(std::filesystem::path(line.FileName).filename()), line.LineNumber);
  }
  return text;
}

// dbghelp is single-threaded; the watchdog and the profiler both run their symbol work under this lock.
std::mutex g_symbol_mutex;

bool InitSymbols(HANDLE process) {
  SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
  static const bool symbols = SymInitializeW(process, nullptr, TRUE) != FALSE;
  if (symbols) {
    SymRefreshModuleList(process);
  }
  return symbols;
}

std::filesystem::path NextLogPath(const std::filesystem::path& folder, const char* prefix) {
  std::error_code ec;
  std::filesystem::create_directories(folder, ec);
  std::filesystem::path path;
  for (int n = 1; n < 1000; ++n) {
    path = folder / (std::string(prefix) + std::to_string(n) + ".txt");
    if (!std::filesystem::exists(path, ec)) {
      break;
    }
  }
  return path;
}

void WriteReportOrThrow(const std::filesystem::path& folder, const char* reason) {
  const std::vector<ThreadStack> stacks = CaptureThreads();
  HANDLE process = GetCurrentProcess();
  std::lock_guard lock(g_symbol_mutex);
  const bool symbols = InitSymbols(process);
  const std::filesystem::path path = NextLogPath(folder, "hang_");
  std::ofstream out(path);
  out << "Reason: " << reason << "\nGuest frames so far: " << g_guest_frames.load() << "\n";
  for (const ThreadStack& stack : stacks) {
    out << "\nThread " << stack.id;
    if (!stack.name.empty()) {
      out << " \"" << Utf8(stack.name) << "\"";
    }
    out << "\n";
    for (size_t i = 0; i < stack.count; ++i) {
      out << "  " << i << "  " << (symbols ? Describe(process, stack.frames[i]) : fmt::format("{:016X}", stack.frames[i]))
          << "\n";
    }
  }
  out.close();
  REXLOG_ERROR("[HANG] {}: thread stacks written to {}", reason, Utf8(path));
}

// A diagnostics failure is logged, never allowed to end the game.
void WriteReport(const std::filesystem::path& folder, const char* reason) {
  try {
    WriteReportOrThrow(folder, reason);
  } catch (const std::exception& error) {
    REXLOG_ERROR("[HANG] {}: could not write the report: {}", reason, error.what());
  }
}

void Watch(HWND window, std::filesystem::path folder) {
  int window_reports = 0;
  int stall_reports = 0;
  uint64_t last_frames = g_guest_frames.load();
  auto frames_changed = std::chrono::steady_clock::now();
  bool window_hung = false;
  bool frames_stalled = false;
  while (window_reports < kMaxReports || stall_reports < kMaxReports) {
    std::this_thread::sleep_for(kPoll);
    if (!IsWindow(window)) {
      return;
    }
    DWORD_PTR result = 0;
    const bool answered =
        SendMessageTimeoutW(window, WM_NULL, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, kWindowTimeoutMs, &result) != 0;
    if (!answered && !window_hung && window_reports < kMaxReports) {
      WriteReport(folder, "the window stopped answering messages for 5 s");
      ++window_reports;
    }
    window_hung = !answered;

    const auto now = std::chrono::steady_clock::now();
    const uint64_t frames = g_guest_frames.load();
    // A minimized or background window may legitimately stop the title.
    if (frames != last_frames || IsIconic(window) || GetForegroundWindow() != window) {
      last_frames = frames;
      frames_changed = now;
      frames_stalled = false;
    } else if (frames > 0 && !frames_stalled && stall_reports < kMaxReports && now - frames_changed >= kFrameStall) {
      frames_stalled = true;
      WriteReport(folder, "the title produced no frame for 10 s");
      ++stall_reports;
    }
  }
}

// --- Sampling profiler (crackdown_debug_profiler, F6) ---

constexpr auto kHotThreadWindow = std::chrono::milliseconds(500);
constexpr double kHotThreadShare = 0.05;
constexpr size_t kTopFunctions = 40;

std::atomic<bool> g_profiling{false};

double ThreadSeconds(HANDLE thread) {
  FILETIME created, exited, kernel, user;
  if (!GetThreadTimes(thread, &created, &exited, &kernel, &user)) {
    return 0.0;
  }
  auto seconds = [](const FILETIME& t) { return double(uint64_t(t.dwHighDateTime) << 32 | t.dwLowDateTime) * 1e-7; };
  return seconds(kernel) + seconds(user);
}

struct ProfiledThread {
  DWORD id = 0;
  HANDLE handle = nullptr;
  std::string name;
  double cpu_share = 0.0;
  std::vector<std::vector<DWORD64>> stacks;
};

// The threads using at least kHotThreadShare of a core, measured over kHotThreadWindow.
std::vector<ProfiledThread> HotThreads() {
  std::vector<ProfiledThread> threads;
  for (const ThreadStack& stack : CaptureThreads()) {
    HANDLE handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE,
                               stack.id);
    if (handle) {
      ProfiledThread& thread = threads.emplace_back();
      thread.id = stack.id;
      thread.handle = handle;
      thread.name = Utf8(stack.name);
    }
  }
  std::vector<double> before;
  for (const ProfiledThread& thread : threads) before.push_back(ThreadSeconds(thread.handle));
  const auto start = std::chrono::steady_clock::now();
  std::this_thread::sleep_for(kHotThreadWindow);
  const double window = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  std::vector<ProfiledThread> hot;
  for (size_t i = 0; i < threads.size(); ++i) {
    threads[i].cpu_share = (ThreadSeconds(threads[i].handle) - before[i]) / window;
    if (threads[i].cpu_share >= kHotThreadShare) {
      hot.push_back(std::move(threads[i]));
    } else {
      CloseHandle(threads[i].handle);
    }
  }
  return hot;
}

// module!function, or module+offset without a symbol.
std::string FunctionName(HANDLE process, DWORD64 address) {
  alignas(SYMBOL_INFOW) uint8_t buffer[sizeof(SYMBOL_INFOW) + 256 * sizeof(wchar_t)] = {};
  auto* symbol = reinterpret_cast<SYMBOL_INFOW*>(buffer);
  symbol->SizeOfStruct = sizeof(SYMBOL_INFOW);
  symbol->MaxNameLen = 255;
  IMAGEHLP_MODULEW64 module{};
  module.SizeOfStruct = sizeof(module);
  const std::string module_name =
      SymGetModuleInfoW64(process, address, &module) ? Utf8(module.ModuleName) : "?";
  DWORD64 displacement = 0;
  if (SymFromAddrW(process, address, &displacement, symbol)) {
    return module_name + "!" + SymbolName(*symbol);
  }
  return fmt::format("{}+0x{:X}", module_name, address - module.BaseOfImage);
}

void WriteProfile(std::vector<ProfiledThread>& threads, double seconds) {
  HANDLE process = GetCurrentProcess();
  std::lock_guard lock(g_symbol_mutex);
  InitSymbols(process);
  std::unordered_map<DWORD64, uint32_t> function_of;
  std::vector<std::string> names;
  std::unordered_map<std::string, uint32_t> name_ids;
  auto function = [&](DWORD64 address) {
    auto [it, inserted] = function_of.try_emplace(address, 0);
    if (inserted) {
      std::string name = FunctionName(process, address);
      auto [id, added] = name_ids.try_emplace(name, uint32_t(names.size()));
      if (added) names.push_back(std::move(name));
      it->second = id->second;
    }
    return it->second;
  };

  const std::filesystem::path path = NextLogPath(g_logs_folder, "profile_");
  std::ofstream out(path);
  out << fmt::format("Sampling profile, {:.1f} s, guest frames so far {}\n", seconds, g_guest_frames.load());
  out << "self: the function itself was running; total: it was on the stack (itself or what it called).\n";
  for (ProfiledThread& thread : threads) {
    const size_t samples = thread.stacks.size();
    out << fmt::format("\n=== Thread {} \"{}\": {:.0f}% of a core, {} samples ===\n", thread.id, thread.name,
                       thread.cpu_share * 100.0, samples);
    if (samples == 0) continue;
    std::unordered_map<uint32_t, uint32_t> self, total;
    std::vector<uint32_t> seen;
    for (const auto& stack : thread.stacks) {
      if (stack.empty()) continue;
      ++self[function(stack[0])];
      seen.clear();
      for (DWORD64 address : stack) {
        const uint32_t id = function(address);
        if (std::find(seen.begin(), seen.end(), id) == seen.end()) {
          seen.push_back(id);
          ++total[id];
        }
      }
    }
    auto print = [&](const char* title, const std::unordered_map<uint32_t, uint32_t>& counts) {
      std::vector<std::pair<uint32_t, uint32_t>> sorted(counts.begin(), counts.end());
      std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
      out << title << "\n";
      for (size_t i = 0; i < sorted.size() && i < kTopFunctions; ++i) {
        out << fmt::format("  {:5.1f}%  {}\n", 100.0 * sorted[i].second / double(samples), names[sorted[i].first]);
      }
    };
    print("-- self --", self);
    print("-- total --", total);
  }
  out.close();
  REXLOG_WARN("[PROFILE] {} thread(s) sampled for {:.1f} s, written to {}", threads.size(), seconds, Utf8(path));
}

void Profile(double seconds) {
  REXLOG_WARN("[PROFILE] sampling the busy threads for {:.0f} s", seconds);
  std::vector<ProfiledThread> threads = HotThreads();
  HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  const auto start = std::chrono::steady_clock::now();
  const auto end = start + std::chrono::duration<double>(seconds);
  DWORD64 frames[kMaxFrames];
  while (std::chrono::steady_clock::now() < end) {
    for (ProfiledThread& thread : threads) {
      size_t count = 0;
      if (SuspendThread(thread.handle) != DWORD(-1)) {
        CONTEXT context{};
        context.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(thread.handle, &context)) {
          count = Unwind(context, frames);
        }
        ResumeThread(thread.handle);
      }
      // Recorded only once the thread runs again: it may hold the heap lock.
      if (count) thread.stacks.emplace_back(frames, frames + count);
    }
    LARGE_INTEGER due;
    due.QuadPart = -10'000;  // 1 ms
    if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
      WaitForSingleObject(timer, INFINITE);
    } else {
      Sleep(1);
    }
  }
  if (timer) CloseHandle(timer);
  // A diagnostics failure is logged, never allowed to end the game.
  try {
    WriteProfile(threads, std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
  } catch (const std::exception& error) {
    REXLOG_ERROR("[PROFILE] could not write the profile: {}", error.what());
  }
  for (ProfiledThread& thread : threads) CloseHandle(thread.handle);
  g_profiling = false;
}
#endif

}  // namespace

void NoteGuestFrame() {
  g_guest_frames.fetch_add(1, std::memory_order_relaxed);
}

void StartHangWatchdog(void* native_window, const std::filesystem::path& logs_folder) {
  g_logs_folder = logs_folder;
#if defined(_WIN32)
  if (!native_window) {
    return;
  }
  std::thread(Watch, static_cast<HWND>(native_window), logs_folder).detach();
#else
  (void)native_window;
#endif
}

const std::filesystem::path& DiagnosticsLogsFolder() {
  return g_logs_folder;
}

bool ThreadProfilerEnabled() {
  return REXCVAR_GET(crackdown_debug_profiler);
}

bool StartThreadProfile(double seconds) {
#if defined(_WIN32)
  if (g_profiling.exchange(true)) {
    return false;
  }
  std::thread(Profile, seconds).detach();
  return true;
#else
  (void)seconds;
  return false;
#endif
}

}  // namespace crackdown
