// DEVELOPMENT ONLY. Compiled only with -DOBS_BLE_SENSORS_DEV_MOCK=ON and activated only
// by the environment variable OBS_BLE_SENSORS_MOCK=1. Never part of a release build.
//
// Simulates one heart-rate sensor so the OBS integration (rendering, hiding, reconnect)
// can be exercised without hardware. Repeating 40 s cycle:
//   0-20 s  notifications every second (70-79 BPM)  -> visible
//  20-30 s  connected but silent                    -> hidden after the stale timeout
//  30-35 s  sensor switched off / out of range      -> hidden immediately
//  35-40 s  sensor back on                          -> reconnects, visible on next packet
#pragma once

#include "core/mock_backend.hpp"

#include <atomic>
#include <memory>
#include <thread>

namespace blehr {

class DevSimulator {
public:
	static constexpr uint64_t kAddress = 0xDE0000000001ULL;

	DevSimulator();
	~DevSimulator();

	std::unique_ptr<BleBackend> takeBackend(); // Call once; the simulator keeps a raw pointer.
	void start();
	void stop(); // Must be called before the backend is destroyed.

private:
	void run();

	std::unique_ptr<MockBackend> owned_;
	MockBackend *mock_ = nullptr;
	std::atomic<bool> running_{false};
	CancelToken stop_;
	std::thread thread_;
};

} // namespace blehr
