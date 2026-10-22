#pragma once

#include <cstdint>

#include <rex/ui/ui_event.h>
#include <rex/ui/virtual_key.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>

namespace crackdown {

// HLE renderer milestone 1: with crackdown_hle_capture on, F8 records the next frames twice, from the
// two ends of the same command stream. The game side (hooks on the D3D draw functions) writes, for
// every DRAW packet the game's D3D device emits, the device's shadow state right after the call to
// logs/hle_game_<n>.bin; the GPU side (gpu_draw_trace, SDK patch 0106) writes the register state the
// command processor sees for every DRAW it executes to logs/hle_gpu_<n>.bin. Both use the same record
// layout and are matched by the DRAW packet's guest physical address (tools/hle-trace-compare.ps1).

// Whether crackdown_hle_capture is on.
bool HleCaptureEnabled();

// Arms a capture starting at the next game frame. False when one is already running.
bool StartHleCapture();

// Called at every main-loop frame of the title.
void HleCaptureFrame();

// F8 starts a capture when crackdown_hle_capture is on. A held key is one press, and the game never sees it.
class HleCaptureKey final : public rex::ui::WindowInputListener {
 public:
  static constexpr size_t kZOrder = 1000;

  explicit HleCaptureKey(rex::ui::Window* window) : window_(window) { window_->AddInputListener(this, kZOrder); }
  ~HleCaptureKey() override { window_->RemoveInputListener(this); }

  HleCaptureKey(const HleCaptureKey&) = delete;
  HleCaptureKey& operator=(const HleCaptureKey&) = delete;

  void OnKeyDown(rex::ui::KeyEvent& e) override {
    if (!IsF8(e)) {
      return;
    }
    e.set_handled(true);
    if (!e.prev_state()) {
      StartHleCapture();
    }
  }

  void OnKeyUp(rex::ui::KeyEvent& e) override {
    if (IsF8(e)) {
      e.set_handled(true);
    }
  }

 private:
  bool IsF8(const rex::ui::KeyEvent& e) const {
    return HleCaptureEnabled() && e.virtual_key() == rex::ui::VirtualKey::kF8 && e.target() == window_;
  }

  rex::ui::Window* window_;
};

}  // namespace crackdown
