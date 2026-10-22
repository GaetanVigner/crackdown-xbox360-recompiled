// HLE renderer milestone 1, game side: see hle_capture.h. Device layout from the reverse engineering of the
// statically linked XDK D3D library (HLE_RENDERER_PLAN.md): r3 of every draw is the device; +48 is the command
// buffer write pointer (the last dword written), +13076 the one of BeginVertices/BeginIndexedVertices until
// EndVertices; every dirty state group is flushed before the DRAW packet, so the shadow right after the call is the
// register state of that draw.

#include "hle_capture.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include "guest_memory.h"
#include "hang_watchdog.h"
#include "platform.h"

REXCVAR_DEFINE_BOOL(crackdown_hle_capture, false, "Crackdown",
                    "Developer diagnostics (HLE renderer): F8 records the next frames from the D3D device (game side) "
                    "and the GPU command processor to logs/hle_game_<n>.bin and logs/hle_gpu_<n>.bin");
REXCVAR_DEFINE_INT32(crackdown_hle_capture_frames, 3, "Crackdown", "Frames an F8 HLE capture records on the GPU side");

namespace crackdown {
namespace {

using guest::Load32;

constexpr uint32_t kTraceVersion = 2;
constexpr uint32_t kUnknownRegister = 0xCDCDCDCD;

// The register blocks of SDK patch 0106, in its order.
struct Block {
  uint32_t first;
  uint32_t count;
};
constexpr Block kBlocks[] = {{0x2000, 0x300}, {0x4000, 2048}, {0x4800, 192}, {0x4900, 40}};

// Shadow groups of the render-state block: register range -> device offset (the flush emitters' map).
struct ShadowGroup {
  uint32_t first_register;
  uint32_t count;
  uint32_t device_offset;
};
constexpr ShadowGroup kRenderStateGroups[] = {
    {0x2000, 0x10, 10240}, {0x2080, 3, 10304}, {0x2100, 0x15, 10316},
    {0x2180, 5, 10400},    {0x2200, 0xC, 10420}, {0x2280, 0x15, 10468},
};
constexpr uint32_t kConstantsOffset = 0x700;   // 0x4000-0x47FF, VS then PS float constants
constexpr uint32_t kFetchOffset = 1024;        // 0x4800-0x48BF
constexpr uint32_t kBoolLoopOffset = 9984;     // 0x4900-0x4927
constexpr uint32_t kWritePointer = 48;
constexpr uint32_t kBeginWritePointer = 13076;
constexpr uint32_t kBinMaskLow = 12428;
constexpr uint32_t kPixelShader = 12412;
constexpr uint32_t kVertexShader = 12416;
constexpr uint32_t kMaxCallDwords = 0x10000;

struct Capture {
  std::mutex mutex;
  std::FILE* file = nullptr;
  std::filesystem::path path;
  uint32_t frame = 0;
  uint32_t frames_left = 0;
  uint32_t sequence = 0;
  uint32_t draws = 0;
  uint32_t unparsed_calls = 0;
};
Capture g_capture;
std::atomic<bool> g_armed{false};
std::atomic<bool> g_recording{false};

// The physical address the D3D library writes for a guest virtual address.
uint32_t PhysicalAddress(uint32_t va) {
  return (va & 0x1FFFFFFF) + ((va >> 20) >= 0xE00 ? 0x1000 : 0);
}

void WriteU32(std::FILE* file, uint32_t value) { std::fwrite(&value, sizeof(value), 1, file); }

// Dword count and the first eight ucode dwords (host order) of the microcode at `va`.
void WriteShaderHead(std::FILE* file, uint8_t* base, uint32_t va, uint32_t dwords) {
  const bool valid = va && dwords && dwords < 0x10000;
  WriteU32(file, valid ? dwords : 0);
  for (uint32_t i = 0; i < 8; ++i) {
    WriteU32(file, valid && i < dwords ? Load32(base, va + 4 * i) : 0);
  }
}

void WritePixelShader(std::FILE* file, uint8_t* base, uint32_t device) {
  const uint32_t ps = Load32(base, device + kPixelShader);
  if (!guest::Plausible(ps)) {
    WriteShaderHead(file, base, 0, 0);
    return;
  }
  const uint32_t program = ps + Load32(base, ps + 64);
  WriteShaderHead(file, base, Load32(base, ps + 24) + Load32(base, program + 40), Load32(base, program + 44) / 4);
}

void WriteVertexShader(std::FILE* file, uint8_t* base, uint32_t device) {
  const uint32_t vs = Load32(base, device + kVertexShader);
  if (!guest::Plausible(vs)) {
    WriteShaderHead(file, base, 0, 0);
    return;
  }
  const uint32_t variant = (Load32(base, vs + 872) & 0x20) ? 1 : 0;
  const uint32_t program = vs + Load32(base, vs + 896 + 8 * variant);
  WriteShaderHead(file, base, Load32(base, vs + 32) + Load32(base, program + 872), Load32(base, program + 876) / 4);
}

void WriteRegisters(std::FILE* file, uint8_t* base, uint32_t device) {
  std::vector<uint32_t> render_state(0x300, kUnknownRegister);
  for (const ShadowGroup& group : kRenderStateGroups) {
    for (uint32_t i = 0; i < group.count; ++i) {
      render_state[group.first_register - 0x2000 + i] = Load32(base, device + group.device_offset + 4 * i);
    }
  }
  std::fwrite(render_state.data(), sizeof(uint32_t), render_state.size(), file);
  auto write_shadow = [&](uint32_t offset, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
      WriteU32(file, Load32(base, device + offset + 4 * i));
    }
  };
  write_shadow(kConstantsOffset, kBlocks[1].count);
  write_shadow(kFetchOffset, kBlocks[2].count);
  write_shadow(kBoolLoopOffset, kBlocks[3].count);
}

// One record per DRAW packet the call wrote (all with the shadow as it is after the call).
void RecordDraw(uint8_t* base, uint32_t device, uint32_t header_va, uint32_t header, uint32_t initiator,
                uint32_t index_base) {
  Capture& capture = g_capture;
  std::FILE* file = capture.file;
  WriteU32(file, 1);
  WriteU32(file, capture.frame);
  WriteU32(file, capture.sequence++);
  WriteU32(file, header_va ? PhysicalAddress(header_va) : 0);
  WriteU32(file, initiator);
  WriteU32(file, index_base);
  WriteU32(file, 0);  // index count (not compared)
  WriteU32(file, 0);  // index format (not compared)
  WriteU32(file, header & 1);
  WriteU32(file, Load32(base, device + kBinMaskLow));
  WriteU32(file, 0);
  WriteU32(file, 0);  // visibility-query kill (GPU side only)
  WriteVertexShader(file, base, device);
  WritePixelShader(file, base, device);
  WriteRegisters(file, base, device);
  ++capture.draws;
}

// Walks the PM4 the call wrote, (pre, post], and records its DRAW packets.
void RecordCall(uint8_t* base, uint32_t device, uint32_t pre, uint32_t post) {
  if (!post || post <= pre || (post - pre) / 4 > kMaxCallDwords) {
    // A command buffer segment switch (or nothing written): record the state without a packet address.
    ++g_capture.unparsed_calls;
    RecordDraw(base, device, 0, 0, 0, 0);
    return;
  }
  uint32_t address = pre + 4;
  while (address <= post) {
    const uint32_t header = Load32(base, address);
    const uint32_t type = header >> 30;
    const uint32_t count = ((header >> 16) & 0x3FFF) + 1;
    if (type == 3) {
      const uint32_t opcode = (header >> 8) & 0x7F;
      if (opcode == 0x22 && address + 12 <= post) {  // DRAW_INDX: viz query, initiator, index base, size
        RecordDraw(base, device, address, header, Load32(base, address + 8), Load32(base, address + 12));
      } else if (opcode == 0x36 && address + 4 <= post) {  // DRAW_INDX_2: initiator, then inline indices
        RecordDraw(base, device, address, header, Load32(base, address + 4), 0);
      }
      address += 4 * (1 + count);
    } else if (type == 0) {
      address += 4 * (1 + count);
    } else if (type == 1) {
      address += 4 * 3;
    } else {
      address += 4;
    }
  }
}

// RunCommandBuffer replays a recorded command buffer with INDIRECT_BUFFER packets; their draws were recorded long
// before (static geometry, at level load), so they are logged as ranges: u32 kind 4, frame, sequence, physical
// address, dword count.
void RecordIndirectBuffers(uint32_t pre, uint32_t post, uint8_t* base) {
  if (!post || post <= pre || (post - pre) / 4 > kMaxCallDwords) {
    return;
  }
  Capture& capture = g_capture;
  for (uint32_t address = pre + 4; address <= post;) {
    const uint32_t header = Load32(base, address);
    const uint32_t type = header >> 30;
    const uint32_t count = ((header >> 16) & 0x3FFF) + 1;
    if (type == 3 && ((header >> 8) & 0x7F) == 0x3F && address + 8 <= post) {
      WriteU32(capture.file, 4);
      WriteU32(capture.file, capture.frame);
      WriteU32(capture.file, capture.sequence);
      WriteU32(capture.file, Load32(base, address + 4) & 0x1FFFFFFF);
      WriteU32(capture.file, Load32(base, address + 8) & 0xFFFFF);
    }
    address += type == 3 || type == 0 ? 4 * (1 + count) : type == 1 ? 12 : 4;
  }
}

template <typename Original>
void CaptureDraw(PPCContext& ctx, uint8_t* base, bool begin_variant, Original original) {
  if (!g_recording.load(std::memory_order_relaxed)) {
    original();
    return;
  }
  const uint32_t device = ctx.r3.u32;
  const uint32_t pre = Load32(base, device + kWritePointer);
  original();
  std::lock_guard lock(g_capture.mutex);
  if (!g_capture.file) {
    return;
  }
  const uint32_t post = Load32(base, device + (begin_variant ? kBeginWritePointer : kWritePointer));
  RecordCall(base, device, pre, post);
}

std::filesystem::path NextPath(const std::filesystem::path& folder, uint32_t& index) {
  std::error_code ec;
  for (index = 1; index < 1000; ++index) {
    if (!std::filesystem::exists(folder / ("hle_game_" + std::to_string(index) + ".bin"), ec)) {
      break;
    }
  }
  return folder / ("hle_game_" + std::to_string(index) + ".bin");
}

}  // namespace

bool HleCaptureEnabled() {
  return REXCVAR_GET(crackdown_hle_capture);
}

bool StartHleCapture() {
  if (g_recording.load() || g_armed.exchange(true)) {
    return false;
  }
  REXLOG_WARN("[HLE] capture armed for the next frame");
  return true;
}

void HleCaptureFrame() {
  Capture& capture = g_capture;
  if (g_recording.load(std::memory_order_relaxed)) {
    std::lock_guard lock(capture.mutex);
    WriteU32(capture.file, 3);
    WriteU32(capture.file, capture.frame);
    WriteU32(capture.file, capture.sequence);
    ++capture.frame;
    capture.sequence = 0;
    if (--capture.frames_left == 0) {
      g_recording = false;
      std::fclose(capture.file);
      capture.file = nullptr;
      REXLOG_WARN("[HLE] game side: {} draws in {} frames ({} calls without a parsed packet) written to {}",
                  capture.draws, capture.frame, capture.unparsed_calls, capture.path.string());
    }
    return;
  }
  if (!g_armed.exchange(false)) {
    return;
  }
  // The session's logs folder (the hang watchdog's), or the port folder's logs before it starts.
  const std::filesystem::path folder =
      DiagnosticsLogsFolder().empty() ? PortFolder() / "logs" : DiagnosticsLogsFolder();
  std::error_code ec;
  std::filesystem::create_directories(folder, ec);
  uint32_t index = 0;
  std::lock_guard lock(capture.mutex);
  capture.path = NextPath(folder, index);
#if defined(_WIN32)
  if (_wfopen_s(&capture.file, capture.path.c_str(), L"wb") != 0) {
    capture.file = nullptr;
  }
#else
  capture.file = std::fopen(capture.path.c_str(), "wb");
#endif
  if (!capture.file) {
    REXLOG_ERROR("[HLE] cannot open {}", capture.path.string());
    return;
  }
  std::setvbuf(capture.file, nullptr, _IOFBF, 1 << 20);
  std::fwrite("GDTR", 4, 1, capture.file);
  WriteU32(capture.file, kTraceVersion);
  WriteU32(capture.file, uint32_t(std::size(kBlocks)));
  for (const Block& block : kBlocks) {
    WriteU32(capture.file, block.first);
    WriteU32(capture.file, block.count);
  }
  const int gpu_frames = std::max(1, REXCVAR_GET(crackdown_hle_capture_frames));
  // The game submits ahead of the GPU: record two more frames on this side so every GPU frame has its draws.
  capture.frames_left = uint32_t(gpu_frames + 2);
  capture.frame = 0;
  capture.sequence = 0;
  capture.draws = 0;
  capture.unparsed_calls = 0;
  const std::filesystem::path gpu_path = folder / ("hle_gpu_" + std::to_string(index) + ".bin");
  rex::cvar::SetFlagByName("gpu_draw_trace_frames", std::to_string(gpu_frames));
  rex::cvar::SetFlagByName("gpu_draw_trace", gpu_path.string());
  g_recording = true;
  REXLOG_WARN("[HLE] capturing {} game frames to {} and {} GPU frames to {}", capture.frames_left,
              capture.path.string(), gpu_frames, gpu_path.string());
}

}  // namespace crackdown

// The four D3D draw entry points (r3 = device): DrawIndexedVertices, DrawVertices, BeginVertices,
// BeginIndexedVertices.
REX_EXTERN(__imp__sub_826B9FF8);
REX_HOOK_RAW(sub_826B9FF8) {
  crackdown::CaptureDraw(ctx, base, false, [&] { __imp__sub_826B9FF8(ctx, base); });
}

REX_EXTERN(__imp__sub_826B9C08);
REX_HOOK_RAW(sub_826B9C08) {
  crackdown::CaptureDraw(ctx, base, false, [&] { __imp__sub_826B9C08(ctx, base); });
}

REX_EXTERN(__imp__sub_826B91E0);
REX_HOOK_RAW(sub_826B91E0) {
  crackdown::CaptureDraw(ctx, base, true, [&] { __imp__sub_826B91E0(ctx, base); });
}

// RunCommandBuffer (r3 = device).
REX_EXTERN(__imp__sub_826C3750);
REX_HOOK_RAW(sub_826C3750) {
  if (!crackdown::g_recording.load(std::memory_order_relaxed)) {
    __imp__sub_826C3750(ctx, base);
    return;
  }
  const uint32_t device = ctx.r3.u32;
  const uint32_t pre = crackdown::guest::Load32(base, device + crackdown::kWritePointer);
  __imp__sub_826C3750(ctx, base);
  std::lock_guard lock(crackdown::g_capture.mutex);
  if (crackdown::g_capture.file) {
    crackdown::RecordIndirectBuffers(pre, crackdown::guest::Load32(base, device + crackdown::kWritePointer), base);
  }
}

REX_EXTERN(__imp__sub_826B96D0);
REX_HOOK_RAW(sub_826B96D0) {
  crackdown::CaptureDraw(ctx, base, true, [&] { __imp__sub_826B96D0(ctx, base); });
}
