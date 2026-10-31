#include "heart_rate_parser.hpp"

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
			m.rrIntervals1024.push_back(readU16(data + pos));
			pos += 2;
		}
	}

	m.status = (m.bpm >= kMinValidBpm && m.bpm <= kMaxValidBpm) ? ParseStatus::Ok : ParseStatus::InvalidValue;
	return m;
}

} // namespace blehr
