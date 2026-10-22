#pragma once

#include <cstdint>
#include <functional>
#include <utility>

#include <rex/ui/ui_event.h>
#include <rex/ui/virtual_key.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>

namespace crackdown {

// Developer diagnostics for physics problems, off unless crackdown_physics_telemetry is set: after every simulation
// update the local player's character, controller and driven vehicle objects are copied raw (guest big-endian) into a
// ring of the last kTelemetryFrames updates. F9, or the first NaN repair of a kind, writes the ring to
// logs\telemetry_<n>.bin on the game thread at its next update.
inline constexpr uint32_t kTelemetryFrames = 4096;
// The character object reaches at least +6312 (its secondary equipment), the controller at least +5084 (its desired
// velocity); the copies stop there so they never read past the objects.
inline constexpr uint32_t kTelemetryCharacterBytes = 6312;
inline constexpr uint32_t kTelemetryControllerBytes = 5084;
// The vehicle the player drives (Character +5580 while driving): its state word sits at +3064.
inline constexpr uint32_t kTelemetryVehicleBytes = 3200;

// Game thread, after each simulation update; step_seconds is the step that update ran.
void RecordPhysicsStep(uint8_t* base, double step_seconds);

// Any thread.
void RequestTelemetryDump();

// Whether crackdown_physics_telemetry is on.
bool PhysicsTelemetryEnabled();

// F9 asks for a dump. A held key is one press, and the game never sees the key.
class TelemetryKey final : public rex::ui::WindowInputListener {
 public:
  static constexpr size_t kZOrder = 1000;

  explicit TelemetryKey(rex::ui::Window* window) : window_(window) { window_->AddInputListener(this, kZOrder); }
  ~TelemetryKey() override { window_->RemoveInputListener(this); }

  TelemetryKey(const TelemetryKey&) = delete;
  TelemetryKey& operator=(const TelemetryKey&) = delete;

  void OnKeyDown(rex::ui::KeyEvent& e) override {
    if (!IsF9(e)) {
      return;
    }
    e.set_handled(true);
    if (!e.prev_state()) {
      RequestTelemetryDump();
    }
  }

  void OnKeyUp(rex::ui::KeyEvent& e) override {
    if (IsF9(e)) {
      e.set_handled(true);
    }
  }

 private:
  bool IsF9(const rex::ui::KeyEvent& e) const {
    // Only while recording: F9 stays free for keyboard bindings otherwise.
    return PhysicsTelemetryEnabled() && e.virtual_key() == rex::ui::VirtualKey::kF9 && e.target() == window_;
  }

  rex::ui::Window* window_;
};

}  // namespace crackdown
