#include "test_framework.hpp"

#include "core/backoff.hpp"
#include "core/device_registry.hpp"
#include "core/sensor_state.hpp"

#include <atomic>
#include <mutex>
#include <thread>

using namespace blehr;

namespace {
constexpr int64_t kSec = 1000000000LL;
constexpr int64_t kTimeout = 5 * kSec;

HeartRateMeasurement hr(uint16_t bpm, uint8_t flags = 0)
{
	return parseHeartRateMeasurement({flags, uint8_t(bpm)});
}

// Two readings: the first reading of a connection alone is never shown.
void prime(SensorLinkMachine &m, int64_t t, uint16_t bpm, uint8_t flags = 0)
{
	m.measurement(hr(bpm, flags), t);
	m.measurement(hr(bpm, flags), t);
}
} // namespace

TEST(visibility_not_configured_is_hidden)
{
	SensorLinkMachine m;
	CHECK(!shouldRender(m.snapshot(), 100 * kSec, kTimeout));
}

TEST(visibility_hidden_while_connecting_states)
{
	SensorLinkMachine m;
	m.searching();
	CHECK(!shouldRender(m.snapshot(), 100 * kSec, kTimeout));
	m.connecting();
	CHECK(!shouldRender(m.snapshot(), 100 * kSec, kTimeout));
	m.discoveringServices();
	CHECK(!shouldRender(m.snapshot(), 100 * kSec, kTimeout));
	m.subscribing();
	CHECK(!shouldRender(m.snapshot(), 100 * kSec, kTimeout));
	m.subscribed(100 * kSec);
	// Subscribed but no measurement yet: still hidden.
	CHECK(!shouldRender(m.snapshot(), 100 * kSec, kTimeout));
	CHECK(effectiveState(m.snapshot(), 100 * kSec, kTimeout) == SensorState::Stale);
}

TEST(visibility_valid_measurement_shows)
{
	SensorLinkMachine m;
	m.subscribed(10 * kSec);
	prime(m, 11 * kSec, 82);
	CHECK(shouldRender(m.snapshot(), 11 * kSec, kTimeout));
	CHECK_EQ(m.snapshot().bpm, 82);
	CHECK(effectiveState(m.snapshot(), 12 * kSec, kTimeout) == SensorState::Live);
}

TEST(visibility_stale_after_timeout)
{
	SensorLinkMachine m;
	m.subscribed(10 * kSec);
	prime(m, 11 * kSec, 82);
	CHECK(shouldRender(m.snapshot(), 15 * kSec, kTimeout));
	CHECK(!shouldRender(m.snapshot(), 16 * kSec, kTimeout)); // exactly 5 s old -> hidden
	CHECK(!shouldRender(m.snapshot(), 60 * kSec, kTimeout));
	CHECK(effectiveState(m.snapshot(), 16 * kSec, kTimeout) == SensorState::Stale);
	// Configurable timeout: 2 s and 15 s.
	CHECK(!shouldRender(m.snapshot(), 13 * kSec, 2 * kSec));
	CHECK(shouldRender(m.snapshot(), 25 * kSec, 15 * kSec));
}

TEST(visibility_disconnect_hides_immediately)
{
	SensorLinkMachine m;
	m.subscribed(10 * kSec);
	prime(m, 11 * kSec, 82);
	m.disconnected("out of range");
	CHECK(!shouldRender(m.snapshot(), 11 * kSec, kTimeout));
	CHECK_EQ(m.snapshot().bpm, 0);
	CHECK(m.snapshot().state == SensorState::Disconnected);
}

TEST(visibility_contact_lost_hides)
{
	SensorLinkMachine m;
	m.subscribed(10 * kSec);
	prime(m, 11 * kSec, 82, 0x06);
	CHECK(shouldRender(m.snapshot(), 11 * kSec, kTimeout));
	prime(m, 12 * kSec, 82, 0x04); // contact supported, not detected
	CHECK(!shouldRender(m.snapshot(), 12 * kSec, kTimeout));
	CHECK(m.snapshot().contactLost);
	prime(m, 13 * kSec, 84, 0x06); // contact back
	CHECK(shouldRender(m.snapshot(), 13 * kSec, kTimeout));
	CHECK_EQ(m.snapshot().bpm, 84);
}

TEST(visibility_zero_bpm_hides_immediately)
{
	SensorLinkMachine m;
	m.subscribed(10 * kSec);
	prime(m, 11 * kSec, 82);
	prime(m, 12 * kSec, 0);
	CHECK(!shouldRender(m.snapshot(), 12 * kSec, kTimeout));
}

TEST(visibility_malformed_packet_does_not_refresh)
{
	SensorLinkMachine m;
	m.subscribed(10 * kSec);
	prime(m, 11 * kSec, 82);
	m.measurement(parseHeartRateMeasurement({0x01}), 15 * kSec);
	CHECK_EQ(m.snapshot().malformedPackets, uint64_t(1));
	CHECK(shouldRender(m.snapshot(), 15 * kSec, kTimeout));  // previous reading still fresh
	CHECK(!shouldRender(m.snapshot(), 16 * kSec, kTimeout)); // but not refreshed by garbage
}

TEST(visibility_late_packet_after_disconnect_ignored)
{
	SensorLinkMachine m;
	m.subscribed(10 * kSec);
	m.disconnected();
	prime(m, 11 * kSec, 90);
	CHECK(!shouldRender(m.snapshot(), 11 * kSec, kTimeout));
}

TEST(visibility_new_connection_requires_new_measurement)
{
	SensorLinkMachine m;
	m.subscribed(10 * kSec);
	prime(m, 11 * kSec, 82);
	const auto gen = m.snapshot().connectionGeneration;
	m.disconnected();
	m.connecting();
	m.subscribing();
	m.subscribed(12 * kSec);
	CHECK(m.snapshot().connectionGeneration == gen + 1);
	CHECK(!shouldRender(m.snapshot(), 12 * kSec, kTimeout));
	prime(m, 13 * kSec, 77);
	CHECK(shouldRender(m.snapshot(), 13 * kSec, kTimeout));
}

TEST(visibility_packet_during_subscribe_is_kept)
{
	SensorLinkMachine m;
	m.connecting();
	m.subscribing();
	prime(m, 10 * kSec, 66); // arrives before the CCCD write completes
	m.subscribed(10 * kSec);
	CHECK(shouldRender(m.snapshot(), 10 * kSec, kTimeout));
	CHECK_EQ(m.snapshot().bpm, 66);
	// But never during connecting / discovery.
	SensorLinkMachine n;
	n.connecting();
	n.measurement(hr(66), 10 * kSec);
	n.discoveringServices();
	n.measurement(hr(66), 10 * kSec);
	CHECK(!n.snapshot().hasMeasurement);
}

TEST(visibility_unsupported_bluetooth_off_error_hidden)
{
	SensorLinkMachine m;
	m.unsupported("no HR service");
	CHECK(!shouldRender(m.snapshot(), 1, kTimeout));
	m.bluetoothOff();
	CHECK(!shouldRender(m.snapshot(), 1, kTimeout));
	m.error("x");
	CHECK(!shouldRender(m.snapshot(), 1, kTimeout));
}

TEST(visibility_clock_going_backwards_hides)
{
	SensorLinkMachine m;
	m.subscribed(10 * kSec);
	prime(m, 11 * kSec, 82);
	CHECK(!shouldRender(m.snapshot(), 10 * kSec, kTimeout));
}

TEST(friendly_status_texts)
{
	SensorLinkMachine m;
	CHECK_EQ(friendlyStatus(m.snapshot(), 0, kTimeout), std::string("No sensor selected"));
	m.unsupported("");
	CHECK(friendlyStatus(m.snapshot(), 0, kTimeout).find("does not provide") != std::string::npos);
	m.subscribed(kSec);
	prime(m, kSec, 70);
	CHECK(friendlyStatus(m.snapshot(), kSec, kTimeout).find("70 BPM") != std::string::npos);
	CHECK(friendlyStatus(m.snapshot(), 10 * kSec, kTimeout).find("waiting") != std::string::npos);
}

TEST(backoff_grows_and_caps)
{
	Backoff b(std::chrono::milliseconds(1000), std::chrono::milliseconds(30000), 0.2, 42);
	int64_t prev = 0;
	for (int i = 0; i < 4; ++i) {
		auto d = b.next().count();
		const int64_t base = 1000LL << i;
		CHECK(d >= int64_t(base * 0.8) - 1 && d <= int64_t(base * 1.2) + 1);
		CHECK(d > prev / 2);
		prev = d;
	}
	for (int i = 0; i < 30; ++i)
		CHECK(b.next().count() <= 30000);
	b.reset();
	CHECK(b.next().count() <= 1200);
}

TEST(registry_add_rename_forget)
{
	DeviceRegistry reg;
	int writes = 0;
	std::vector<SavedDevice> lastWritten;
	reg.setPersist([&](const std::vector<SavedDevice> &d) {
		writes++;
		lastWritten = d;
		return true;
	});

	SavedDevice polar;
	polar.identity = *identityFromKey("ble:public:A0:9E:1A:00:00:01");
	polar.alias = "Polar H10";
	polar.lastKnownName = "Polar H10 1234";
	SavedDevice wahoo;
	wahoo.identity = *identityFromKey("ble:random:C1:00:00:00:00:02");
	wahoo.lastKnownName = "TICKR 55";

	CHECK(reg.save(polar));
	CHECK(reg.save(wahoo));
	CHECK_EQ(reg.list().size(), size_t(2));
	CHECK_EQ(writes, 2);
	CHECK_EQ(reg.find(wahoo.key())->displayName(), std::string("TICKR 55"));

	CHECK(reg.rename(wahoo.key(), "Wahoo TICKR"));
	CHECK_EQ(reg.find(wahoo.key())->displayName(), std::string("Wahoo TICKR"));
	CHECK_EQ(lastWritten.size(), size_t(2));

	// Saving the same device again updates instead of duplicating.
	polar.alias = "Chest strap";
	CHECK(reg.save(polar));
	CHECK_EQ(reg.list().size(), size_t(2));
	CHECK_EQ(reg.find(polar.key())->alias, std::string("Chest strap"));

	CHECK(reg.forget(polar.key()));
	CHECK(!reg.find(polar.key()).has_value());
	CHECK(!reg.forget(polar.key()));
	CHECK_EQ(lastWritten.size(), size_t(1));
	CHECK(!reg.rename("ble:public:11:11:11:11:11:11", "x"));
}

TEST(registry_note_connected_throttles_writes)
{
	DeviceRegistry reg;
	int writes = 0;
	reg.setPersist([&](const std::vector<SavedDevice> &) {
		writes++;
		return true;
	});
	SavedDevice d;
	d.identity = *identityFromKey("ble:public:A0:00:00:00:00:01");
	reg.save(d);
	writes = 0;
	reg.noteConnected(d.key(), 1000000, "HRM", "BluetoothLE#x");
	CHECK_EQ(writes, 1);
	reg.noteConnected(d.key(), 1000010, "HRM", "BluetoothLE#x");
	CHECK_EQ(writes, 0 + 1);
	reg.noteConnected(d.key(), 1000010 + 3600, "HRM", "BluetoothLE#x");
	CHECK_EQ(writes, 2);
	CHECK_EQ(reg.find(d.key())->identity.windowsDeviceId, std::string("BluetoothLE#x"));
	// Unknown devices are never added implicitly.
	reg.noteConnected("ble:public:B0:00:00:00:00:01", 1, "x", "");
	CHECK_EQ(reg.list().size(), size_t(1));
}

TEST(registry_load_drops_duplicates_and_invalid)
{
	DeviceRegistry reg;
	SavedDevice a;
	a.identity = *identityFromKey("ble:public:A0:00:00:00:00:01");
	SavedDevice invalid;
	reg.load({a, a, invalid});
	CHECK_EQ(reg.list().size(), size_t(1));
}

TEST(visibility_first_reading_of_connection_needs_confirmation)
{
	SensorLinkMachine m;
	m.subscribing();
	m.subscribed(10 * kSec);
	m.measurement(hr(70), 10 * kSec); // could be a replayed stored value
	CHECK(!shouldRender(m.snapshot(), 10 * kSec, kTimeout));
	m.measurement(hr(71), 11 * kSec);
	CHECK(shouldRender(m.snapshot(), 11 * kSec, kTimeout));
	CHECK_EQ(m.snapshot().bpm, 71);
	// Each new connection must confirm again.
	m.disconnected();
	m.subscribing();
	m.subscribed(12 * kSec);
	m.measurement(hr(72), 12 * kSec);
	CHECK(!shouldRender(m.snapshot(), 12 * kSec, kTimeout));
}

TEST(registry_concurrent_saves_never_regress_and_reads_never_wait_on_io)
{
	DeviceRegistry reg;
	std::mutex fileMutex;
	std::vector<SavedDevice> onDisk;
	std::atomic<int> writes{0};
	reg.setPersist([&](const std::vector<SavedDevice> &d) {
		std::this_thread::sleep_for(std::chrono::milliseconds(20)); // slow disk
		std::lock_guard<std::mutex> lock(fileMutex);
		onDisk = d;
		writes++;
		return true;
	});
	std::vector<std::thread> writers;
	for (int t = 0; t < 4; ++t)
		writers.emplace_back([&reg, t] {
			for (int i = 0; i < 5; ++i) {
				SavedDevice d;
				d.identity.address = 0xA00000000000ULL + uint64_t(t * 16 + i);
				reg.save(d);
			}
		});
	// While writers wait on the slow "disk", lookups must stay fast.
	const auto start = std::chrono::steady_clock::now();
	int lookups = 0;
	while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(150)) {
		const auto t0 = std::chrono::steady_clock::now();
		(void)reg.find("ble:public:A0:00:00:00:00:01");
		CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(15));
		lookups++;
	}
	for (auto &w : writers)
		w.join();
	CHECK(lookups > 10);
	std::lock_guard<std::mutex> lock(fileMutex);
	CHECK_EQ(onDisk.size(), size_t(20)); // the newest state is what ended up on disk
	CHECK(writes.load() <= 20);           // stale snapshots were skipped, never written last
}
