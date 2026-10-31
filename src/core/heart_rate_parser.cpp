#include "heart_rate_parser.hpp"

#include <cmath>

namespace blehr {

namespace {
constexpr uint8_t kFlagUint16 = 0x01;
constexpr uint8_t kFlagContactDetected = 0x02;
constexpr uint8_t kFlagContactSupported = 0x04;
constexpr uint8_t kFlagEnergyExpended = 0x08;
constexpr uint8_t kFlagRrInterval = 0x10;

uint16_t readU16(const uint8_t *p)
{
	return static_cast<uint16_t>(p[0] | (p[1] << 8));
}
} // namespace

HeartRateMeasurement parseHeartRateMeasurement(const uint8_t *data, size_t size)
{
	HeartRateMeasurement m;
	if (!data || size < 2)
		return m; // Malformed: needs at least flags + UINT8 value.

	const uint8_t flags = data[0];
	size_t pos = 1;

	m.uint16Format = (flags & kFlagUint16) != 0;
	if (m.uint16Format) {
		if (size < pos + 2)
			return m;
		m.bpm = readU16(data + pos);
		pos += 2;
	} else {
		m.bpm = data[pos];
		pos += 1;
	}

	if (flags & kFlagContactSupported)
		m.contact = (flags & kFlagContactDetected) ? ContactStatus::Detected : ContactStatus::NotDetected;

	if (flags & kFlagEnergyExpended) {
		if (size < pos + 2)
			return m; // Truncated energy field means the packet is not trustworthy.
		m.energyExpendedKj = readU16(data + pos);
		pos += 2;
	}

	if (flags & kFlagRrInterval) {
		// RR intervals fill the rest of the packet. A trailing odd byte is ignored rather
		// than discarding a packet whose mandatory fields are intact.
		while (pos + 2 <= size) {
			if (m.rrCount < HeartRateMeasurement::kMaxRr)
				m.rrIntervals1024[m.rrCount++] = readU16(data + pos);
			pos += 2;
		}
	}

	m.status = (m.bpm >= kMinValidBpm && m.bpm <= kMaxValidBpm) ? ParseStatus::Ok : ParseStatus::InvalidValue;
	return m;
}

const char *measurementKindName(MeasurementKind kind)
{
	switch (kind) {
	case MeasurementKind::HeartRateService:
		return "heart-rate sensor";
	case MeasurementKind::IndoorBike:
		return "indoor bike";
	case MeasurementKind::Treadmill:
		return "treadmill";
	case MeasurementKind::Rower:
		return "rower";
	case MeasurementKind::CrossTrainer:
		return "cross trainer";
	case MeasurementKind::PlxContinuous:
	case MeasurementKind::PlxSpotCheck:
		return "pulse oximeter";
	}
	return "sensor";
}

namespace {

// Field layout of an FTMS data characteristic: for each flag bit, the size of the
// field(s) it enables. Bit 0 ("More Data") is inverted: its fields are present when the
// bit is 0. -1 marks the Heart Rate field (UINT8, beats per minute).
struct FtmsLayout {
	int flagBytes;
	int bits;
	int sizes[16];
};

// Sizes from the Bluetooth SIG Fitness Machine Service 1.0 specification.
const FtmsLayout kIndoorBike = {2, 13, {2, 2, 2, 2, 3, 2, 2, 2, 5, -1, 1, 2, 2}};
const FtmsLayout kTreadmill = {2, 13, {2, 2, 3, 4, 4, 1, 1, 5, -1, 1, 2, 2, 4}};
const FtmsLayout kRower = {2, 13, {3, 1, 3, 2, 2, 2, 2, 2, 5, -1, 1, 2, 2}};
const FtmsLayout kCrossTrainer = {3, 15, {2, 2, 3, 4, 2, 4, 4, 2, 2, 2, 5, -1, 1, 2, 2}};

HeartRateMeasurement parseFtms(const FtmsLayout &layout, const uint8_t *data, size_t size)
{
	HeartRateMeasurement m; // Malformed by default.
	if (!data || size < size_t(layout.flagBytes))
		return m;
	uint32_t flags = 0;
	for (int i = 0; i < layout.flagBytes; ++i)
		flags |= uint32_t(data[i]) << (8 * i);
	size_t pos = size_t(layout.flagBytes);

	for (int bit = 0; bit < layout.bits; ++bit) {
		const bool set = (flags >> bit) & 1u;
		const bool present = bit == 0 ? !set : set;
		if (!present)
			continue;
		const int fieldSize = layout.sizes[bit];
		if (fieldSize == -1) {
			if (pos + 1 > size)
				return m;
			m.bpm = data[pos];
			m.status = (m.bpm >= kMinValidBpm && m.bpm <= kMaxValidBpm) ? ParseStatus::Ok
										  : ParseStatus::InvalidValue;
			return m; // Fields after the heart rate are not needed.
		}
		pos += size_t(fieldSize);
		if (pos > size)
			return m;
	}
	m.status = ParseStatus::NoHeartRate;
	return m;
}

// IEEE-11073 16-bit SFLOAT: 4-bit signed exponent, 12-bit signed mantissa.
bool sfloatToBpm(uint16_t raw, uint16_t &bpm)
{
	const int mantissaRaw = raw & 0x0FFF;
	if (mantissaRaw >= 0x07FE && mantissaRaw <= 0x0802)
		return false; // NaN, NRes, +/-INFINITY, reserved
	const int mantissa = mantissaRaw >= 0x0800 ? mantissaRaw - 0x1000 : mantissaRaw;
	int exponent = (raw >> 12) & 0x0F;
	if (exponent >= 0x08)
		exponent -= 0x10;
	const double value = double(mantissa) * std::pow(10.0, exponent);
	if (!(value >= 0.0 && value < 1000.0))
		return false;
	bpm = uint16_t(std::lround(value));
	return true;
}

} // namespace

HeartRateMeasurement parseFitnessMachineData(MeasurementKind kind, const uint8_t *data, size_t size)
{
	switch (kind) {
	case MeasurementKind::IndoorBike:
		return parseFtms(kIndoorBike, data, size);
	case MeasurementKind::Treadmill:
		return parseFtms(kTreadmill, data, size);
	case MeasurementKind::Rower:
		return parseFtms(kRower, data, size);
	case MeasurementKind::CrossTrainer:
		return parseFtms(kCrossTrainer, data, size);
	default:
		return HeartRateMeasurement{};
	}
}

HeartRateMeasurement parsePlxMeasurement(const uint8_t *data, size_t size)
{
	// Spot-check and Continuous both start with: Flags (UINT8), SpO2 (SFLOAT),
	// Pulse Rate (SFLOAT).
	HeartRateMeasurement m;
	if (!data || size < 5)
		return m;
	uint16_t bpm = 0;
	if (!sfloatToBpm(readU16(data + 3), bpm)) {
		m.status = ParseStatus::InvalidValue; // "Not available" values (finger out, etc.)
		return m;
	}
	m.bpm = bpm;
	m.status = (bpm >= kMinValidBpm && bpm <= kMaxValidBpm) ? ParseStatus::Ok : ParseStatus::InvalidValue;
	return m;
}

HeartRateMeasurement parseMeasurement(MeasurementKind kind, const uint8_t *data, size_t size)
{
	switch (kind) {
	case MeasurementKind::HeartRateService:
		return parseHeartRateMeasurement(data, size);
	case MeasurementKind::PlxContinuous:
	case MeasurementKind::PlxSpotCheck:
		return parsePlxMeasurement(data, size);
	default:
		return parseFitnessMachineData(kind, data, size);
	}
}

} // namespace blehr
