// HLE renderer milestones 1-2, game side: see hle_capture.h. Device layout from the reverse engineering of the
// statically linked XDK D3D library (HLE_RENDERER_PLAN.md): r3 of every draw is the device; +48 is the command
// buffer write pointer (the last dword written), +13076 the one of BeginVertices/BeginIndexedVertices until
// EndVertices; every dirty state group is flushed before the DRAW packet, so the shadow plus the call's own packets
// (walked in order, INDIRECT_BUFFERs followed) is the register state of each draw.

#include "hle_capture.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/kernel_state.h>

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
// The end limit of the ring segment being written; a call that changes it switched segments (sub_826BB408), so
// (pre, post] is not one contiguous run of its packets.
constexpr uint32_t kSegmentEnd = 56;
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
  // The ring position the walk has reached (guest virtual, the last dword walked; 0 unknown) and the end of its
  // segment. PM4 written between hooked calls (constant writes of unhooked D3D functions) is walked from there.
  uint32_t cursor = 0;
  uint32_t cursor_segment_end = 0;
  uint32_t gap_dwords = 0;
  uint32_t gap_draws = 0;
};
Capture g_capture;

// Where the draw being recorded comes from, written in the record's (GPU-side) visibility-query field: the
// comparison reports each source apart. Set by the hooks, under the capture mutex.
enum class Source : uint32_t { kDraw = 0, kCommandBuffer = 1, kGprFlush = 2, kClear = 3, kResolve = 4 };
Source g_source = Source::kDraw;
std::atomic<bool> g_armed{false};
std::atomic<bool> g_recording{false};

// The physical address the D3D library writes for a guest virtual address.
uint32_t PhysicalAddress(uint32_t va) {
  return (va & 0x1FFFFFFF) + ((va >> 20) >= 0xE00 ? 0x1000 : 0);
}

void WriteU32(std::FILE* file, uint32_t value) { std::fwrite(&value, sizeof(value), 1, file); }


// Microcode identity: dword count and the first eight ucode dwords, host order (what the GPU side records).
struct ShaderHead {
  uint32_t dwords = 0;
  uint32_t head[8] = {};
};

// The register state of one draw, in the trace's block layout. Render-state registers no shadow group covers and no
// packet wrote stay kUnknownRegister, which the comparison skips.
struct RegisterImage {
  std::vector<uint32_t> values;
  ShaderHead vs, ps;

  RegisterImage() {
    uint32_t total = 0;
    for (const Block& block : kBlocks) total += block.count;
    values.assign(total, kUnknownRegister);
  }

  uint32_t* Find(uint32_t reg) {
    uint32_t position = 0;
    for (const Block& block : kBlocks) {
      if (reg >= block.first && reg < block.first + block.count) {
        return &values[position + reg - block.first];
      }
      position += block.count;
    }
    return nullptr;
  }

  void Set(uint32_t reg, uint32_t value) {
    if (uint32_t* slot = Find(reg)) *slot = value;
  }
};

// The float constants the command stream has written since the capture started, as the GPU's register file holds
// them. The shadow alone is not enough: shader literal constants are loaded (LOAD_ALU_CONSTANT) only when a shader
// is bound, so later draws with the same shader keep them on the GPU while neither the shadow nor their own packets
// carry them. Constants written by predicated packets (per tile) are kUnknownRegister: they differ between tiles.
struct GpuFloatConstants {
  static constexpr uint32_t kFirst = 0x4000;
  static constexpr uint32_t kCount = 2048;
  uint32_t values[kCount];
  bool seen[kCount];

  void Clear() { std::fill(std::begin(seen), std::end(seen), false); }
  void Note(uint32_t reg, uint32_t value) {
    if (reg >= kFirst && reg < kFirst + kCount) {
      values[reg - kFirst] = value;
      seen[reg - kFirst] = true;
    }
  }
};
GpuFloatConstants g_gpu_constants;

// Guest physical memory, where command buffers, ring segments and shader microcode live (big-endian dwords).
uint32_t ReadPhysical(uint32_t physical) {
  const uint8_t* host = rex::system::kernel_state()->memory()->physical_membase() + (physical & 0x1FFFFFFF);
  uint32_t value;
  std::memcpy(&value, host, sizeof(value));
  return std::byteswap(value);
}

ShaderHead ShaderFromGuest(uint8_t* base, uint32_t va, uint32_t dwords) {
  ShaderHead shader;
  if (!va || !dwords || dwords >= 0x10000) return shader;
  shader.dwords = dwords;
  for (uint32_t i = 0; i < 8 && i < dwords; ++i) shader.head[i] = Load32(base, va + 4 * i);
  return shader;
}

ShaderHead ShaderFromPhysical(uint32_t physical, uint32_t dwords) {
  ShaderHead shader;
  if (!dwords || dwords >= 0x10000) return shader;
  shader.dwords = dwords;
  for (uint32_t i = 0; i < 8 && i < dwords; ++i) shader.head[i] = ReadPhysical(physical + 4 * i);
  return shader;
}

ShaderHead DevicePixelShader(uint8_t* base, uint32_t device) {
  const uint32_t ps = Load32(base, device + kPixelShader);
  if (!guest::Plausible(ps)) return {};
  const uint32_t program = ps + Load32(base, ps + 64);
  return ShaderFromGuest(base, Load32(base, ps + 24) + Load32(base, program + 40), Load32(base, program + 44) / 4);
}

ShaderHead DeviceVertexShader(uint8_t* base, uint32_t device) {
  const uint32_t vs = Load32(base, device + kVertexShader);
  if (!guest::Plausible(vs)) return {};
  const uint32_t variant = (Load32(base, vs + 872) & 0x20) ? 1 : 0;
  const uint32_t program = vs + Load32(base, vs + 896 + 8 * variant);
  return ShaderFromGuest(base, Load32(base, vs + 32) + Load32(base, program + 872), Load32(base, program + 876) / 4);
}

// The device shadow: what the GPU registers hold before the call's own packets.
RegisterImage ImageFromShadow(uint8_t* base, uint32_t device) {
  RegisterImage image;
  for (const ShadowGroup& group : kRenderStateGroups) {
    for (uint32_t i = 0; i < group.count; ++i) {
      image.Set(group.first_register + i, Load32(base, device + group.device_offset + 4 * i));
    }
  }
  for (uint32_t i = 0; i < kBlocks[1].count; ++i) image.Set(0x4000 + i, Load32(base, device + kConstantsOffset + 4 * i));
  for (uint32_t i = 0; i < kBlocks[2].count; ++i) image.Set(0x4800 + i, Load32(base, device + kFetchOffset + 4 * i));
  for (uint32_t i = 0; i < kBlocks[3].count; ++i) image.Set(0x4900 + i, Load32(base, device + kBoolLoopOffset + 4 * i));
  for (uint32_t i = 0; i < GpuFloatConstants::kCount; ++i) {
    if (g_gpu_constants.seen[i]) image.Set(GpuFloatConstants::kFirst + i, g_gpu_constants.values[i]);
  }
  image.vs = DeviceVertexShader(base, device);
  image.ps = DevicePixelShader(base, device);
  return image;
}

// True while walking the PM4 between hooked calls: its writes reach the GPU's running copy, its draws are only counted.
bool g_gap_walk = false;

// Diagnostics: the first time a float constant receives a PM4 type-3 DRAW_INDX header, the packet that wrote it is
// logged (a walk that lost its alignment).
uint32_t g_walk_packet = 0;
uint32_t g_walk_header = 0;
uint32_t g_walk_depth = 0;

// A register write the walk applies: to the draw's image and, for float constants, to the GPU's running copy.
void Apply(RegisterImage& image, uint32_t reg, uint32_t value, bool predicated = false) {
  static bool logged = false;
  const uint32_t opcode = (value >> 8) & 0x7F;
  if (!logged && reg >= 0x4000 && reg < 0x4800 && (value & 0xC00080FE) == 0xC0000000 &&
      ((value >> 16) & 0x3FFF) < 8 && (opcode == 0x22 || opcode == 0x36)) {
    logged = true;
    REXLOG_WARN("[HLE] float constant 0x{:04X} set to {:08X} by the packet {:08X} at physical {:08X} (depth {}, source {})",
                reg, value, g_walk_header, g_walk_packet, g_walk_depth, uint32_t(g_source));
  }
  const uint32_t applied = predicated ? kUnknownRegister : value;
  image.Set(reg, applied);
  g_gpu_constants.Note(reg, applied);
}

void WriteShader(std::FILE* file, const ShaderHead& shader) {
  WriteU32(file, shader.dwords);
  for (uint32_t dword : shader.head) WriteU32(file, dword);
}

// Record kinds: 1 draw, 2 resolve (EDRAM copy mode), as on the GPU side.
void RecordDraw(uint8_t* base, uint32_t device, const RegisterImage& image, uint32_t packet_physical,
                uint32_t header, uint32_t initiator, uint32_t index_base) {
  Capture& capture = g_capture;
  if (g_gap_walk) {
    ++capture.gap_draws;
    return;
  }
  std::FILE* file = capture.file;
  const uint32_t mode = image.values[0x208] & 7;  // RB_MODECONTROL (0x2208) edram_mode
  WriteU32(file, mode == 6 ? 2 : 1);
  WriteU32(file, capture.frame);
  WriteU32(file, capture.sequence++);
  WriteU32(file, packet_physical);
  WriteU32(file, initiator);
  WriteU32(file, index_base);
  WriteU32(file, 0);  // index count (not compared)
  WriteU32(file, 0);  // index format (not compared)
  WriteU32(file, header & 1);
  WriteU32(file, Load32(base, device + kBinMaskLow));
  WriteU32(file, 0);
  WriteU32(file, uint32_t(g_source));  // the GPU side's visibility-query kill field
  WriteShader(file, image.vs);
  WriteShader(file, image.ps);
  std::fwrite(image.values.data(), sizeof(uint32_t), image.values.size(), file);
  ++capture.draws;
}

// Base register of SET_CONSTANT / LOAD_ALU_CONSTANT types: ALU, fetch, bool, loop, registers.
uint32_t ConstantBase(uint32_t type) {
  switch (type) {
    case 0: return 0x4000;
    case 1: return 0x4800;
    case 2: return 0x4900;
    case 3: return 0x4908;
    case 4: return 0x2000;
    default: return 0;
  }
}

constexpr uint32_t kMaxIndirectDepth = 4;

// Walks `dwords` of PM4 at a guest physical address, applying the register and constant writes to `image` in order,
// following INDIRECT_BUFFERs, and records every DRAW packet with the state it executes with. Writes of predicated
// packets (per-tile values of predicated tiling) leave their registers unknown; the comparison skips those.
void WalkPm4(uint8_t* base, uint32_t device, RegisterImage& image, uint32_t physical, uint32_t dwords, uint32_t depth) {
  uint32_t i = 0;
  while (i < dwords) {
    const uint32_t at = physical + 4 * i;
    const uint32_t header = ReadPhysical(at);
    g_walk_packet = at;
    g_walk_header = header;
    g_walk_depth = depth;
    const uint32_t type = header >> 30;
    auto data = [&](uint32_t n) { return ReadPhysical(at + 4 * (1 + n)); };
    if (type == 0) {
      const uint32_t count = ((header >> 16) & 0x3FFF) + 1;
      const uint32_t first = header & 0x7FFF;
      const bool one_register = (header >> 15) & 1;
      for (uint32_t m = 0; m < count && i + 1 + m < dwords; ++m) Apply(image, one_register ? first : first + m, data(m));
      i += 1 + count;
      continue;
    }
    if (type == 1) {
      Apply(image, header & 0x7FF, data(0));
      Apply(image, (header >> 11) & 0x7FF, data(1));
      i += 3;
      continue;
    }
    if (type == 2) {
      i += 1;
      continue;
    }
    const uint32_t count = ((header >> 16) & 0x3FFF) + 1;
    const uint32_t opcode = (header >> 8) & 0x7F;
    const bool predicated = header & 1;
    switch (opcode) {
      case 0x2D: {  // SET_CONSTANT
        const uint32_t offset_type = data(0);
        const uint32_t first = ConstantBase((offset_type >> 16) & 0xFF) + (offset_type & 0x7FF);
        for (uint32_t k = 0; k + 1 < count; ++k) Apply(image, first + k, data(1 + k), predicated);
        break;
      }
      case 0x2F: {  // LOAD_ALU_CONSTANT: constants from memory
        const uint32_t address = data(0) & 0x3FFFFFFF;
        const uint32_t offset_type = data(1);
        const uint32_t size = data(2) & 0xFFF;
        const uint32_t first = ConstantBase((offset_type >> 16) & 0xFF) + (offset_type & 0x7FF);
        for (uint32_t k = 0; k < size; ++k) Apply(image, first + k, ReadPhysical(address + 4 * k), predicated);
        break;
      }
      case 0x27: {  // IM_LOAD: shader microcode from memory
        const uint32_t address_type = data(0);
        const ShaderHead shader = ShaderFromPhysical(address_type & ~3u, data(1) & 0xFFFF);
        if ((address_type & 3) == 0) image.vs = shader;
        if ((address_type & 3) == 1) image.ps = shader;
        break;
      }
      case 0x2B: {  // IM_LOAD_IMMEDIATE: inline microcode
        const ShaderHead shader = ShaderFromPhysical(at + 12, data(1) & 0xFFFF);
        if ((data(0) & 3) == 0) image.vs = shader;
        if ((data(0) & 3) == 1) image.ps = shader;
        break;
      }
      case 0x22:  // DRAW_INDX: viz query, initiator, index base, size
        RecordDraw(base, device, image, at, header, data(1), data(2));
        break;
      case 0x36:  // DRAW_INDX_2: initiator, then inline indices
        RecordDraw(base, device, image, at, header, data(0), 0);
        break;
      case 0x37:  // INDIRECT_BUFFER_PFD
      case 0x3F:  // INDIRECT_BUFFER
        if (depth < kMaxIndirectDepth) {
          const uint32_t target = data(0) & 0x1FFFFFFF;
          const uint32_t target_dwords = data(1) & 0xFFFFF;
          // Logged as a range (u32 kind 4, frame, sequence, physical address, dwords), then walked.
          WriteU32(g_capture.file, 4);
          WriteU32(g_capture.file, g_capture.frame);
          WriteU32(g_capture.file, g_capture.sequence);
          WriteU32(g_capture.file, target);
          WriteU32(g_capture.file, target_dwords);
          WalkPm4(base, device, image, target, target_dwords, depth + 1);
        }
        break;
      default:
        break;
    }
    i += 1 + count;
  }
}

// Walks the PM4 a D3D call wrote into the ring, (pre, post] (guest virtual, the last dword written), on top of the
// device shadow after the call. A ring segment switch (post outside the range) records the shadow alone.
bool RecordCall(uint8_t* base, uint32_t device, uint32_t pre, uint32_t post, bool same_segment,
                bool record_unparsed = true) {
  RegisterImage image = ImageFromShadow(base, device);
  if (!same_segment || !post || post <= pre || (post - pre) / 4 > kMaxCallDwords) {
    ++g_capture.unparsed_calls;
    if (record_unparsed) RecordDraw(base, device, image, 0, 0, 0, 0);
    return false;
  }
  WalkPm4(base, device, image, PhysicalAddress(pre + 4), (post - pre) / 4, 0);
  return true;
}

// Walks the ring from the cursor to `pre`, the write pointer at the start of a hooked call, if both lie in the
// segment the cursor was left in.
void WalkGap(uint8_t* base, uint32_t device, uint32_t pre, uint32_t segment_end) {
  Capture& capture = g_capture;
  if (!capture.cursor || segment_end != capture.cursor_segment_end || pre <= capture.cursor ||
      (pre - capture.cursor) / 4 > kMaxCallDwords) {
    return;
  }
  RegisterImage image = ImageFromShadow(base, device);
  g_gap_walk = true;
  WalkPm4(base, device, image, PhysicalAddress(capture.cursor + 4), (pre - capture.cursor) / 4, 0);
  g_gap_walk = false;
  capture.gap_dwords += (pre - capture.cursor) / 4;
}

// Moves the cursor to the end of a hooked call's output. After a Begin* call the ring holds the vertices the game
// writes next, not PM4: the cursor is unknown until the next call ends.
void AdvanceCursor(uint8_t* base, uint32_t device, uint32_t post) {
  g_capture.cursor = post;
  g_capture.cursor_segment_end = Load32(base, device + kSegmentEnd);
}

// RunCommandBuffer(dev, cb, predSelect) replays a command buffer stored in a mesh resource (precompiled PM4,
// relocated at load) with one INDIRECT_BUFFER per segment. Normally the walk of the PM4 the call wrote follows those
// INDIRECT_BUFFERs; when the call's output straddles a ring segment, this falls back to the segment list the
// reverse engineering found at cb+116 ({next, count, (physical address, dwords) x count}; unconfirmed: the first
// capture found no segment there, so its first words are logged once).
void RecordCommandBuffer(uint8_t* base, uint32_t device, uint32_t command_buffer) {
  static bool logged = false;
  if (!logged && guest::Plausible(command_buffer)) {
    logged = true;
    std::string words;
    for (uint32_t offset = 100; offset < 164; offset += 4) {
      words += fmt::format(" +{}={:08X}", offset, Load32(base, command_buffer + offset));
    }
    REXLOG_WARN("[HLE] command buffer {:08X}:{}", command_buffer, words);
  }
  constexpr uint32_t kSegmentList = 116;
  constexpr uint32_t kMaxNodes = 64;
  Capture& capture = g_capture;
  RegisterImage image = ImageFromShadow(base, device);
  uint32_t node = guest::Plausible(command_buffer) ? Load32(base, command_buffer + kSegmentList) : 0;
  for (uint32_t n = 0; n < kMaxNodes && guest::Plausible(node); ++n) {
    const uint32_t count = Load32(base, node + 4);
    for (uint32_t s = 0; s < count && s < 256; ++s) {
      const uint32_t physical = Load32(base, node + 8 + 8 * s) & 0x1FFFFFFF;
      const uint32_t dwords = Load32(base, node + 12 + 8 * s) & 0xFFFFF;
      WriteU32(capture.file, 4);
      WriteU32(capture.file, capture.frame);
      WriteU32(capture.file, capture.sequence);
      WriteU32(capture.file, physical);
      WriteU32(capture.file, dwords);
      WalkPm4(base, device, image, physical, dwords, 1);
    }
    node = Load32(base, node);
  }
}

template <typename Original>
void CaptureDraw(PPCContext& ctx, uint8_t* base, bool begin_variant, Original original, Source source = Source::kDraw) {
  if (!g_recording.load(std::memory_order_relaxed)) {
    original();
    return;
  }
  const uint32_t device = ctx.r3.u32;
  const uint32_t pre = Load32(base, device + kWritePointer);
  const uint32_t segment_end = Load32(base, device + kSegmentEnd);
  {
    std::lock_guard lock(g_capture.mutex);
    if (g_capture.file) WalkGap(base, device, pre, segment_end);
  }
  original();
  std::lock_guard lock(g_capture.mutex);
  if (!g_capture.file) {
    return;
  }
  const uint32_t post = Load32(base, device + (begin_variant ? kBeginWritePointer : kWritePointer));
  g_source = source;
  RecordCall(base, device, pre, post, Load32(base, device + kSegmentEnd) == segment_end);
  g_source = Source::kDraw;
  AdvanceCursor(base, device, begin_variant ? 0 : post);
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
      REXLOG_WARN("[HLE] game side: {} draws in {} frames ({} calls without a parsed packet; {} dwords and {} draws "
                  "between hooked calls) written to {}",
                  capture.draws, capture.frame, capture.unparsed_calls, capture.gap_dwords, capture.gap_draws,
                  capture.path.string());
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
  capture.cursor = 0;
  capture.gap_dwords = 0;
  capture.gap_draws = 0;
  g_gpu_constants.Clear();
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

REX_EXTERN(__imp__sub_826B96D0);
REX_HOOK_RAW(sub_826B96D0) {
  crackdown::CaptureDraw(ctx, base, true, [&] { __imp__sub_826B96D0(ctx, base); });
}

// D3D calls that draw with their own state written straight into the ring (the walk applies it): the clear
// (r3 = device, its rect draws are DRAW_INDX_2), the resolve (an EDRAM copy draw), and SetShaderGPRAllocation (three
// INDIRECT_BUFFERs into the device's static block: setup, SQ_GPR_MANAGEMENT, 24 invisible points).
REX_EXTERN(__imp__sub_826C04A0);
REX_HOOK_RAW(sub_826C04A0) {
  crackdown::CaptureDraw(ctx, base, false, [&] { __imp__sub_826C04A0(ctx, base); }, crackdown::Source::kClear);
}

REX_EXTERN(__imp__sub_826BE578);
REX_HOOK_RAW(sub_826BE578) {
  crackdown::CaptureDraw(ctx, base, false, [&] { __imp__sub_826BE578(ctx, base); }, crackdown::Source::kResolve);
}

REX_EXTERN(__imp__sub_826B3C48);
REX_HOOK_RAW(sub_826B3C48) {
  crackdown::CaptureDraw(ctx, base, false, [&] { __imp__sub_826B3C48(ctx, base); }, crackdown::Source::kGprFlush);
}

// RunCommandBuffer(r3 = device, r4 = command buffer, r5 = predicate select).
REX_EXTERN(__imp__sub_826C3750);
REX_HOOK_RAW(sub_826C3750) {
  const uint32_t device = ctx.r3.u32;
  const uint32_t pre = crackdown::guest::Load32(base, device + crackdown::kWritePointer);
  const uint32_t segment_end = crackdown::guest::Load32(base, device + crackdown::kSegmentEnd);
  if (!crackdown::g_recording.load(std::memory_order_relaxed)) {
    __imp__sub_826C3750(ctx, base);
    return;
  }
  const uint32_t command_buffer = ctx.r4.u32;
  {
    std::lock_guard lock(crackdown::g_capture.mutex);
    if (crackdown::g_capture.file) crackdown::WalkGap(base, device, pre, segment_end);
  }
  __imp__sub_826C3750(ctx, base);
  std::lock_guard lock(crackdown::g_capture.mutex);
  if (crackdown::g_capture.file) {
    crackdown::g_source = crackdown::Source::kCommandBuffer;
    const uint32_t post = crackdown::guest::Load32(base, device + crackdown::kWritePointer);
    const bool same_segment = crackdown::guest::Load32(base, device + crackdown::kSegmentEnd) == segment_end;
    if (!crackdown::RecordCall(base, device, pre, post, same_segment, false)) {
      crackdown::RecordCommandBuffer(base, device, command_buffer);
    }
    crackdown::AdvanceCursor(base, device, post);
    crackdown::g_source = crackdown::Source::kDraw;
  }
}
