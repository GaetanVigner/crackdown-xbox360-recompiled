// Main scene in one predicated tile (crackdown_single_tile_scene). The title renders its scene with the XDK's
// predicated tiling because two 1280x720 2x MSAA targets (color A8R8G8B8 + depth D24FS8, 976 + 976 EDRAM tiles a
// 484-row tile) do not fit the 10 MB EDRAM: sub_82326A90 brackets the scene with BeginTiling(count 2, rects
// {0,0,1280,484} {0,480,1280,720}) and the GPU replays every draw of the bracket once per tile. At 1x MSAA the
// whole frame fits (720 + 720 EDRAM tiles), so:
// - the post-process object (sub_825A76E0), sub_82326728 and the shader parameters computed at startup (a
//   size from the tile rects: at the title's entry point (xstart) their first rect becomes {0,0,1280,720} (seven .data copies of the table) and
//   the 4-row tile overlap sub_825A0280 scales its texcoords by (0x82BA7048) becomes 0;
// - CreateRenderTarget sub_826AED90 creates those two targets at 1x MSAA (r6 0 instead of 1);
// - BeginTiling sub_826BD488 gets count 1 for the scene bracket;
// - the title's own per-tile loops still run their tile-1 iteration: after SetPredication sub_826BD300 with a mask
//   holding no tile-0 bit (3 << 0, or bit 31 which the tile-0 select carries), its draws, resolves and clears are
//   skipped (SingleTileSuppressed; SetPredication(0) would turn predication off, not drop them). EndTiling
//   sub_826BDA38 ends the bracket.
// - the precompiled command buffers (mesh assets, replayed by RunCommandBuffer sub_826C3750) carry their own
//   predicated per-tile window scissor writes, authored for the two-tile layout: the first time a segment is
//   replayed, its tile-0 scissor bottom-right (1280x484) becomes 1280x720. Mesh streaming loads new command buffers
//   (fixed up by sub_826C3D90), possibly at addresses scanned before, so every load starts the scan over.
// - pixel shaders carry a compiled-in texel size for the 484-row tile (1/484 among their literal constants, loaded
//   with LOAD_ALU_CONSTANT from the shader object into PS c252..c255; the ghost copies at y * 484/720 otherwise):
//   when the D3D library binds a shader (sub_826C6A50 emits one LOAD_ALU_CONSTANT per entry of the shader's literal
//   table) and when a command buffer segment loads them, their 1/484 values become 1/720.
// The shadow atlas (4 tiles, sub_825C5790) is left as it is.

#include "single_tile_scene.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <unordered_set>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/kernel_state.h>

#include "guest_memory.h"

REXCVAR_DEFINE_BOOL(crackdown_single_tile_scene, false, "Crackdown",
                    "Experimental: render the main scene at 1x MSAA in one predicated tile instead of two 2x MSAA "
                    "tiles; read once, when the scene render targets are first created");

namespace crackdown {
namespace {

using guest::Load32;
using guest::Store32;

// The scene tile rect tables (two D3DRECTs each), identical in .data.
constexpr uint32_t kRectTables[] = {0x82BA7028, 0x82BA7070, 0x82BA7AE4, 0x82BA8548,
                                    0x82BA9010, 0x82BAA334, 0x82BAA368};
constexpr uint32_t kTwoTiles[8] = {0, 0, 1280, 484, 0, 480, 1280, 720};
constexpr uint32_t kTileOverlap = 0x82BA7048;  // rows, per mode, after the first table
constexpr uint32_t kSceneRects = 0x82BAA334;  // BeginTiling's rects in sub_82326A90 (mode 0)
constexpr uint32_t kSceneColorFormat = 0x18280186;
constexpr uint32_t kSceneDepthFormat = 0x1A220197;
constexpr uint32_t kTileZeroBits = 0x80000003;
constexpr uint32_t kWindowScissorBr = 0x2082;
constexpr uint32_t kTileZeroScissorBr = 0x01E40500;  // y 484, x 1280
constexpr uint32_t kSceneScissorBr = 0x02D00500;     // y 720, x 1280
constexpr uint32_t kWritePointer = 48;               // device: the last ring dword written
constexpr uint32_t kSegmentEnd = 56;                 // device: the end of the ring segment being written
constexpr uint32_t kMaxIndirectDepth = 4;

enum class State { kUnset, kOff, kOn };
State g_state = State::kUnset;
bool g_creating_targets = false;  // inside sub_82326728
bool g_in_bracket = false;        // between the scene's BeginTiling and EndTiling
bool g_suppressed = false;        // the current predication selects tile 1 only
uint32_t g_suppressed_calls = 0;
std::unordered_set<uint32_t> g_scanned_segments;  // command buffer segments already patched (physical addresses)
uint32_t g_patched_scissors = 0;
uint32_t g_patched_literals = 0;
std::atomic<uint32_t> g_loads{0};  // command buffer fixups so far (loader threads)
uint32_t g_scanned_loads = 0;      // g_loads when g_scanned_segments was last cleared

bool Enabled() { return g_state == State::kOn; }

// Rewrites the first rect of every table, once; all tables are checked first so that a different build of the title
// is left alone.
bool PatchRectTables(uint8_t* base) {
  for (uint32_t table : kRectTables) {
    for (uint32_t i = 0; i < 8; ++i) {
      if (Load32(base, table + 4 * i) != kTwoTiles[i]) {
        REXLOG_WARN("[SINGLE_TILE] tile rects at {:08X} are not the expected ones; single-tile scene disabled", table);
        return false;
      }
    }
  }
  if (Load32(base, kTileOverlap) != 4) {
    REXLOG_WARN("[SINGLE_TILE] tile overlap at {:08X} is not 4; single-tile scene disabled", kTileOverlap);
    return false;
  }
  for (uint32_t table : kRectTables) Store32(base, table + 12, 720);
  Store32(base, kTileOverlap, 0);
  return true;
}

// The physical address the D3D library writes for a guest virtual address.
uint32_t PhysicalAddress(uint32_t va) { return (va & 0x1FFFFFFF) + ((va >> 20) >= 0xE00 ? 0x1000 : 0); }

uint8_t* PhysicalHost(uint32_t physical) {
  return rex::system::kernel_state()->memory()->physical_membase() + (physical & 0x1FFFFFFF);
}

uint32_t ReadPhysical(uint32_t physical) {
  uint32_t value;
  std::memcpy(&value, PhysicalHost(physical), sizeof(value));
  return std::byteswap(value);
}

void PatchScissor(uint32_t physical, uint32_t reg) {
  if (reg != kWindowScissorBr || ReadPhysical(physical) != kTileZeroScissorBr) {
    return;
  }
  const uint32_t value = std::byteswap(kSceneScissorBr);
  std::memcpy(PhysicalHost(physical), &value, sizeof(value));
  ++g_patched_scissors;
  if (g_patched_scissors == 1 || g_patched_scissors == 100 || g_patched_scissors == 1000) {
    REXLOG_INFO("[SINGLE_TILE] {} command buffer scissors set to 1280x720 ({} segments scanned)", g_patched_scissors,
                g_scanned_segments.size());
  }
}

// A float within 0.3% of 1/484 (the two-tile scene height).
bool IsTileTexelSize(uint32_t bits);

// Physical memory is 512 MB: a misparsed packet can name an unaligned address whose dword crosses its end.
constexpr uint32_t kPhysicalSize = 0x20000000;
bool PlausibleRange(uint32_t physical, uint32_t dwords) {
  return (physical & 3) == 0 && physical < kPhysicalSize && dwords <= (kPhysicalSize - physical) / 4;
}

void NoteLiteral(uint32_t physical) {
  ++g_patched_literals;
  if (g_patched_literals <= 4 || g_patched_literals == 100) {
    REXLOG_INFO("[SINGLE_TILE] shader literal 1/484 at physical {:08X} set to 1/720 ({} so far)", physical,
                g_patched_literals);
  }
}

// The literal constants a LOAD_ALU_CONSTANT reads (`size` dwords of guest physical memory): 1/484 becomes 1/720.
void PatchLiterals(uint32_t physical, uint32_t size) {
  if (!PlausibleRange(physical, size)) {
    return;
  }
  for (uint32_t k = 0; k < size; ++k) {
    if (!IsTileTexelSize(ReadPhysical(physical + 4 * k))) continue;
    const uint32_t value = std::byteswap(std::bit_cast<uint32_t>(1.0f / 720.0f));
    std::memcpy(PhysicalHost(physical + 4 * k), &value, sizeof(value));
    NoteLiteral(physical + 4 * k);
  }
}

// Patches the tile-0 window scissor writes (type-0 or SET_CONSTANT) of `dwords` of PM4, following INDIRECT_BUFFERs
// into segments not scanned yet.
void ScanPm4(uint32_t physical, uint32_t dwords, uint32_t depth) {
  if (!PlausibleRange(physical, dwords)) {
    return;
  }
  uint32_t i = 0;
  while (i < dwords) {
    const uint32_t at = physical + 4 * i;
    const uint32_t header = ReadPhysical(at);
    const uint32_t type = header >> 30;
    if (type == 0) {
      const uint32_t count = ((header >> 16) & 0x3FFF) + 1;
      const uint32_t first = header & 0x7FFF;
      const bool one_register = (header >> 15) & 1;
      for (uint32_t m = 0; m < count && i + 1 + m < dwords; ++m) {
        PatchScissor(at + 4 * (1 + m), one_register ? first : first + m);
      }
      i += 1 + count;
      continue;
    }
    if (type == 1) {
      i += 3;
      continue;
    }
    if (type == 2) {
      i += 1;
      continue;
    }
    const uint32_t count = ((header >> 16) & 0x3FFF) + 1;
    const uint32_t opcode = (header >> 8) & 0x7F;
    if (opcode == 0x2D) {  // SET_CONSTANT
      const uint32_t offset_type = ReadPhysical(at + 4);
      if (((offset_type >> 16) & 0xFF) == 4) {  // registers
        const uint32_t first = 0x2000 + (offset_type & 0x7FF);
        for (uint32_t k = 0; k + 1 < count && i + 2 + k < dwords; ++k) PatchScissor(at + 4 * (2 + k), first + k);
      }
    } else if (opcode == 0x2F && i + 3 < dwords) {  // LOAD_ALU_CONSTANT: address, offset/type, size
      if (((ReadPhysical(at + 8) >> 16) & 0xFF) == 0) PatchLiterals(ReadPhysical(at + 4) & 0x3FFFFFFF,
                                                                     ReadPhysical(at + 12) & 0xFFF);
    } else if ((opcode == 0x3F || opcode == 0x37) && depth < kMaxIndirectDepth) {  // INDIRECT_BUFFER(_PFD)
      const uint32_t target = ReadPhysical(at + 4) & 0x1FFFFFFF;
      if (!g_scanned_segments.contains(target)) {
        g_scanned_segments.insert(target);
        ScanPm4(target, ReadPhysical(at + 8) & 0xFFFFF, depth + 1);
      }
    }
    i += 1 + count;
  }
}

// A float within 0.3% of 1/484 (the two-tile scene height).
bool IsTileTexelSize(uint32_t bits) {
  const float value = std::bit_cast<float>(bits);
  return std::fabs(value * 484.0f - 1.0f) < 0.003f;
}

// Decides once, at the first of the two creators of tile-sized resources.
void Latch(uint8_t* base, const char* first) {
  if (g_state != State::kUnset) {
    return;
  }
  g_state = REXCVAR_GET(crackdown_single_tile_scene) && PatchRectTables(base) ? State::kOn : State::kOff;
  if (Enabled()) REXLOG_INFO("[SINGLE_TILE] main scene: one tile, 1x MSAA (tile rects patched before {})", first);
}

}  // namespace

void SingleTileCommandBuffer(uint8_t* base, uint32_t device, uint32_t pre, uint32_t segment_end) {
  if (!Enabled()) {
    return;
  }
  const uint32_t post = Load32(base, device + kWritePointer);
  if (Load32(base, device + kSegmentEnd) != segment_end || post <= pre || (post - pre) / 4 > 0x10000) {
    return;
  }
  const uint32_t loads = g_loads.load(std::memory_order_acquire);
  if (loads != g_scanned_loads) {
    g_scanned_loads = loads;
    g_scanned_segments.clear();
  }
  ScanPm4(PhysicalAddress(pre + 4), (post - pre) / 4, 0);
}


bool SingleTileSuppressed() {
  if (!g_suppressed) {
    return false;
  }
  if (++g_suppressed_calls == 1) {
    REXLOG_INFO("[SINGLE_TILE] skipping the title's tile-1 draws, resolves and clears");
  }
  return true;
}

}  // namespace crackdown

// Creates the scene's color and depth targets once (guard byte 0x82DE25C6), sized from the tile rects.
REX_EXTERN(__imp__sub_82326728);
REX_HOOK_RAW(sub_82326728) {
  using namespace crackdown;
  Latch(base, "sub_82326728");
  g_creating_targets = true;
  __imp__sub_82326728(ctx, base);
  g_creating_targets = false;
}

// Constructor of the post-process object (stored at 0x82DE4160): tile-sized textures, half-res targets 640 x half
// the tile height, per-tile half-res textures, all sized from the tile rects.
REX_EXTERN(__imp__sub_825A76E0);
REX_HOOK_RAW(sub_825A76E0) {
  crackdown::Latch(base, "sub_825A76E0");
  __imp__sub_825A76E0(ctx, base);
}

// The title's entry point: the tile rects are patched before any title code reads them.
REX_EXTERN(__imp__xstart);
REX_HOOK_RAW(xstart) {
  crackdown::Latch(base, "xstart");
  __imp__xstart(ctx, base);
}

// CreateRenderTarget(r3 width, r4 height, r5 format, r6 multisample 0/1/2 = 1x/2x/4x, r7 params).
REX_EXTERN(__imp__sub_826AED90);
REX_HOOK_RAW(sub_826AED90) {
  using namespace crackdown;
  if (Enabled() && g_creating_targets && ctx.r6.u32 == 1 && ctx.r3.u32 == 1280 && ctx.r4.u32 == 720 &&
      (ctx.r5.u32 == kSceneColorFormat || ctx.r5.u32 == kSceneDepthFormat)) {
    ctx.r6.u64 = 0;
  }
  __imp__sub_826AED90(ctx, base);
}

// BeginTiling(r3 device, r4 flags, r5 count, r6 rects, r7 clear color, f1 z, r9 stencil).
REX_EXTERN(__imp__sub_826BD488);
REX_HOOK_RAW(sub_826BD488) {
  using namespace crackdown;
  if (Enabled() && ctx.r5.u32 == 2 && ctx.r6.u32 == kSceneRects) {
    ctx.r5.u64 = 1;
    g_in_bracket = true;
    g_suppressed = false;
  }
  __imp__sub_826BD488(ctx, base);
}

// EndTiling(r3 device, r4 flags, r5 resolve rects, ...).
REX_EXTERN(__imp__sub_826BDA38);
REX_HOOK_RAW(sub_826BDA38) {
  using namespace crackdown;
  const bool scene = g_in_bracket;
  g_suppressed = false;
  __imp__sub_826BDA38(ctx, base);
  if (scene) g_in_bracket = false;
}

// SetPredication(r3 device, r4 mask): D3DPRED_TILE(i) = 3 << 2i, D3DPRED_TILE_RENDER(i) = 2 << 2i, 0xAAAAAAAA all.
REX_EXTERN(__imp__sub_826BD300);
REX_HOOK_RAW(sub_826BD300) {
  using namespace crackdown;
  if (g_in_bracket) g_suppressed = ctx.r4.u32 != 0 && (ctx.r4.u32 & kTileZeroBits) == 0;
  __imp__sub_826BD300(ctx, base);
}

// Render-target fixup of a command buffer at load (from sub_823BD140).
REX_EXTERN(__imp__sub_826C3D90);
REX_HOOK_RAW(sub_826C3D90) {
  __imp__sub_826C3D90(ctx, base);
  crackdown::g_loads.fetch_add(1, std::memory_order_release);
}

// Binds a shader's literal constants (r3 device, r4 shader, r5 the shader's data base): one LOAD_ALU_CONSTANT per entry
// of the literal table at shader + *(shader + 20) + 20 (its end at table + 16): {u16 first float4, u16 dwords,
// u32 offset from r5}.
REX_EXTERN(__imp__sub_826C6A50);
REX_HOOK_RAW(sub_826C6A50) {
  using namespace crackdown;
  const uint32_t shader = ctx.r4.u32;
  const uint32_t table_offset = Enabled() ? Load32(base, shader + 20) : 0;
  if (table_offset) {
    const uint32_t table = shader + table_offset;
    uint32_t entry = table + 20;
    const uint32_t end = entry + Load32(base, table + 16);
    for (uint32_t n = 0; entry < end && n < 64; ++n, entry += 8) {
      const uint32_t dwords = Load32(base, entry) & 0xFFFF;
      if (dwords == 0 || dwords > 1024) break;
      const uint32_t literals = Load32(base, entry + 4) + ctx.r5.u32;
      for (uint32_t k = 0; k < dwords; ++k) {
        if (!IsTileTexelSize(Load32(base, literals + 4 * k))) continue;
        Store32(base, literals + 4 * k, std::bit_cast<uint32_t>(1.0f / 720.0f));
        NoteLiteral(PhysicalAddress(literals + 4 * k));
      }
    }
  }
  __imp__sub_826C6A50(ctx, base);
}
