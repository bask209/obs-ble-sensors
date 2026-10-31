// Exponential reconnect backoff with a maximum delay and jitter.
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <random>

namespace blehr {

class Backoff {
public:
	using ms = std::chrono::milliseconds;

	Backoff(ms initial = ms(1000), ms maximum = ms(30000), double jitter = 0.2, uint32_t seed = std::random_device{}())
		: initial_(initial),
		  maximum_(maximum),
		  jitter_(jitter),
		  rng_(seed)
	{
	}

	// Delay before the next attempt. Grows 1x, 2x, 4x... capped at maximum, +/- jitter.
	ms next()
	{
		const double base = std::min<double>(double(initial_.count()) * double(uint64_t(1) << std::min(attempt_, 20u)),
						     double(maximum_.count()));
		attempt_++;
		std::uniform_real_distribution<double> dist(1.0 - jitter_, 1.0 + jitter_);
		const double value = std::min(base * dist(rng_), double(maximum_.count()));
		return ms(std::max<int64_t>(1, int64_t(value)));
	}

	void reset() { attempt_ = 0; }
	uint32_t attempts() const { return attempt_; }
	ms maximum() const { return maximum_; }

private:
	ms initial_;
	ms maximum_;
	double jitter_;
	uint32_t attempt_ = 0;
	std::mt19937 rng_;
};

} // namespace blehr
