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
	Ok,           // Well-formed packet with a plausible BPM value.
	Malformed,    // Too short / truncated fields. Packet must be ignored.
	InvalidValue, // Well-formed, but the BPM is zero or implausible.
	NoHeartRate   // Well-formed, but this packet carries no heart-rate field (FTMS).
};

// Standard Bluetooth SIG sources of a heart rate. Every one is an open SIG specification;
// proprietary protocols are deliberately not supported.
enum class MeasurementKind {
	HeartRateService, // 0x180D / 0x2A37 Heart Rate Measurement (straps, bands, watches)
	IndoorBike,       // 0x1826 / 0x2AD2 Fitness Machine: Indoor Bike Data
	Treadmill,        // 0x1826 / 0x2ACD Fitness Machine: Treadmill Data
	Rower,            // 0x1826 / 0x2AD1 Fitness Machine: Rower Data
	CrossTrainer,     // 0x1826 / 0x2ACE Fitness Machine: Cross Trainer Data
	PlxContinuous,    // 0x1822 / 0x2A5F Pulse Oximeter: PLX Continuous Measurement
	PlxSpotCheck      // 0x1822 / 0x2A5E Pulse Oximeter: PLX Spot-check Measurement
};

const char *measurementKindName(MeasurementKind kind);

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

// Heart Rate field of the FTMS machine data characteristics (only present when the
// machine has a heart-rate source, e.g. a paired strap or hand grips).
HeartRateMeasurement parseFitnessMachineData(MeasurementKind kind, const uint8_t *data, size_t size);
// Pulse Rate (IEEE-11073 SFLOAT) of the PLX measurements.
HeartRateMeasurement parsePlxMeasurement(const uint8_t *data, size_t size);
// Dispatches on the kind.
HeartRateMeasurement parseMeasurement(MeasurementKind kind, const uint8_t *data, size_t size);

inline HeartRateMeasurement parseMeasurement(MeasurementKind kind, const std::vector<uint8_t> &bytes)
{
	return parseMeasurement(kind, bytes.data(), bytes.size());
}

} // namespace blehr
