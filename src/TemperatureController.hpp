#pragma once

#include <Arduino.h>
#include <math.h>
#include "PIDController.hpp"

/**
 * High-level temperature controller for a heat-only (hot plate + SSR) system.
 *
 * Thin wrapper around PIDController. The PID acts on the ordinary error
 * (target - measurement). Overshoot is handled inside the PID itself via the
 * windowed derivative brake and banded integrator.
 */
class TemperatureController {
public:
  /** A profile target at an elapsed time, expressed in minutes and deg C. */
  struct ProfilePoint {
    float minutes;
    float targetC;
  };

  enum class Mode : uint8_t {
    FixedTarget,
    Profile,
  };

  static constexpr size_t MAX_PROFILE_POINTS = 64;

  explicit TemperatureController(const PIDController &pid)
      : _pid(pid) {}


  /** Set a single target temperature (deg C) and leave profile mode. */
  void setTarget(float target) {
    _target = target;
    _mode = Mode::FixedTarget;
  }

  /**
   * Start following a time/temperature profile.
   *
   * Points must be in strictly increasing minute order. The target is held at
   * the first point before its time, linearly interpolated between points, and
   * held at the last point after the profile finishes.
   *
   * @param points      profile points, where minutes are elapsed minutes
   * @param pointCount  number of points, from 1 to MAX_PROFILE_POINTS
   * @param startMillis profile start timestamp; defaults to now
   * @return true when the profile is valid and was started
   */
  bool startProfile(const ProfilePoint *points, size_t pointCount,
                    unsigned long startMillis = millis()) {
    if (points == nullptr || pointCount == 0 || pointCount > MAX_PROFILE_POINTS) {
      return false;
    }

    for (size_t i = 0; i < pointCount; ++i) {
      if (!isfinite(points[i].minutes) || !isfinite(points[i].targetC) ||
          points[i].minutes < 0.0f ||
          (i > 0 && points[i].minutes <= points[i - 1].minutes)) {
        return false;
      }
    }

    for (size_t i = 0; i < pointCount; ++i) {
      _profile[i] = points[i];
    }
    _profilePointCount = pointCount;
    _profileStartedAt = startMillis;
    _target = _profile[0].targetC;
    _mode = Mode::Profile;
    return true;
  }

  /** Stop profile mode and retain the current working target as a fixed target. */
  void stopProfile() { _mode = Mode::FixedTarget; }

  Mode mode() const { return _mode; }
  bool isFollowingProfile() const { return _mode == Mode::Profile; }
  size_t profilePointCount() const { return _profilePointCount; }

  /** Calculate and store the target that applies at the given clock time. */
  float updateTarget(unsigned long now) {
    if (_mode != Mode::Profile || _profilePointCount == 0) {
      return _target;
    }

    const float elapsedMinutes = (now - _profileStartedAt) / 60000.0f;
    if (elapsedMinutes <= _profile[0].minutes) {
      _target = _profile[0].targetC;
      return _target;
    }

    const size_t last = _profilePointCount - 1;
    if (elapsedMinutes >= _profile[last].minutes) {
      _target = _profile[last].targetC;
      return _target;
    }

    for (size_t i = 1; i < _profilePointCount; ++i) {
      if (elapsedMinutes <= _profile[i].minutes) {
        const ProfilePoint &before = _profile[i - 1];
        const ProfilePoint &after = _profile[i];
        const float fraction = (elapsedMinutes - before.minutes) /
                               (after.minutes - before.minutes);
        _target = before.targetC + fraction * (after.targetC - before.targetC);
        return _target;
      }
    }

    return _target; // Unreachable for a valid profile; retains a safe target.
  }

  float target() const { return _target; }

  float power() const { return _pid.lastOutput(); }
  PIDController &pid() { return _pid; }

  /** Reset all internal PID state. */
  void reset(float /*currentTemp*/) { _pid.reset(); }

  /**
   * Compute the heating power for one control step.
   * @param measurement current temperature (deg C)
   * @param dt          time since previous update (seconds)
   * @return heating power fraction in [0, 1]
   */
  float update(float measurement, float dt) {
    return _pid.update(_target, measurement, dt);
  }

  /** Update the active profile target, then compute the heating power. */
  float update(float measurement, float dt, unsigned long now) {
    updateTarget(now);
    return _pid.update(_target, measurement, dt);
  }

private:
  PIDController _pid;
  float _target = 25.0f;
  Mode _mode = Mode::FixedTarget;
  ProfilePoint _profile[MAX_PROFILE_POINTS]{};
  size_t _profilePointCount = 0;
  unsigned long _profileStartedAt = 0;
};
