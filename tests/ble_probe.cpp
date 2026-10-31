// Diagnostic console tool (not shipped, not needed at runtime): exercises the real
// Windows BLE backend through the same BleManager the plugin uses.
//
//   ble-probe                       scan 20 s and list advertising devices
//   ble-probe <key> [seconds]       connect to a sensor key (ble:public:AA:BB:...) and
//                                   print live state/BPM once per second
#include "core/ble_manager.hpp"
#include "windows/winrt_backend.hpp"

#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <thread>

using namespace blehr;

int main(int argc, char **argv)
{
	auto manager = BleManager::create(createWinRtBackend());

	if (argc < 2) {
		std::mutex m;
		std::map<std::string, Advertisement> seen;
		auto token = manager->addScanListener([&](const Advertisement &a) {
			std::lock_guard<std::mutex> lock(m);
			auto &e = seen[a.identity.key()];
			const bool isNew = e.identity.address == 0;
			std::string name = a.name.empty() ? e.name : a.name;
			const bool hr = e.advertisesHeartRate || a.advertisesHeartRate;
			e = a;
			e.name = name;
			e.advertisesHeartRate = hr;
			if (isNew)
				std::printf("found %s rssi=%d name=\"%s\"%s\n", a.identity.key().c_str(), a.rssi, name.c_str(),
					    hr ? " [Heart Rate service]" : "");
		});
		std::printf("Scanning for 20 s...\n");
		std::this_thread::sleep_for(std::chrono::seconds(20));
		token.reset();
		std::lock_guard<std::mutex> lock(m);
		int hr = 0;
		for (const auto &kv : seen)
			hr += kv.second.advertisesHeartRate ? 1 : 0;
		std::printf("\n%zu device(s) advertising, %d with the Heart Rate service\n", seen.size(), hr);
		for (const auto &kv : seen)
			if (kv.second.advertisesHeartRate || !kv.second.name.empty())
				std::printf("  %s  \"%s\"%s\n", kv.first.c_str(), kv.second.name.c_str(),
					    kv.second.advertisesHeartRate ? "  [HR]" : "");
		manager->shutdown();
		return 0;
	}

	auto identity = identityFromKey(argv[1]);
	if (!identity) {
		std::printf("invalid key: %s\n", argv[1]);
		return 2;
	}
	const int seconds = argc > 2 ? std::atoi(argv[2]) : 60;
	auto sub = manager->subscribe(*identity, true);
	for (int i = 0; i < seconds; ++i) {
		std::this_thread::sleep_for(std::chrono::seconds(1));
		const auto snap = sub->snapshot();
		const int64_t now = monotonicNs();
		const bool visible = shouldRender(snap, now, 5LL * 1000000000LL);
		std::printf("[%3ds] %-20s visible=%d bpm=%u  %s\n", i + 1, stateName(effectiveState(snap, now, 5LL * 1000000000LL)),
			    visible ? 1 : 0, visible ? snap.bpm : 0, friendlyStatus(snap, now, 5LL * 1000000000LL).c_str());
		std::fflush(stdout);
	}
	sub.reset();
	manager->shutdown();
	return 0;
}
