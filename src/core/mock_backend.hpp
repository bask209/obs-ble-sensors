// Scriptable BLE backend for automated tests and the explicitly enabled development
// mode. It is compiled into the plugin only when OBS_BLE_SENSORS_DEV_MOCK=ON, which the
// release presets never set. It produces no readings on its own: every notification is
// pushed explicitly by the test (or the dev simulator).
#pragma once

#include "ble_backend.hpp"

#include <map>
#include <mutex>
#include <vector>

namespace blehr {

struct MockDevice {
	DeviceIdentity identity;
	std::string name = "Mock HRM";
	bool advertisesHeartRate = true;
	bool supportsHeartRate = true; // false -> Unsupported on connect.
	bool present = true;           // false -> NotFound on connect.
	bool accessDenied = false;
};

class MockBackend : public BleBackend {
public:
	// --- scripting API (thread safe) ---
	void addDevice(const MockDevice &device);
	void setPresent(uint64_t address, bool present);
	void setBluetoothOn(bool on);
	void setConnectDelay(std::chrono::milliseconds delay);
	// Delivers a raw 0x2A37 payload to the live session of a device. Returns false if none.
	bool notify(uint64_t address, const std::vector<uint8_t> &payload);
	// Simulates a link loss for the live session of a device.
	bool dropConnection(uint64_t address, const std::string &reason = "out of range");
	void advertise(uint64_t address); // Emits one advertisement if scanning.

	int connectAttempts(uint64_t address) const;
	int successfulConnections(uint64_t address) const;
	int liveSessions(uint64_t address) const;
	bool scanning() const;
	bool scanningActive() const;
	int filteredAdvertisements() const;

	// --- BleBackend ---
	bool startScan(const ScanParams &params, AdvertisementFn onAdvertisement) override;
	void stopScan() override;
	bool scanHealthy() override;
	bool bluetoothAvailable() override;
	ConnectResult connect(const DeviceIdentity &identity, CancelToken &cancel, std::shared_ptr<SessionSink> sink,
			      const PhaseFn &onPhase) override;

private:
	class Session;
	friend class Session;
	void sessionClosed(uint64_t address, SessionSink *sink);

	struct State {
		MockDevice device;
		int attempts = 0;
		int successes = 0;
		std::vector<std::shared_ptr<SessionSink>> sinks; // Live sessions.
	};

	mutable std::mutex mutex_;
	std::map<uint64_t, State> devices_;
	bool bluetoothOn_ = true;
	std::chrono::milliseconds connectDelay_{0};

	mutable std::mutex scanMutex_; // Held while delivering advertisements; stopScan waits on it.
	AdvertisementFn scanFn_;
	bool scanning_ = false;
	ScanParams scanParams_;
	int filteredAdvertisements_ = 0;
};

} // namespace blehr
