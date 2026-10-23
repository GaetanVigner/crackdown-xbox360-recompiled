#pragma once

#include <cstdint>

namespace crackdown {

// With crackdown_single_tile_scene on, the main scene renders at 1x MSAA in one predicated tile instead of two 2x
// MSAA tiles (1280x720 color and depth then fit the 10 MB EDRAM), so the GPU executes each scene draw once instead of
// once per tile. True while a call the title makes for its second tile (a draw, resolve or clear under a predication
// mask with no tile-0 bit) must be skipped.
bool SingleTileSuppressed();

// Called after every RunCommandBuffer (sub_826C3750) with the device, its ring write pointer and segment end before the
// call: patches the tile-0 window scissor and the 1/484 shader literals of the command buffer segments it replays,
// once per segment.
void SingleTileCommandBuffer(uint8_t* base, uint32_t device, uint32_t pre, uint32_t segment_end);

}  // namespace crackdown
