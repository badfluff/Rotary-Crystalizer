#pragma once

#include <Arduino.h>
#include <math.h>

/**
 * A general-purpose, time-aware PID controller.
 *
 * Design choices that matter for a slow, high-inertia thermal process:
 *
 *  - Derivative on measurement (not on error). Differentiating the measured
 *    temperature instead of the error avoids a "derivative kick" whenever the
 *    setpoint changes, and it lets the D term act purely as a brake against
 *    the tank's own thermal momentum.
 *
 *  - Rolling-window derivative. Instead of a single-step d(temp)/dt (which is
 *    dominated by sensor quantisation and produces a couple-percent of pointless
 *    jitter on the output every time the reading ticks), the rate of change is
 *    estimated by a least-squares straight-line fit over the last
 *    `derivWindow` seconds of samples. Over such a window even the sharpest
 *    real inflection is nearly linear, so this recovers the true trend while
 *    averaging the noise away. The window is adjustable for tuning.
 *
 *  - Asymmetric derivative braking. The plant is asymmetric: we can push the
 *    temperature up quickly (large forward authority) but can only let it fall
 *    at the slow, roughly constant rate at which it loses heat to ambient.
 *    A rising temperature therefore needs a strong brake to avoid overshoot,
 *    while a falling temperature needs only a gentle response (dumping heat
 *    aggressively on the way down just sets up the next overshoot). Separate
 *    `kdRising` / `kdFalling` gains express this directly.
 *
 *  - Banded, conditional-integration anti-windup. The integrator only learns
 *    while the measurement is within `integralBand` degrees of the setpoint.
 *    Far from the setpoint the P (and D) terms carry the load, so letting the
 *    integral wind up during the long heat-up serves no purpose and only
 *    guarantees overshoot; near the setpoint the integral quietly converges to
 *    whatever steady-state hold power ambient conditions demand. Within the
 *    band the classic conditional-integration rule additionally prevents
 *    integrating further into saturation.
 *
 * All gains are in intuitive, tunable units. No physical model constants are
 * required: the integral term learns whatever steady-state power the current
 * ambient conditions demand.
 */
class PIDController {
public:
  // Upper bound on stored derivative-window samples. Samples arrive at the
  // sensor conversion cadence (~0.75 s), so this comfortably covers windows of
  // a few minutes; if it ever fills, the oldest sample is evicted gracefully.
  static const int kDerivBufferMax = 256;

  PIDController(float kp, float ki, float kd,
                float outMin = 0.0f, float outMax = 1.0f,
                float derivWindow = 30.0f,
                float integralBand = 8.0f)
      : _kp(kp), _ki(ki), _kdRising(kd), _kdFalling(kd),
        _outMin(outMin), _outMax(outMax),
        _derivWindow(derivWindow), _integralBand(integralBand) {
    reset();
  }

  void setGains(float kp, float ki, float kd) {
    _kp = kp;
    _ki = ki;
    _kdRising = kd;
    _kdFalling = kd;
  }

  /**
   * Separate derivative gains for a rising vs. falling measurement.
   * @param kdRising  brake gain applied while temperature is increasing.
   * @param kdFalling response gain applied while temperature is decreasing.
   */
  void setDerivativeGains(float kdRising, float kdFalling) {
    _kdRising = kdRising;
    _kdFalling = kdFalling;
  }

  /** Seconds of history used for the rolling rate-of-change estimate. */
  void setDerivativeWindow(float seconds) { _derivWindow = seconds; }

  /** Degrees around the setpoint within which the integrator is allowed to learn. */
  void setIntegralBand(float band) { _integralBand = band; }

  void setOutputLimits(float lo, float hi) {
    _outMin = lo;
    _outMax = hi;
    _integral = constrain(_integral, _outMin, _outMax);
  }

  /** Clear all internal state (integral, derivative history). */
  void reset() {
    _integral = 0.0f;
    _deriv = 0.0f;
    _pTerm = 0.0f;
    _dTerm = 0.0f;
    _lastOutput = _outMin;
    _clock = 0.0f;
    _head = 0;
    _count = 0;
  }

  /**
   * Seed the integrator, e.g. to the expected steady-state hold power, so the
   * controller settles faster after a (re)start.
   */
  void primeIntegral(float value) {
    _integral = constrain(value, _outMin, _outMax);
  }

  /**
   * Advance the controller by one step.
   * @param setpoint    desired value (same units as measurement)
   * @param measurement current process value
   * @param dt          elapsed time since the previous call, in seconds
   * @return control output, clamped to [outMin, outMax]
   */
  float update(float setpoint, float measurement, float dt) {
    if (dt <= 0.0f) {
      return _lastOutput;
    }

    const float error = setpoint - measurement;

    // --- Derivative on measurement, rolling least-squares slope ---
    _clock += dt;
    pushSample(_clock, measurement);
    _deriv = windowSlope(); // units/s over the last _derivWindow seconds

    const float kd = (_deriv >= 0.0f) ? _kdRising : _kdFalling;

    const float pTerm = _kp * error;
    const float dTerm = -kd * _deriv; // brake against rising process
    _pTerm = pTerm;
    _dTerm = dTerm;

    // --- Integral: only learn near the setpoint (banded), with
    //     conditional-integration anti-windup while saturated. ---
    const bool withinBand = fabsf(error) <= _integralBand;
    const float candidateIntegral =
        withinBand ? _integral + _ki * error * dt : _integral;
    float output = pTerm + dTerm + candidateIntegral;

    if (output >= _outMax) {
      // Saturated high: only integrate if that pulls the output back down.
      if (withinBand && error < 0.0f) {
        _integral = candidateIntegral;
      }
      output = _outMax;
    } else if (output <= _outMin) {
      // Saturated low: only integrate if that pushes the output back up.
      if (withinBand && error > 0.0f) {
        _integral = candidateIntegral;
      }
      output = _outMin;
    } else {
      _integral = candidateIntegral;
    }

    _integral = constrain(_integral, _outMin, _outMax);
    _lastOutput = constrain(output, _outMin, _outMax);
    return _lastOutput;
  }

  float integral() const { return _integral; }
  float derivative() const { return _deriv; }
  float lastOutput() const { return _lastOutput; }

  // Individual contributions to the most recent output (in output units,
  // i.e. fractions of full power). Exposed for tuning/telemetry.
  float pTerm() const { return _pTerm; }
  float dTerm() const { return _dTerm; }
  float iTerm() const { return _integral; }

private:
  // Append a sample to the ring buffer and drop everything older than the
  // configured window (keeping at least the two most recent samples so a slope
  // can always be formed).
  void pushSample(float t, float y) {
    const int tail = (_head + _count) % kDerivBufferMax;
    _t[tail] = t;
    _y[tail] = y;
    if (_count < kDerivBufferMax) {
      _count++;
    } else {
      _head = (_head + 1) % kDerivBufferMax; // overwrite oldest
    }

    // Evict samples older than the window, but never below 2 samples.
    while (_count > 2 && (t - _t[_head]) > _derivWindow) {
      _head = (_head + 1) % kDerivBufferMax;
      _count--;
    }
  }

  // Least-squares slope (dy/dt) of the buffered samples. Times are taken
  // relative to the oldest buffered sample to preserve float precision.
  float windowSlope() const {
    if (_count < 2) {
      return 0.0f;
    }
    const float t0 = _t[_head];
    float sumT = 0.0f, sumY = 0.0f, sumTT = 0.0f, sumTY = 0.0f;
    for (int i = 0; i < _count; ++i) {
      const int idx = (_head + i) % kDerivBufferMax;
      const float t = _t[idx] - t0;
      const float y = _y[idx];
      sumT += t;
      sumY += y;
      sumTT += t * t;
      sumTY += t * y;
    }
    const float n = static_cast<float>(_count);
    const float denom = n * sumTT - sumT * sumT;
    if (fabsf(denom) < 1e-9f) {
      return 0.0f;
    }
    return (n * sumTY - sumT * sumY) / denom;
  }

  float _kp, _ki;
  float _kdRising, _kdFalling;
  float _outMin, _outMax;
  float _derivWindow;   // seconds
  float _integralBand;  // degrees around setpoint where the integral learns

  float _integral;
  float _deriv;      // most recent windowed rate of change (units/s)
  float _pTerm;      // most recent proportional contribution (output units)
  float _dTerm;      // most recent derivative contribution (output units)
  float _lastOutput;

  // Rolling-window sample history for the derivative estimate.
  float _clock;      // monotonically increasing time base (seconds)
  float _t[kDerivBufferMax];
  float _y[kDerivBufferMax];
  int _head;         // index of oldest sample
  int _count;        // number of valid samples
};
