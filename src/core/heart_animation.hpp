// Heartbeat animation timing: the heart "beats" at the measured BPM with a lub-dub
// pulse. Pure math, unit tested.
#pragma once

#include <cmath>
#include <cstdint>

namespace blehr {

// Pulse intensity in [0, 1] for a beat phase in [0, 1): a strong "lub" right after the
// beat and a softer "dub" shortly after, resting for the rest of the cycle.
inline float heartPulse(double phase)
{
	const double lub = (phase - 0.06) / 0.055;
	const double dub = (phase - 0.30) / 0.06;
	const double v = std::exp(-lub * lub) + 0.55 * std::exp(-dub * dub);
	return float(v > 1.0 ? 1.0 : v);
}

class HeartBeatAnimator {
public:
	// Advances by `seconds` at `bpm` beats per minute.
	void advance(double seconds, uint16_t bpm)
	{
		if (bpm == 0 || seconds <= 0.0)
			return;
		phase_ += seconds * double(bpm) / 60.0;
		phase_ -= std::floor(phase_);
	}
	void reset() { phase_ = 0.0; }
	double phase() const { return phase_; }
	// Scale factor for a beat strength `amplitude` (e.g. 0.18 = up to 18 % larger).
	float scale(float amplitude) const { return 1.0f + amplitude * heartPulse(phase_); }

private:
	double phase_ = 0.0;
};

} // namespace blehr
