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
			CHECK(m.rrIntervals1024.size() <= len / 2);
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
