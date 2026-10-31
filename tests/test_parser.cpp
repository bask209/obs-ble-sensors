#include "test_framework.hpp"

#include "core/display_format.hpp"
#include "core/heart_animation.hpp"
#include "core/heart_rate_parser.hpp"
#include "core/device_identity.hpp"
#include "core/sensor_state.hpp"

using namespace blehr;

TEST(parser_uint8_bpm)
{
	auto m = parseHeartRateMeasurement({0x00, 72});
	CHECK(m.status == ParseStatus::Ok);
	CHECK_EQ(m.bpm, 72);
	CHECK(!m.uint16Format);
	CHECK(m.contact == ContactStatus::NotSupported);
	CHECK(m.isDisplayable());
}

TEST(parser_uint16_bpm)
{
	auto m = parseHeartRateMeasurement({0x01, 0x2C, 0x01}); // 300
	CHECK(m.status == ParseStatus::Ok);
	CHECK(m.uint16Format);
	CHECK_EQ(m.bpm, 300);

	auto m2 = parseHeartRateMeasurement({0x01, 82, 0x00});
	CHECK(m2.status == ParseStatus::Ok);
	CHECK_EQ(m2.bpm, 82);
}

TEST(parser_uint16_out_of_range_is_invalid)
{
	auto m = parseHeartRateMeasurement({0x01, 0xE8, 0x03}); // 1000
	CHECK(m.status == ParseStatus::InvalidValue);
	CHECK(!m.isDisplayable());
}

TEST(parser_zero_is_invalid)
{
	auto m = parseHeartRateMeasurement({0x00, 0x00});
	CHECK(m.status == ParseStatus::InvalidValue);
	CHECK(!m.isDisplayable());
}

TEST(parser_malformed)
{
	CHECK(parseHeartRateMeasurement(nullptr, 0).status == ParseStatus::Malformed);
	CHECK(parseHeartRateMeasurement(std::vector<uint8_t>{}).status == ParseStatus::Malformed);
	CHECK(parseHeartRateMeasurement({0x00}).status == ParseStatus::Malformed);
	CHECK(parseHeartRateMeasurement({0x01, 0x50}).status == ParseStatus::Malformed); // truncated uint16
	CHECK(parseHeartRateMeasurement({0x08, 70, 0x01}).status == ParseStatus::Malformed); // truncated energy
}

TEST(parser_contact_status)
{
	auto detected = parseHeartRateMeasurement({0x06, 70});
	CHECK(detected.contact == ContactStatus::Detected);
	CHECK(detected.isDisplayable());

	auto lost = parseHeartRateMeasurement({0x04, 70});
	CHECK(lost.contact == ContactStatus::NotDetected);
	CHECK(lost.status == ParseStatus::Ok);
	CHECK(!lost.isDisplayable());

	// Bit 1 without "supported" bit 2 means nothing.
	auto unsupported = parseHeartRateMeasurement({0x02, 70});
	CHECK(unsupported.contact == ContactStatus::NotSupported);
}

TEST(parser_energy_and_rr)
{
	// flags: uint16 + energy + rr ; bpm 0x0050=80 ; energy 0x0102 ; rr 0x0400, 0x03FF
	auto m = parseHeartRateMeasurement({0x19, 0x50, 0x00, 0x02, 0x01, 0x00, 0x04, 0xFF, 0x03});
	CHECK(m.status == ParseStatus::Ok);
	CHECK_EQ(m.bpm, 80);
	CHECK(m.energyExpendedKj.has_value());
	CHECK_EQ(*m.energyExpendedKj, 0x0102);
	CHECK_EQ(size_t(m.rrCount), size_t(2));
	CHECK_EQ(m.rrIntervals1024[0], 0x0400);
	CHECK_EQ(m.rrIntervals1024[1], 0x03FF);
}

TEST(parser_rr_odd_trailing_byte_ignored)
{
	auto m = parseHeartRateMeasurement({0x10, 64, 0x00, 0x04, 0x01});
	CHECK(m.status == ParseStatus::Ok);
	CHECK_EQ(m.bpm, 64);
	CHECK_EQ(size_t(m.rrCount), size_t(1));
}

TEST(parser_polar_h10_style_packet)
{
	// Polar H10: flags 0x16 (contact supported+detected, RR present), 1 RR value.
	auto m = parseHeartRateMeasurement({0x16, 0x3E, 0x2F, 0x03});
	CHECK(m.status == ParseStatus::Ok);
	CHECK_EQ(m.bpm, 62);
	CHECK(m.contact == ContactStatus::Detected);
	CHECK_EQ(size_t(m.rrCount), size_t(1));
}

TEST(display_formats)
{
	CHECK_EQ(formatBpm(DisplayStyle::NumberOnly, 82), std::string("82"));
	CHECK_EQ(formatBpm(DisplayStyle::NumberUnit, 82), std::string("82 BPM"));
	CHECK_EQ(formatBpm(DisplayStyle::HeartNumber, 82), std::string("\xE2\x99\xA5 82"));
	CHECK_EQ(formatBpm(DisplayStyle::HeartNumberUnit, 82), std::string("\xE2\x99\xA5 82 BPM"));
	CHECK(displayStyleFromInt(99) == DisplayStyle::HeartNumberUnit);
	CHECK(kDefaultDisplayStyle == DisplayStyle::HeartNumberUnit);
}

TEST(identity_keys_roundtrip)
{
	DeviceIdentity id;
	id.address = 0xA09E1A123456ULL;
	id.addressType = AddressType::Random;
	CHECK_EQ(id.key(), std::string("ble:random:A0:9E:1A:12:34:56"));
	auto parsed = identityFromKey(id.key());
	CHECK(parsed.has_value());
	CHECK(parsed->sameDevice(id));

	CHECK(!identityFromKey("Polar H10").has_value());
	CHECK(!identityFromKey("ble:public:00:00:00:00:00:00").has_value());
	CHECK(!identityFromKey("ble:public:A0:9E:1A:12:34").has_value());
	CHECK(!identityFromKey("ble:public:A0-9E-1A-12-34-56").has_value());
}

TEST(heart_animation_pulse_shape)
{
	CHECK(heartPulse(0.06) > 0.95f);                    // "lub" peak
	CHECK(heartPulse(0.30) > 0.5f);                     // softer "dub"
	CHECK(heartPulse(0.30) < heartPulse(0.06));
	CHECK(heartPulse(0.6) < 0.01f);                     // resting
	CHECK(heartPulse(0.95) < 0.05f);
	for (int i = 0; i < 100; ++i) {
		const float v = heartPulse(i / 100.0);
		CHECK(v >= 0.0f && v <= 1.0f);
	}
}

TEST(heart_animation_follows_bpm)
{
	HeartBeatAnimator a;
	a.advance(0.5, 60); // half a beat at 60 BPM
	CHECK(std::fabs(a.phase() - 0.5) < 1e-9);
	a.advance(0.5, 120); // a full beat at 120 BPM wraps around
	CHECK(std::fabs(a.phase() - 0.5) < 1e-9);
	a.advance(1.0, 0); // no BPM: frozen
	CHECK(std::fabs(a.phase() - 0.5) < 1e-9);
	a.reset();
	CHECK(a.phase() == 0.0);
	a.advance(0.06, 60);
	CHECK(a.scale(0.2f) > 1.18f && a.scale(0.2f) <= 1.2f);
	CHECK(a.scale(0.0f) == 1.0f);
}

TEST(display_number_parts)
{
	CHECK(styleShowsHeart(DisplayStyle::HeartNumberUnit));
	CHECK(styleShowsHeart(DisplayStyle::HeartNumber));
	CHECK(!styleShowsHeart(DisplayStyle::NumberUnit));
	CHECK_EQ(formatNumber(DisplayStyle::HeartNumberUnit, 82), std::string("82 BPM"));
	CHECK_EQ(formatNumber(DisplayStyle::HeartNumber, 82), std::string("82"));
	CHECK_EQ(numberLayoutSample(DisplayStyle::NumberUnit), std::string("888 BPM"));
}

TEST(ftms_indoor_bike_heart_rate)
{
	// flags 0x0244: More Data=0 (speed present), cadence (bit2), power (bit6), HR (bit9)
	// speed 0x0A00, cadence 0x00B4, power 0x00C8, HR 142
	auto m = parseMeasurement(MeasurementKind::IndoorBike, {0x44, 0x02, 0x00, 0x0A, 0xB4, 0x00, 0xC8, 0x00, 142});
	CHECK(m.status == ParseStatus::Ok);
	CHECK_EQ(m.bpm, 142);
	// Same machine without a heart-rate source: no HR flag.
	auto none = parseMeasurement(MeasurementKind::IndoorBike, {0x44, 0x00, 0x00, 0x0A, 0xB4, 0x00, 0xC8, 0x00});
	CHECK(none.status == ParseStatus::NoHeartRate);
	CHECK(!none.isDisplayable());
	// HR flag set but truncated.
	CHECK(parseMeasurement(MeasurementKind::IndoorBike, {0x44, 0x02, 0x00, 0x0A, 0xB4, 0x00, 0xC8, 0x00}).status ==
	      ParseStatus::Malformed);
	// HR 0 = not available.
	CHECK(parseMeasurement(MeasurementKind::IndoorBike, {0x01, 0x02, 0x00}).status == ParseStatus::InvalidValue);
	// Energy (5 bytes) before HR: flags bit8|bit9, More Data=1
	auto e = parseMeasurement(MeasurementKind::IndoorBike, {0x01, 0x03, 1, 2, 3, 4, 5, 99});
	CHECK(e.status == ParseStatus::Ok);
	CHECK_EQ(e.bpm, 99);
}

TEST(ftms_treadmill_rower_crosstrainer_heart_rate)
{
	// Treadmill: More Data=1, distance (bit2, 3 bytes), HR (bit8)
	auto t = parseMeasurement(MeasurementKind::Treadmill, {0x05, 0x01, 1, 2, 3, 128});
	CHECK(t.status == ParseStatus::Ok);
	CHECK_EQ(t.bpm, 128);
	// Treadmill pace fields are 1 byte each: bits 5,6 + HR
	auto tp = parseMeasurement(MeasurementKind::Treadmill, {0x61, 0x01, 7, 8, 131});
	CHECK_EQ(tp.bpm, 131);
	// Rower: More Data=0 -> stroke rate(1)+count(2) present, HR bit9
	auto r = parseMeasurement(MeasurementKind::Rower, {0x00, 0x02, 20, 0x10, 0x00, 117});
	CHECK(r.status == ParseStatus::Ok);
	CHECK_EQ(r.bpm, 117);
	// Cross trainer: 24-bit flags, More Data=1, HR bit11
	auto c = parseMeasurement(MeasurementKind::CrossTrainer, {0x01, 0x08, 0x00, 133});
	CHECK(c.status == ParseStatus::Ok);
	CHECK_EQ(c.bpm, 133);
	CHECK(parseMeasurement(MeasurementKind::CrossTrainer, {0x01, 0x08}).status == ParseStatus::Malformed);
}

TEST(plx_pulse_rate)
{
	// flags 0, SpO2 97 (0x0061), PR 72 (0x0048), exponent 0
	auto m = parseMeasurement(MeasurementKind::PlxContinuous, {0x00, 0x61, 0x00, 0x48, 0x00});
	CHECK(m.status == ParseStatus::Ok);
	CHECK_EQ(m.bpm, 72);
	// PR 725 * 10^-1 = 72.5 -> 73 (exponent 0xF = -1)
	auto f = parseMeasurement(MeasurementKind::PlxSpotCheck, {0x00, 0x61, 0x00, 0xD5, 0xF2});
	CHECK(f.status == ParseStatus::Ok);
	CHECK_EQ(f.bpm, 73);
	// NaN pulse rate (finger out) -> invalid, never shown
	CHECK(parseMeasurement(MeasurementKind::PlxContinuous, {0x00, 0xFF, 0x07, 0xFF, 0x07}).status ==
	      ParseStatus::InvalidValue);
	CHECK(parseMeasurement(MeasurementKind::PlxContinuous, {0x00, 0x61, 0x00, 0x48}).status == ParseStatus::Malformed);
}

TEST(state_ignores_packets_without_heart_rate)
{
	SensorLinkMachine m;
	m.subscribing();
	m.subscribed(1000000000LL);
	m.measurement(parseMeasurement(MeasurementKind::IndoorBike, {0x01, 0x02, 90}), 2000000000LL);
	m.measurement(parseMeasurement(MeasurementKind::IndoorBike, {0x01, 0x02, 90}), 2000000000LL);
	CHECK(shouldRender(m.snapshot(), 2000000000LL, 5000000000LL));
	m.measurement(parseMeasurement(MeasurementKind::IndoorBike, {0x01, 0x00}), 3000000000LL);
	CHECK_EQ(m.snapshot().malformedPackets, uint64_t(0));
	CHECK(shouldRender(m.snapshot(), 3000000000LL, 5000000000LL)); // still fresh from 2 s
	CHECK(!shouldRender(m.snapshot(), 7000000000LL, 5000000000LL)); // not refreshed
}
