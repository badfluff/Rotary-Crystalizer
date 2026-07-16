#pragma once

#include <Arduino.h>

/**
 * Time-proportioning ("slow PWM") driver for a solid-state relay.
 *
 * A hot plate has an enormous thermal mass, so there is no benefit to fast
 * PWM. Instead the requested duty cycle (0..1) is realised by switching the
 * relay on for a fraction of a fixed, relatively long period (e.g. a couple of
 * seconds). This is gentle on the relay and perfectly adequate for controlling
 * heater power into water.
 *
 * `activeHigh` selects the electrical polarity: set it false if your SSR turns
 * the load ON when the control pin is driven LOW.
 */
class SlowPWM {
public:
  SlowPWM(int pin, unsigned long periodMs, bool activeHigh = true)
      : _pin(pin), _period(periodMs), _activeHigh(activeHigh) {
    pinMode(_pin, OUTPUT);
    _cycleStart = millis();
    writeRelay(false);
  }

  /** Set the requested heater power as a fraction in [0, 1]. */
  void setDuty(float duty) { _duty = constrain(duty, 0.0f, 1.0f); }
  float duty() const { return _duty; }
  bool isOn() const { return _isOn; }

  /** Call frequently from loop(). Non-blocking. */
  void update() {
    const unsigned long now = millis();
    unsigned long elapsed = now - _cycleStart;

    // Roll over to a fresh period (and catch up if the loop was delayed).
    if (elapsed >= _period) {
      _cycleStart += _period * (elapsed / _period);
      elapsed = now - _cycleStart;
    }

    const unsigned long onTime = (unsigned long)(_period * _duty);
    writeRelay(elapsed < onTime);
  }

  /** Force the heater off immediately. */
  void off() {
    _duty = 0.0f;
    writeRelay(false);
  }

private:
  void writeRelay(bool on) {
    _isOn = on;
    digitalWrite(_pin, (on == _activeHigh) ? HIGH : LOW);
  }

  int _pin;
  unsigned long _period;
  bool _activeHigh;
  float _duty = 0.0f;
  unsigned long _cycleStart = 0;
  bool _isOn = false;
};
