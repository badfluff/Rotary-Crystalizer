#pragma once

#include <Arduino.h>
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
  explicit TemperatureController(const PIDController &pid)
      : _pid(pid) {}


  /** Set the target temperature (deg C). */
  void setTarget(float target) { _target = target; }
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

private:
  PIDController _pid;
  float _target = 25.0f;
};
