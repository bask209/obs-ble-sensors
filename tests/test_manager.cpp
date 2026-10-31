// BleManager + MockBackend: discovery, connection, notifications, disconnects,
// reconnection, sharing and shutdown safety.
#include "test_framework.hpp"

#include "core/ble_manager.hpp"
#include "core/mock_backend.hpp"

#include <atomic>

using namespace blehr;
using testing::eventually;
using ms = std::chrono::milliseconds;

namespace {
constexpr int64_t kTimeout = 5LL * 1000000000LL;
constexpr uint64_t kPolar = 0xA09E1A000001ULL;
constexpr uint64_t kWahoo = 0xC10000000002ULL;
constexpr uint64_t kWatch = 0xD20000000003ULL;

DeviceIdentity ident(uint64_t address)
{
	DeviceIdentity id;
	id.address = address;
	return id;
}

struct Fixture {
	MockBackend *mock = nullptr;
	std::shared_ptr<BleManager> manager;

	explicit Fixture(const ManagerConfig &cfg = fastConfig())
	{
		auto backend = std::make_unique<MockBackend>();
		mock = backend.get();
		for (auto addr : {kPolar, kWahoo}) {
			MockDevice d;
			d.identity = ident(addr);
			d.name = addr == kPolar ? "Polar H10" : "TICKR";
			mock->addDevice(d);
		}
		MockDevice watch;
		watch.identity = ident(kWatch);
		watch.name = "Proprietary Watch";
		watch.advertisesHeartRate = false;
		watch.supportsHeartRate = false;
		mock->addDevice(watch);
		manager = BleManager::create(std::move(backend), cfg);
	}

	// Sends a payload twice: a reading is shown only once a second one confirms that the
	// device is streaming.
	void feed(uint64_t address, const std::vector<uint8_t> &payload)
	{
		mock->notify(address, payload);
		mock->notify(address, payload);
	}

	static ManagerConfig fastConfig()
	{
		ManagerConfig c;
		c.silentBackoffInitial = ms(20);
		c.silentBackoffMax = ms(200);
		c.longAbsenceAfter = ms(60000);
		c.longAbsenceRetry = ms(30000);
		c.backoffInitial = ms(20);
		c.backoffMax = ms(200);
		c.silenceReconnect = ms(400);
		c.advertWakeMinInterval = ms(10);
		c.backoffSeed = 7;
		return c;
	}

	static bool live(BleManager::Subscription &s, uint16_t expected)
	{
		uint16_t bpm = 0;
		uint64_t gen = 0;
		return s.liveBpm(monotonicNs(), kTimeout, bpm, gen) && bpm == expected;
	}
	static bool hidden(BleManager::Subscription &s)
	{
		uint16_t bpm = 0;
		uint64_t gen = 0;
		return !s.liveBpm(monotonicNs(), kTimeout, bpm, gen);
	}
};
} // namespace

TEST(manager_connects_and_shows_72_bpm)
{
	Fixture f;
	auto sub = f.manager->subscribe(ident(kPolar), true);
	CHECK(sub != nullptr);
	CHECK(Fixture::hidden(*sub)); // Nothing before the first measurement.
	CHECK(eventually([&] { return f.mock->liveSessions(kPolar) == 1; }));
	CHECK(Fixture::hidden(*sub)); // Connected but no data yet.
	f.feed(kPolar, {0x00, 72});
	CHECK(Fixture::live(*sub, 72));
	CHECK(sub->snapshot().state == SensorState::Live);
}

TEST(manager_uint16_notification)
{
	Fixture f;
	auto sub = f.manager->subscribe(ident(kPolar), true);
	CHECK(eventually([&] { return f.mock->liveSessions(kPolar) == 1; }));
	f.feed(kPolar, {0x01, 0x52, 0x00});
	CHECK(Fixture::live(*sub, 82));
}

TEST(manager_malformed_data_never_shows)
{
	Fixture f;
	auto sub = f.manager->subscribe(ident(kPolar), true);
	CHECK(eventually([&] { return f.mock->liveSessions(kPolar) == 1; }));
	f.feed(kPolar, {0x01});
	f.feed(kPolar, {});
	f.feed(kPolar, {0x00, 0x00});
	CHECK(Fixture::hidden(*sub));
	CHECK(sub->snapshot().malformedPackets == 4); // each payload fed twice
}

TEST(manager_sudden_disconnect_hides_then_reconnects)
{
	Fixture f;
	auto sub = f.manager->subscribe(ident(kPolar), true);
	CHECK(eventually([&] { return f.mock->liveSessions(kPolar) == 1; }));
	f.feed(kPolar, {0x00, 82});
	CHECK(Fixture::live(*sub, 82));

	f.mock->dropConnection(kPolar);
	CHECK(Fixture::hidden(*sub)); // Immediately, not after the timeout.

	CHECK(eventually([&] { return f.mock->successfulConnections(kPolar) == 2 && f.mock->liveSessions(kPolar) == 1; }));
	CHECK(Fixture::hidden(*sub)); // Reconnected, waiting for a fresh measurement.
	f.feed(kPolar, {0x00, 90});
	CHECK(Fixture::live(*sub, 90));
}

TEST(manager_sensor_off_then_on_again)
{
	Fixture f;
	auto sub = f.manager->subscribe(ident(kPolar), true);
	CHECK(eventually([&] { return f.mock->liveSessions(kPolar) == 1; }));
	f.feed(kPolar, {0x00, 70});
	f.mock->setPresent(kPolar, false);
	f.mock->dropConnection(kPolar, "sensor turned off");
	CHECK(Fixture::hidden(*sub));
	CHECK(eventually([&] { return sub->snapshot().state == SensorState::Searching; }));
	const int attempts = f.mock->connectAttempts(kPolar);
	CHECK(eventually([&] { return f.mock->connectAttempts(kPolar) > attempts + 1; })); // keeps retrying
	CHECK(Fixture::hidden(*sub));

	f.mock->setPresent(kPolar, true);
	CHECK(eventually([&] { return f.mock->liveSessions(kPolar) == 1; }));
	f.feed(kPolar, {0x00, 71});
	CHECK(Fixture::live(*sub, 71));
}

TEST(manager_no_notifications_stays_hidden_and_reconnects)
{
	Fixture f;
	auto sub = f.manager->subscribe(ident(kPolar), true);
	CHECK(eventually([&] { return f.mock->liveSessions(kPolar) == 1; }));
	CHECK(Fixture::hidden(*sub));
	// Silence watchdog (400 ms in tests) forces a fresh connection.
	CHECK(eventually([&] { return f.mock->successfulConnections(kPolar) >= 2; }));
	CHECK(Fixture::hidden(*sub));
}

TEST(manager_data_stops_hidden_within_timeout)
{
	Fixture f;
	auto sub = f.manager->subscribe(ident(kPolar), true);
	CHECK(eventually([&] { return f.mock->liveSessions(kPolar) == 1; }));
	f.feed(kPolar, {0x00, 82});
	uint16_t bpm = 0;
	uint64_t gen = 0;
	const int64_t shortTimeout = 2LL * 1000000000LL;
	CHECK(sub->liveBpm(monotonicNs(), shortTimeout, bpm, gen));
	CHECK(!sub->liveBpm(monotonicNs() + shortTimeout, shortTimeout, bpm, gen));
}

TEST(manager_contact_lost)
{
	Fixture f;
	auto sub = f.manager->subscribe(ident(kPolar), true);
	CHECK(eventually([&] { return f.mock->liveSessions(kPolar) == 1; }));
	f.feed(kPolar, {0x06, 75});
	CHECK(Fixture::live(*sub, 75));
	f.feed(kPolar, {0x04, 75});
	CHECK(Fixture::hidden(*sub));
	CHECK(sub->snapshot().contactLost);
}

TEST(manager_device_not_found)
{
	Fixture f;
	auto sub = f.manager->subscribe(ident(0x112233445566ULL), true);
	CHECK(eventually([&] { return sub->snapshot().state == SensorState::Searching &&
				      f.mock->connectAttempts(0x112233445566ULL) >= 2; }));
	CHECK(Fixture::hidden(*sub));
}

TEST(manager_unsupported_device)
{
	Fixture f;
	auto sub = f.manager->subscribe(ident(kWatch), true);
	CHECK(eventually([&] { return sub->snapshot().state == SensorState::Unsupported; }));
	CHECK(Fixture::hidden(*sub));
	CHECK(friendlyStatus(sub->snapshot(), monotonicNs(), kTimeout).find("does not provide") != std::string::npos);
	// Safety: a non-heart-rate device is left alone, never reconnected automatically
	// (backoff max is 200 ms in tests, so retries would show up quickly).
	std::this_thread::sleep_for(ms(700));
	CHECK_EQ(f.mock->connectAttempts(kWatch), 1);
	CHECK(!f.manager->scanWanted());
	// Only an explicit user request tries again.
	sub->reconnectNow();
	CHECK(eventually([&] { return f.mock->connectAttempts(kWatch) == 2; }));
}

TEST(manager_no_background_scan_by_default)
{
	Fixture f;
	f.mock->setPresent(kPolar, false);
	auto sub = f.manager->subscribe(ident(kPolar), true);
	CHECK(eventually([&] { return f.mock->connectAttempts(kPolar) >= 3; }));
	CHECK(!f.mock->scanning()); // periodic retries only; no radio listening in between
}

TEST(manager_background_scan_ignores_unrelated_devices)
{
	ManagerConfig cfg = Fixture::fastConfig();
	cfg.backgroundScan = true;
	Fixture f(cfg);
	f.mock->setPresent(kPolar, false);
	auto sub = f.manager->subscribe(ident(kPolar), true);
	CHECK(eventually([&] { return f.mock->scanning(); }));
	CHECK(!f.mock->scanningActive()); // passive: never transmits scan requests
	f.mock->advertise(kWahoo);        // someone else's device
	f.mock->advertise(kWatch);
	CHECK_EQ(f.mock->filteredAdvertisements(), 2);
}

TEST(manager_silent_sensor_backs_off)
{
	ManagerConfig cfg = Fixture::fastConfig();
	cfg.backoffInitial = ms(300);
	cfg.backoffMax = ms(5000);
	cfg.silenceReconnect = ms(100);
	cfg.silentBackoffInitial = ms(300);
	cfg.silentBackoffMax = ms(5000);
	Fixture f(cfg);
	auto sub = f.manager->subscribe(ident(kPolar), true);
	std::this_thread::sleep_for(ms(2000));
	// Connected but never sends data: ~0 s, ~0.45 s, ~1.15 s, then >2 s. Without growing
	// backoff it would be about 5 connections.
	CHECK(f.mock->successfulConnections(kPolar) <= 3);
	CHECK(f.mock->successfulConnections(kPolar) >= 2);
}

TEST(manager_shared_connection_for_two_sources)
{
	Fixture f;
	auto a = f.manager->subscribe(ident(kPolar), true);
	auto b = f.manager->subscribe(ident(kPolar), true);
	CHECK(eventually([&] { return f.mock->liveSessions(kPolar) == 1; }));
	CHECK_EQ(f.manager->activeLinkCount(), size_t(1));
	f.feed(kPolar, {0x00, 101});
	CHECK(Fixture::live(*a, 101));
	CHECK(Fixture::live(*b, 101));
	CHECK_EQ(f.mock->successfulConnections(kPolar), 1);

	a.reset(); // One source removed: connection stays for the other.
	f.feed(kPolar, {0x00, 102});
	CHECK(Fixture::live(*b, 102));
	CHECK_EQ(f.mock->liveSessions(kPolar), 1);

	b.reset();
	CHECK(eventually([&] { return f.mock->liveSessions(kPolar) == 0; }));
	CHECK_EQ(f.manager->activeLinkCount(), size_t(0));
}

TEST(manager_switching_device_has_no_stale_data)
{
	Fixture f;
	auto sub = f.manager->subscribe(ident(kPolar), true);
	CHECK(eventually([&] { return f.mock->liveSessions(kPolar) == 1; }));
	f.feed(kPolar, {0x00, 150});
	CHECK(Fixture::live(*sub, 150));

	sub = f.manager->subscribe(ident(kWahoo), true); // old subscription released
	CHECK_EQ(sub->key(), ident(kWahoo).key());
	CHECK(Fixture::hidden(*sub));
	CHECK(eventually([&] { return f.mock->liveSessions(kWahoo) == 1; }));
	CHECK(Fixture::hidden(*sub));
	f.feed(kPolar, {0x00, 151}); // old device data must not leak
	CHECK(Fixture::hidden(*sub));
	f.feed(kWahoo, {0x00, 66});
	CHECK(Fixture::live(*sub, 66));
	CHECK(eventually([&] { return f.mock->liveSessions(kPolar) == 0; }));
}

TEST(manager_bluetooth_off_and_on)
{
	Fixture f;
	auto sub = f.manager->subscribe(ident(kPolar), true);
	CHECK(eventually([&] { return f.mock->liveSessions(kPolar) == 1; }));
	f.feed(kPolar, {0x00, 88});
	f.mock->setBluetoothOn(false);
	CHECK(Fixture::hidden(*sub));
	CHECK(eventually([&] { return sub->snapshot().state == SensorState::BluetoothOff; }));
	f.mock->setBluetoothOn(true);
	CHECK(eventually([&] { return f.mock->liveSessions(kPolar) == 1; }));
	CHECK(Fixture::hidden(*sub));
	f.feed(kPolar, {0x00, 89});
	CHECK(Fixture::live(*sub, 89));
}

TEST(manager_auto_reconnect_disabled_waits_for_request)
{
	Fixture f;
	auto sub = f.manager->subscribe(ident(kPolar), false);
	CHECK(eventually([&] { return f.mock->liveSessions(kPolar) == 1; })); // first connect still happens
	f.mock->dropConnection(kPolar);
	CHECK(eventually([&] { return sub->snapshot().state == SensorState::Disconnected; }));
	std::this_thread::sleep_for(ms(300));
	CHECK_EQ(f.mock->successfulConnections(kPolar), 1);
	sub->reconnectNow();
	CHECK(eventually([&] { return f.mock->successfulConnections(kPolar) == 2; }));
	// Turning auto reconnect back on resumes automatic behaviour.
	sub->setAutoReconnect(true);
	f.mock->dropConnection(kPolar);
	CHECK(eventually([&] { return f.mock->successfulConnections(kPolar) == 3; }));
}

TEST(manager_advertisement_wakes_reconnect_early)
{
	ManagerConfig cfg = Fixture::fastConfig();
	cfg.backgroundScan = true;
	cfg.backoffInitial = ms(5000);
	cfg.backoffMax = ms(5000);
	Fixture f(cfg);
	f.mock->setPresent(kPolar, false);
	auto sub = f.manager->subscribe(ident(kPolar), true);
	CHECK(eventually([&] { return sub->snapshot().state == SensorState::Searching; }));
	CHECK(eventually([&] { return f.manager->scanWanted() && f.mock->scanning(); }));
	CHECK(!f.mock->scanningActive()); // background scanning is passive
	f.mock->setPresent(kPolar, true);
	f.mock->advertise(kPolar);
	CHECK(eventually([&] { return f.mock->liveSessions(kPolar) == 1; }, 1500)); // well before 5 s
	CHECK(eventually([&] { return !f.mock->scanning(); })); // no scanning while connected
}

TEST(manager_scan_listener_receives_devices)
{
	Fixture f;
	std::atomic<int> seen{0};
	std::atomic<bool> sawWatchWithoutHr{false};
	{
		auto token = f.manager->addScanListener([&](const Advertisement &adv) {
			seen++;
			if (adv.identity.address == kWatch && !adv.advertisesHeartRate)
				sawWatchWithoutHr = true;
		});
		CHECK(f.mock->scanningActive());
		f.mock->advertise(kPolar);
		f.mock->advertise(kWatch);
		CHECK_EQ(seen.load(), 2);
		CHECK(sawWatchWithoutHr.load());
	}
	CHECK(!f.mock->scanning());
	f.mock->advertise(kPolar);
	CHECK_EQ(seen.load(), 2); // No callbacks after the token is gone.
}

TEST(manager_release_during_connection_attempt)
{
	Fixture f;
	f.mock->setConnectDelay(ms(2000));
	auto sub = f.manager->subscribe(ident(kPolar), true);
	CHECK(eventually([&] { return f.mock->connectAttempts(kPolar) == 1; }));
	const auto start = std::chrono::steady_clock::now();
	sub.reset(); // Must not block the caller.
	CHECK(std::chrono::steady_clock::now() - start < ms(100));
	CHECK_EQ(f.manager->activeLinkCount(), size_t(0));
}

TEST(manager_shutdown_while_connecting_is_fast_and_clean)
{
	Fixture f;
	f.mock->setConnectDelay(ms(5000));
	auto a = f.manager->subscribe(ident(kPolar), true);
	auto b = f.manager->subscribe(ident(kWahoo), true);
	auto token = f.manager->addScanListener([](const Advertisement &) {});
	CHECK(eventually([&] { return f.mock->connectAttempts(kPolar) == 1 && f.mock->connectAttempts(kWahoo) == 1; }));
	const auto start = std::chrono::steady_clock::now();
	f.manager->shutdown();
	CHECK(std::chrono::steady_clock::now() - start < ms(1000));
	CHECK(f.manager->subscribe(ident(kPolar), true) == nullptr);
	// Handles outliving shutdown are harmless.
	CHECK(Fixture::hidden(*a));
	a.reset();
	b.reset();
	token.reset();
	f.manager.reset();
}

TEST(manager_destroyed_with_live_subscriptions)
{
	std::unique_ptr<BleManager::Subscription> sub;
	{
		Fixture f;
		sub = f.manager->subscribe(ident(kPolar), true);
		CHECK(eventually([&] { return f.mock->liveSessions(kPolar) == 1; }));
	} // manager destroyed here (joins workers)
	CHECK(Fixture::hidden(*sub));
	sub.reset();
}

TEST(manager_stress_subscribe_release)
{
	Fixture f;
	for (int i = 0; i < 50; ++i) {
		auto a = f.manager->subscribe(ident(i % 2 ? kPolar : kWahoo), true);
		if (i % 5 == 0)
			f.feed(kPolar, {0x00, 60});
	}
	CHECK(eventually([&] { return f.manager->activeLinkCount() == 0; }));
}

TEST(manager_single_replayed_reading_is_not_shown)
{
	// A watch with broadcasting off may replay one stored value on subscription.
	Fixture f;
	auto sub = f.manager->subscribe(ident(kPolar), true);
	CHECK(eventually([&] { return f.mock->liveSessions(kPolar) == 1; }));
	f.mock->notify(kPolar, {0x00, 88});
	CHECK(Fixture::hidden(*sub));
	f.mock->notify(kPolar, {0x00, 89}); // second reading: really streaming
	CHECK(Fixture::live(*sub, 89));
}

TEST(manager_fitness_machine_and_pulse_oximeter)
{
	auto backend = std::make_unique<MockBackend>();
	MockBackend *mock = backend.get();
	MockDevice bike;
	bike.identity = ident(0xB1000000000AULL);
	bike.kind = MeasurementKind::IndoorBike;
	mock->addDevice(bike);
	MockDevice oxi;
	oxi.identity = ident(0xB1000000000BULL);
	oxi.kind = MeasurementKind::PlxContinuous;
	mock->addDevice(oxi);
	auto manager = BleManager::create(std::move(backend), Fixture::fastConfig());
	auto a = manager->subscribe(bike.identity, true);
	auto b = manager->subscribe(oxi.identity, true);
	CHECK(eventually([&] { return mock->liveSessions(bike.identity.address) == 1 && mock->liveSessions(oxi.identity.address) == 1; }));
	for (int i = 0; i < 2; ++i) {
		mock->notify(bike.identity.address, {0x01, 0x02, 131});            // FTMS HR 131
		mock->notify(oxi.identity.address, {0x00, 0x61, 0x00, 0x48, 0x00}); // PLX PR 72
	}
	CHECK(Fixture::live(*a, 131));
	CHECK(Fixture::live(*b, 72));
	manager->shutdown();
}

TEST(manager_reports_adapter_problems)
{
	Fixture f;
	f.mock->setAdapterState(AdapterState::NoLowEnergy);
	auto sub = f.manager->subscribe(ident(kPolar), true);
	CHECK(eventually([&] { return sub->snapshot().state == SensorState::BluetoothOff; }));
	CHECK(friendlyStatus(sub->snapshot(), monotonicNs(), kTimeout).find("Bluetooth LE") != std::string::npos);
	f.mock->setAdapterState(AdapterState::Ready);
	CHECK(eventually([&] { return f.mock->liveSessions(kPolar) == 1; }));
}

TEST(manager_lists_paired_devices_to_scan_listeners)
{
	auto backend = std::make_unique<MockBackend>();
	MockBackend *mock = backend.get();
	MockDevice paired;
	paired.identity = ident(0xC0FFEE000001ULL);
	paired.name = "Paired strap";
	paired.paired = true;
	mock->addDevice(paired);
	auto manager = BleManager::create(std::move(backend), Fixture::fastConfig());
	std::mutex m;
	std::vector<Advertisement> got;
	auto token = manager->addScanListener([&](const Advertisement &a) {
		std::lock_guard<std::mutex> lock(m);
		got.push_back(a);
	});
	manager->requestPairedDevices();
	CHECK(eventually([&] {
		std::lock_guard<std::mutex> lock(m);
		return got.size() == 1 && got[0].paired && got[0].name == "Paired strap";
	}));
	token.reset();
	manager->shutdown();
}

TEST(manager_logs_connection_events_once)
{
	// Declared before the fixture: the callback may run until the manager (and its
	// worker threads) are gone.
	std::mutex m;
	std::vector<std::string> events;
	Fixture f;
	f.manager->setEventLog([&](const std::string &, const std::string &msg) {
		std::lock_guard<std::mutex> lock(m);
		events.push_back(msg);
	});
	f.mock->setPresent(kPolar, false);
	auto sub = f.manager->subscribe(ident(kPolar), true);
	CHECK(eventually([&] { return f.mock->connectAttempts(kPolar) >= 3; }));
	f.mock->setPresent(kPolar, true);
	CHECK(eventually([&] { return f.mock->liveSessions(kPolar) == 1; }));
	f.mock->dropConnection(kPolar, "out of range");
	CHECK(eventually([&] {
		std::lock_guard<std::mutex> lock(m);
		return events.size() >= 3;
	}));
	std::lock_guard<std::mutex> lock(m);
	CHECK(events[0].find("not found") != std::string::npos);
	CHECK(events[1].find("connected to Polar H10") != std::string::npos); // repeated "not found" deduplicated
	CHECK(events[2].find("disconnected") != std::string::npos);
}

TEST(manager_render_query_is_lock_free_and_consistent_under_load)
{
	// One thread streams readings and disconnects as fast as possible while another
	// queries like the OBS render thread. Every visible result must be a value that was
	// actually sent; ThreadSanitizer/ASan in CI check the memory model.
	Fixture f;
	auto sub = f.manager->subscribe(ident(kPolar), true);
	CHECK(eventually([&] { return f.mock->liveSessions(kPolar) == 1; }));
	std::atomic<bool> stop{false};
	std::atomic<uint64_t> queries{0}, visible{0}, bad{0};
	std::thread reader([&] {
		while (!stop) {
			uint16_t bpm = 0;
			uint64_t gen = 0;
			if (sub->liveBpm(monotonicNs(), kTimeout, bpm, gen)) {
				visible++;
				if (bpm < 60 || bpm > 69)
					bad++;
			}
			queries++;
		}
	});
	for (int i = 0; i < 20000; ++i) {
		f.mock->notify(kPolar, {0x00, uint8_t(60 + i % 10)});
		if (i % 2000 == 1999) {
			f.mock->dropConnection(kPolar);
			eventually([&] { return f.mock->liveSessions(kPolar) == 1; });
		}
	}
	stop = true;
	reader.join();
	CHECK(bad.load() == 0);
	CHECK(visible.load() > 0);
	CHECK(queries.load() > 1000);
}

TEST(manager_long_absence_retries_less_often)
{
	ManagerConfig cfg = Fixture::fastConfig();
	cfg.longAbsenceAfter = ms(300);
	cfg.longAbsenceRetry = ms(1000);
	Fixture f(cfg);
	f.mock->setPresent(kPolar, false);
	auto sub = f.manager->subscribe(ident(kPolar), true);
	std::this_thread::sleep_for(ms(400)); // now "absent for a long time"
	const int before = f.mock->connectAttempts(kPolar);
	std::this_thread::sleep_for(ms(1500));
	const int during = f.mock->connectAttempts(kPolar) - before;
	CHECK(during >= 1 && during <= 2); // ~every 1 s instead of every <=200 ms
	f.mock->setPresent(kPolar, true);
	CHECK(eventually([&] { return f.mock->liveSessions(kPolar) == 1; }, 3000));
}
