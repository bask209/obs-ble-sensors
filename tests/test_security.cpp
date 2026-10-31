// Security-oriented tests: everything that arrives over the radio is untrusted.
#include "test_framework.hpp"

#include "core/device_identity.hpp"
#include "core/heart_rate_parser.hpp"
#include "core/sensor_state.hpp"

#include <random>
#include <vector>

using namespace blehr;

TEST(fuzz_all_parsers_never_read_out_of_bounds)
{
	// Each packet is copied into an exactly-sized heap buffer so that an out-of-bounds
	// read would hit allocator guard regions (and is caught by ASan builds).
	const MeasurementKind kinds[] = {MeasurementKind::HeartRateService, MeasurementKind::IndoorBike,
					 MeasurementKind::Treadmill,        MeasurementKind::Rower,
					 MeasurementKind::CrossTrainer,     MeasurementKind::PlxContinuous,
					 MeasurementKind::PlxSpotCheck};
	std::mt19937 rng(12345);
	uint64_t ok = 0, total = 0;
	for (const MeasurementKind kind : kinds) {
		for (int i = 0; i < 300000; ++i) {
			const size_t len = rng() % 40;
			std::vector<uint8_t> packet(len);
			for (auto &b : packet)
				b = uint8_t(rng());
			// Bias half the packets toward "all fields present" flags.
			if (len > 0 && (i & 1))
				packet[0] = 0xFF;
			if (len > 1 && (i & 1))
				packet[1] = 0xFF;
			const HeartRateMeasurement m = parseMeasurement(kind, packet);
			total++;
			if (m.status == ParseStatus::Ok) {
				ok++;
				CHECK(m.bpm >= kMinValidBpm && m.bpm <= kMaxValidBpm);
			}
			CHECK(size_t(m.rrCount) <= len / 2);
		}
	}
	CHECK(total == 7 * 300000ULL);
	CHECK(ok > 0); // The fuzzer does reach the success paths.
}

TEST(fuzz_state_machine_with_random_packets)
{
	std::mt19937 rng(777);
	SensorLinkMachine m;
	m.subscribing();
	m.subscribed(1);
	int64_t t = 1;
	for (int i = 0; i < 200000; ++i) {
		std::vector<uint8_t> packet(rng() % 8);
		for (auto &b : packet)
			b = uint8_t(rng());
		t += 1000000;
		m.measurement(parseHeartRateMeasurement(packet), t);
		const SensorSnapshot &s = m.snapshot();
		if (shouldRender(s, t, 5000000000LL))
			CHECK(s.bpm >= kMinValidBpm && s.bpm <= kMaxValidBpm && !s.contactLost);
	}
}

TEST(device_names_are_sanitized)
{
	CHECK_EQ(sanitizeDeviceName("Polar H10 A1B2"), std::string("Polar H10 A1B2"));
	CHECK_EQ(sanitizeDeviceName("  spaced  "), std::string("spaced"));
	CHECK_EQ(sanitizeDeviceName(std::string("ab\x00\x01\x1b[31mcd\x7f", 12)), std::string("ab[31mcd"));
	CHECK_EQ(sanitizeDeviceName("Caf\xC3\xA9 \xE2\x99\xA5"), std::string("Caf\xC3\xA9 \xE2\x99\xA5")); // valid UTF-8 kept
	CHECK_EQ(sanitizeDeviceName("bad\xC3"), std::string("bad"));              // truncated sequence
	CHECK_EQ(sanitizeDeviceName("x\xFF\xFEy"), std::string("xy"));            // invalid bytes
	CHECK_EQ(sanitizeDeviceName(std::string(500, 'A')).size(), size_t(64));    // length cap
	CHECK_EQ(sanitizeDeviceName(std::string(30, '\x01')), std::string());
	// Never splits a multi-byte character at the cap.
	std::string hearts;
	for (int i = 0; i < 40; ++i)
		hearts += "\xE2\x99\xA5";
	const std::string capped = sanitizeDeviceName(hearts);
	CHECK(capped.size() <= 64 && capped.size() % 3 == 0);
}

TEST(identity_keys_reject_malicious_input)
{
	CHECK(!identityFromKey("").has_value());
	CHECK(!identityFromKey("ble:public:").has_value());
	CHECK(!identityFromKey("ble:public:ZZ:00:00:00:00:01").has_value());
	CHECK(!identityFromKey("ble:public:AA:BB:CC:DD:EE:FF:00").has_value());
	CHECK(!identityFromKey(std::string("ble:public:AA:BB\0CC:DD:EE:FF", 28)).has_value());
	CHECK(!identityFromKey(std::string(100000, 'A')).has_value());
	DeviceIdentity rpa;
	rpa.address = 0x705CF5BF6280ULL; // top bits 01
	rpa.addressType = AddressType::Random;
	CHECK(rpa.isResolvablePrivate());
	rpa.address = 0xC05CF5BF6280ULL; // static random (11)
	CHECK(!rpa.isResolvablePrivate());
}

TEST(render_word_matches_visibility_rule)
{
	// The packed word must agree with shouldRender() everywhere except within 1 ms of the
	// timeout edge (millisecond packing).
	std::mt19937_64 rng(99);
	const int64_t ms = 1000000;
	int checked = 0;
	for (int i = 0; i < 300000; ++i) {
		SensorSnapshot s;
		const int st = int(rng() % 11);
		s.state = SensorState(st);
		s.hasMeasurement = rng() & 1;
		s.contactLost = (rng() % 4) == 0;
		s.bpm = uint16_t(rng() % 400);
		s.lastValidNs = int64_t(rng() % (1000LL * 1000 * ms)) + 1; // up to ~11 days
		s.connectionGeneration = rng();
		const int64_t now = s.lastValidNs + int64_t(rng() % (20000 * ms)) - 2000 * ms;
		const int64_t timeout = (2 + int64_t(rng() % 14)) * 1000 * ms;
		uint16_t bpm = 0;
		uint64_t gen = 0;
		const bool a = shouldRender(s, now, timeout);
		const bool b = RenderWord::evaluate(RenderWord::pack(s), now, timeout, bpm, gen);
		const int64_t edge = now - s.lastValidNs - timeout;
		if (edge > -2 * ms && edge < 2 * ms)
			continue;
		if (now >= s.lastValidNs && now - s.lastValidNs < 2 * ms)
			continue;
		CHECK(a == b);
		if (a && b) {
			CHECK(bpm == s.bpm);
			CHECK(gen == (s.connectionGeneration & 0x3FFF));
		}
		checked++;
	}
	CHECK(checked > 200000);
}
