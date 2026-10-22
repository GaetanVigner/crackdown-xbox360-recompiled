#include "physics_telemetry.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "guest_memory.h"
#include "hang_watchdog.h"
#include "platform.h"

REXCVAR_DEFINE_BOOL(crackdown_physics_telemetry, false, "Crackdown",
                    "Developer diagnostics: keep the last ~34 s of the player's raw state (about 58 MB) and write it to "
                    "logs/telemetry_<n>.bin on F9 or on a NaN repair");

namespace {

using crackdown::guest::Load32;
using crackdown::guest::Plausible;

// Same as cheat_hooks.cpp: the local player controller, and its +292 holding the Character plus 32.
constexpr uint32_t kPlayerControllerGlobal = 0x82DE3F3C;
constexpr uint32_t kCharacterOffsetInController = 292;
constexpr uint32_t kCharacterBase = 32;
constexpr uint32_t kPositionOffset = 0x140;
constexpr uint32_t kVehicleOffset = 5580;
constexpr uint32_t kVehicleFlagsOffset = 5457;
constexpr uint8_t kDrivingFlags = 0x18;

struct TelemetryFrame {
  double time_seconds;
  double step_seconds;
  uint32_t character;
  uint32_t controller;
  uint32_t vehicle;
  std::array<uint8_t, crackdown::kTelemetryCharacterBytes> character_bytes;
  std::array<uint8_t, crackdown::kTelemetryControllerBytes> controller_bytes;
  std::array<uint8_t, crackdown::kTelemetryVehicleBytes> vehicle_bytes;
};

// About 58 MB, allocated on the first update so a session that never plays costs nothing.
std::unique_ptr<std::array<TelemetryFrame, crackdown::kTelemetryFrames>> g_ring;
uint32_t g_next = 0;
uint32_t g_count = 0;
std::atomic<bool> g_dump_requested{false};
const auto g_start = std::chrono::steady_clock::now();

void CopyGuest(uint8_t* base, uint32_t address, uint8_t* out, size_t size) {
  if (Plausible(address)) {
    std::memcpy(out, rex::memory::GuestPtr<const uint8_t*>(base, address), size);
  } else {
    std::memset(out, 0, size);
  }
}

std::filesystem::path NextDumpPath() {
  const std::filesystem::path folder = crackdown::DiagnosticsLogsFolder().empty()
                                          ? crackdown::PortFolder() / "logs"
                                          : crackdown::DiagnosticsLogsFolder();
  std::error_code ec;
  std::filesystem::create_directories(folder, ec);
  for (int n = 1;; ++n) {
    std::filesystem::path path = folder / ("telemetry_" + std::to_string(n) + ".bin");
    if (!std::filesystem::exists(path, ec)) {
      return path;
    }
  }
}

// Layout: "CDTEL2\0\0", u32 frames, u32 character bytes, u32 controller bytes, u32 vehicle bytes (little-endian),
// then each frame oldest first as f64 time, f64 step, u32 character, u32 controller, u32 vehicle (0 when not
// driving), the character bytes, the controller bytes, the vehicle bytes. The copied objects stay guest big-endian.
void Dump() {
  if (!g_ring || !g_count) {
    REXLOG_WARN("[TELEMETRY] nothing recorded yet");
    return;
  }
  const std::filesystem::path path = NextDumpPath();
  std::ofstream out(path, std::ios::binary);
  const char magic[8] = {'C', 'D', 'T', 'E', 'L', '2', 0, 0};
  const uint32_t header[4] = {g_count, crackdown::kTelemetryCharacterBytes, crackdown::kTelemetryControllerBytes,
                              crackdown::kTelemetryVehicleBytes};
  out.write(magic, sizeof(magic));
  out.write(reinterpret_cast<const char*>(header), sizeof(header));
  const uint32_t first = (g_next + crackdown::kTelemetryFrames - g_count) % crackdown::kTelemetryFrames;
  for (uint32_t i = 0; i < g_count; ++i) {
    const TelemetryFrame& frame = (*g_ring)[(first + i) % crackdown::kTelemetryFrames];
    out.write(reinterpret_cast<const char*>(&frame.time_seconds), sizeof(frame.time_seconds));
    out.write(reinterpret_cast<const char*>(&frame.step_seconds), sizeof(frame.step_seconds));
    out.write(reinterpret_cast<const char*>(&frame.character), sizeof(frame.character));
    out.write(reinterpret_cast<const char*>(&frame.controller), sizeof(frame.controller));
    out.write(reinterpret_cast<const char*>(&frame.vehicle), sizeof(frame.vehicle));
    out.write(reinterpret_cast<const char*>(frame.character_bytes.data()), frame.character_bytes.size());
    out.write(reinterpret_cast<const char*>(frame.controller_bytes.data()), frame.controller_bytes.size());
    out.write(reinterpret_cast<const char*>(frame.vehicle_bytes.data()), frame.vehicle_bytes.size());
  }
  if (out) {
    REXLOG_WARN("[TELEMETRY] wrote {} updates to {}", g_count, path.string());
  } else {
    REXLOG_ERROR("[TELEMETRY] could not write {}", path.string());
  }
}

}  // namespace

void crackdown::RecordPhysicsStep(uint8_t* base, double step_seconds) {
  if (!REXCVAR_GET(crackdown_physics_telemetry)) {
    return;
  }
  const uint32_t controller = Load32(base, kPlayerControllerGlobal);
  const uint32_t subobject = Plausible(controller) ? Load32(base, controller + kCharacterOffsetInController) : 0;
  const uint32_t character = Plausible(subobject) ? subobject - kCharacterBase : 0;
  if (Plausible(controller) && Plausible(character)) {
    if (!g_ring) {
      g_ring = std::make_unique<std::array<TelemetryFrame, kTelemetryFrames>>();
    }
    TelemetryFrame& frame = (*g_ring)[g_next];
    frame.time_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
    frame.step_seconds = step_seconds;
    frame.character = character;
    frame.controller = controller;
    CopyGuest(base, character, frame.character_bytes.data(), frame.character_bytes.size());
    CopyGuest(base, controller, frame.controller_bytes.data(), frame.controller_bytes.size());
    const uint8_t flags = crackdown::guest::Load8(base, character + kVehicleFlagsOffset);
    const bool driving = (flags & kDrivingFlags) == kDrivingFlags;
    frame.vehicle = driving ? Load32(base, character + kVehicleOffset) : 0;
    CopyGuest(base, frame.vehicle, frame.vehicle_bytes.data(), frame.vehicle_bytes.size());
    g_next = (g_next + 1) % kTelemetryFrames;
    g_count = std::min(g_count + 1, kTelemetryFrames);
    // The first non-finite position keeps the seconds that produced it.
    static bool dumped_nan = false;
    if (!dumped_nan && !std::isfinite(crackdown::guest::LoadFloat(base, character + kPositionOffset))) {
      dumped_nan = true;
      REXLOG_WARN("[TELEMETRY] the player's position is not finite");
      RequestTelemetryDump();
    }
  }
  if (g_dump_requested.exchange(false, std::memory_order_relaxed)) {
    Dump();
  }
}

void crackdown::RequestTelemetryDump() {
  if (!REXCVAR_GET(crackdown_physics_telemetry)) {
    return;
  }
  g_dump_requested.store(true, std::memory_order_relaxed);
}

bool crackdown::PhysicsTelemetryEnabled() {
  return REXCVAR_GET(crackdown_physics_telemetry);
}
