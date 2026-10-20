#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace crackdown {

inline constexpr int kFrameRateUnlimited = 0;
inline constexpr int kOriginalFrameRate = 30;
inline constexpr int kDefaultFrameRate = 60;
inline constexpr int kGuestVblankRate = 60;
inline constexpr std::array<int, 8> kFrameRates = {30, 60, 90, 120, 144, 165, 240, kFrameRateUnlimited};

// Measured frames are kept in these bounds: a longer frame slows the game down, as a missed frame did on the
// console, instead of making the next step larger than the title's own 1/30 s.
inline constexpr double kMinMeasuredFrameSeconds = 0.00025;
inline constexpr double kMaxMeasuredFrameSeconds = 1.0 / kOriginalFrameRate;

// The launcher's choices; any other value (an old or hand-edited one) is the default.
constexpr int NormalizeFrameRate(int rate) {
  return std::find(kFrameRates.begin(), kFrameRates.end(), rate) != kFrameRates.end() ? rate : kDefaultFrameRate;
}

// The saved setting as a frame rate; text that is not one of the choices is the default.
inline int ParseFrameRate(std::string_view text) {
  int rate = 0;
  const char* end = text.data() + text.size();
  const auto [stop, error] = std::from_chars(text.data(), end, rate);
  return error == std::errc() && stop == end ? NormalizeFrameRate(rate) : kDefaultFrameRate;
}

// The launcher's Frame rate choices as {value, label}.
inline std::vector<std::pair<std::string, std::string>> FrameRateOptions() {
  std::vector<std::pair<std::string, std::string>> options;
  for (const int rate : kFrameRates) {
    std::string label = rate == kFrameRateUnlimited ? "Unlimited" : std::to_string(rate) + " FPS";
    if (rate == kOriginalFrameRate) {
      label += " (Original)";
    } else if (rate == kDefaultFrameRate) {
      label += " (Recommended)";
    }
    options.emplace_back(std::to_string(rate), std::move(label));
  }
  return options;
}

// 30 and 60 wait for the 60 Hz guest vblank; higher rates and Unlimited release each swap at once and are paced on
// the host.
constexpr bool HostPacedFrameRate(int rate) {
  return rate == kFrameRateUnlimited || rate > kGuestVblankRate;
}

// Present interval in guest vblanks; 0 releases the swap at once.
constexpr uint32_t PresentIntervalFor(int rate) {
  return HostPacedFrameRate(rate) ? 0 : uint32_t(kGuestVblankRate / rate);
}

// The title sleeps until 1000 / limiter ms after the frame started while its fixed step is on.
constexpr uint32_t LimiterFrameRateFor(int rate) {
  return rate == kFrameRateUnlimited ? 1000 : uint32_t(32 * rate / kOriginalFrameRate);
}

// One frame of game time, before the title's time scale.
struct FrameStep {
  double seconds;
  float seconds_f;
  // In the title's own 1/30 s steps.
  double original_steps;
};

constexpr FrameStep FixedFrameStep(int rate) {
  return {1.0 / rate, 1.0f / float(rate), double(kOriginalFrameRate) / rate};
}

constexpr FrameStep MeasuredFrameStep(double seconds) {
  const double clamped = std::clamp(seconds, kMinMeasuredFrameSeconds, kMaxMeasuredFrameSeconds);
  return {clamped, float(clamped), clamped * kOriginalFrameRate};
}

// A step already known to be within bounds (CompensatedStepClock clamps its own result), so it is packaged without
// MeasuredFrameStep's fixed 1/30 s ceiling.
constexpr FrameStep StepFromSeconds(double seconds) {
  return {seconds, float(seconds), seconds * kOriginalFrameRate};
}

// 30 and 60 present at the guest vblank, so a slow frame is already free of host pacing; this measures how far
// behind it fell and compensates instead of letting a fixed per-call step turn the slowdown into slow motion. A
// frame close to a whole number of the rate's own steps (within the tolerance) snaps to exactly that many, so
// timer jitter does not perturb an on-time frame; a frame further off advances by the real interval, clamped to at
// most kMaxCompensatedSteps steps so one huge stall cannot become one huge physics step. The gap between the step
// taken and the real interval carries to the next frame (clamped to half a step), so sustained slow frames still
// advance game time at the real rate instead of drifting into permanent debt, and a stall past the cap drops its
// excess instead of being owed back. Adapted from Geometry Wars Retro Evolved 2's GameDeltaClock.
inline constexpr int kMaxCompensatedSteps = 3;

class CompensatedStepClock {
 public:
  enum class Kind { kSnapped, kCompensated, kClamped };

  struct Result {
    double seconds;
    Kind kind;
  };

  Result Take(double interval, double step) {
    const double target = interval + owed_;
    const double steps = target / step;
    const double nearest = std::clamp(std::round(steps), 1.0, double(kMaxCompensatedSteps));
    const double cap = kMaxCompensatedSteps * step;
    const bool snap = std::fabs(steps - nearest) <= kSnapTolerance;
    double delta = snap ? nearest * step : target;
    const bool clamped = delta > cap || delta < kMinMeasuredFrameSeconds;
    delta = std::clamp(delta, kMinMeasuredFrameSeconds, cap);
    owed_ = target > cap ? 0.0 : std::clamp(target - delta, -0.5 * step, 0.5 * step);
    return {delta, snap ? Kind::kSnapped : (clamped ? Kind::kClamped : Kind::kCompensated)};
  }

  void Reset() { owed_ = 0.0; }
  double owed() const { return owed_; }

 private:
  static constexpr double kSnapTolerance = 0.2;
  double owed_ = 0.0;
};

// Splits a running total into whole units and the fraction carried to the next frame.
constexpr uint32_t TakeWhole(double& total) {
  const uint32_t whole = uint32_t(total);
  total -= whole;
  return whole;
}

// Game time for frames that cannot keep their rate. The title's variable-step mode does not tolerate a step that
// jumps from one frame to the next (walking on a slope, the character controller turns the jump into a launch or a
// push into the wall), so a late frame does not get the raw interval: the step follows a moving average of the real
// intervals, and the time owed to the game (real time not yet stepped) is paid back a fraction per frame. On a
// steady rate the average equals the interval and the step is exact. An interval is counted at most 1/30 s, the
// title's own step, so a stall slows the game instead of being owed back.
class StepSmoother {
 public:
  void Reset(double seconds) {
    average_ = seconds;
    owed_ = 0.0;
  }

  double Take(double interval) {
    const double counted = std::clamp(interval, kMinMeasuredFrameSeconds, kMaxMeasuredFrameSeconds);
    owed_ += counted;
    average_ += kAverageWeight * (counted - average_);
    const double step =
        std::clamp(average_ + kPaybackWeight * (owed_ - average_), kMinMeasuredFrameSeconds, kMaxMeasuredFrameSeconds);
    owed_ = std::clamp(owed_ - step, -kMaxMeasuredFrameSeconds, kMaxMeasuredFrameSeconds);
    return step;
  }

  double owed() const { return owed_; }

 private:
  static constexpr double kAverageWeight = 0.25;
  static constexpr double kPaybackWeight = 0.25;
  double average_ = 1.0 / kDefaultFrameRate;
  double owed_ = 0.0;
};

// Developer test of uneven frame times (crackdown_debug_frame_dips): the extra delay before a frame, as if the GPU
// could not keep up. The load sweeps from none to full and back every kDipPeriodSeconds, each frame takes 60-100% of
// the current load (jitter in [0, 1)), and kDipSpikeChance of frames (spike below it) stall for twice the maximum.
inline constexpr double kDipPeriodSeconds = 6.0;
inline constexpr double kDipSpikeChance = 0.01;
inline constexpr int kMaxDebugDipMs = 100;

inline double DebugFrameDipSeconds(int max_ms, double seconds, double jitter, double spike) {
  if (max_ms <= 0) {
    return 0.0;
  }
  const double max = std::min(max_ms, kMaxDebugDipMs) * 1e-3;
  if (spike < kDipSpikeChance) {
    return 2.0 * max;
  }
  constexpr double kTwoPi = 6.283185307179586;
  const double load = 0.5 - 0.5 * std::cos(kTwoPi * seconds / kDipPeriodSeconds);
  return max * load * (0.6 + 0.4 * jitter);
}

// A frame's whole milliseconds for the title's integer frame-time sites, carrying the fraction like TakeWhole but
// never 0: the camera divides by its time step, so a frame shorter than a millisecond takes one and the next frames
// pay it back, keeping the total exact.
constexpr uint32_t TakeFrameMs(double& total_ms) {
  if (total_ms < 1.0) {
    total_ms -= 1.0;
    return 1;
  }
  const uint32_t whole = uint32_t(total_ms);
  total_ms -= whole;
  return whole;
}

// Host pacing for HostPacedFrameRate rates. A fixed rate releases frames on a grid of its period, so sleep overshoot
// never accumulates, and an on-time frame steps exactly one period. A frame that starts after its slot restarts the
// grid at once (no backlog of slots behind it), and its step comes from StepSmoother. Unlimited never waits and steps
// the measured time through StepSmoother too, since its intervals vary from frame to frame.
class FramePacer {
 public:
  struct Frame {
    int64_t release_ns;  // wait until this time before running the frame
    FrameStep step;
  };

  Frame Next(int rate, int64_t now_ns) {
    if (rate != rate_ || !started_) {
      rate_ = rate;
      started_ = true;
      Restart(now_ns);
      const FrameStep first = rate == kFrameRateUnlimited ? FixedFrameStep(kGuestVblankRate) : FixedFrameStep(rate);
      smoother_.Reset(first.seconds);
      return {now_ns, first};
    }
    if (rate == kFrameRateUnlimited) {
      const int64_t elapsed = now_ns - last_ns_;
      last_ns_ = now_ns;
      return {now_ns, StepFromSeconds(smoother_.Take(double(elapsed) * 1e-9))};
    }
    if (++index_ == rate) {
      // A whole second of periods is exactly 1e9 ns, so the grid moves on without rounding.
      base_ns_ += kNsPerSecond;
      index_ = 0;
    }
    const int64_t deadline = base_ns_ + index_ * kNsPerSecond / rate;
    if (now_ns > deadline) {
      const int64_t elapsed = now_ns - last_ns_;
      Restart(now_ns);
      return {now_ns, StepFromSeconds(smoother_.Take(double(elapsed) * 1e-9))};
    }
    last_ns_ = deadline;
    const FrameStep fixed = FixedFrameStep(rate);
    const double step = smoother_.Take(fixed.seconds);
    // Exactly one period once nothing is owed, with FixedFrameStep's exact original_steps.
    return {deadline, step == fixed.seconds ? fixed : StepFromSeconds(step)};
  }

 private:
  static constexpr int64_t kNsPerSecond = 1'000'000'000;

  void Restart(int64_t now_ns) {
    base_ns_ = now_ns;
    last_ns_ = now_ns;
    index_ = 0;
  }

  int rate_ = kDefaultFrameRate;
  bool started_ = false;
  int64_t base_ns_ = 0;
  int64_t last_ns_ = 0;
  int64_t index_ = 0;
  StepSmoother smoother_;
};

}  // namespace crackdown
