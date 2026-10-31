// Pure parser for the Bluetooth SIG Heart Rate Measurement characteristic (0x2A37).
// No Windows or OBS dependencies so it can be unit tested anywhere.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace blehr {

// Valid heart-rate range accepted for display. Values outside are treated as invalid,
// never clamped or invented.
constexpr uint16_t kMinValidBpm = 20;
constexpr uint16_t kMaxValidBpm = 300;

enum class ParseStatus {
	Ok,          // Well-formed packet with a plausible BPM value.
	Malformed,   // Too short / truncated fields. Packet must be ignored.
	InvalidValue // Well-formed, but the BPM is zero or implausible.
};

enum class ContactStatus {
	NotSupported, // Sensor does not report skin contact.
	Detected,
	NotDetected // Sensor explicitly reports that contact is lost.
};

struct HeartRateMeasurement {
	ParseStatus status = ParseStatus::Malformed;
	uint16_t bpm = 0;
	bool uint16Format = false;
	ContactStatus contact = ContactStatus::NotSupported;
	std::optional<uint16_t> energyExpendedKj;
	std::vector<uint16_t> rrIntervals1024; // RR intervals in 1/1024 s units.

	// True only when the packet may be shown: well-formed, plausible BPM and contact
	// not explicitly lost.
	bool isDisplayable() const { return status == ParseStatus::Ok && contact != ContactStatus::NotDetected; }
};

HeartRateMeasurement parseHeartRateMeasurement(const uint8_t *data, size_t size);

inline HeartRateMeasurement parseHeartRateMeasurement(const std::vector<uint8_t> &bytes)
{
	return parseHeartRateMeasurement(bytes.data(), bytes.size());
}

} // namespace blehr
