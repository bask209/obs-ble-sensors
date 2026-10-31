// Abstract BLE backend. The shipping build uses the Windows (C++/WinRT) backend; tests
// and the explicitly enabled development mode use MockBackend.
#pragma once

#include "device_identity.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace blehr {

int64_t monotonicNs();
int64_t unixTimeSeconds();

struct Advertisement {
	DeviceIdentity identity;
	std::string name;
	bool advertisesHeartRate = false;
	int16_t rssi = 0;
};

// Cooperative cancellation shared between a link worker and the backend.
class CancelToken {
public:
	void cancel()
	{
		{
			std::lock_guard<std::mutex> lock(mutex_);
			cancelled_ = true;
		}
		cv_.notify_all();
	}
	bool cancelled() const { return cancelled_.load(); }
	// Sleeps up to `duration`; returns true if cancelled.
	bool waitFor(std::chrono::milliseconds duration)
	{
		std::unique_lock<std::mutex> lock(mutex_);
		return cv_.wait_for(lock, duration, [this] { return cancelled_.load(); });
	}

private:
	std::mutex mutex_;
	std::condition_variable cv_;
	std::atomic<bool> cancelled_{false};
};

// Receives events from one connection. Backend callbacks may arrive on any thread and
// even after the connection was torn down; detach() guarantees that once it returns no
// callback is running and none will run again.
class SessionSink {
public:
	using DataFn = std::function<void(const uint8_t *, size_t)>;
	using LostFn = std::function<void(const std::string &)>;

	SessionSink(DataFn onData, LostFn onLost) : onData_(std::move(onData)), onLost_(std::move(onLost)) {}

	void data(const uint8_t *bytes, size_t size)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (alive_ && onData_)
			onData_(bytes, size);
	}
	void lost(const std::string &reason)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (alive_ && onLost_)
			onLost_(reason);
	}
	void detach()
	{
		std::lock_guard<std::mutex> lock(mutex_);
		alive_ = false;
		onData_ = nullptr;
		onLost_ = nullptr;
	}

private:
	std::mutex mutex_;
	bool alive_ = true;
	DataFn onData_;
	LostFn onLost_;
};

// An established, subscribed connection. Destroying it unsubscribes and releases all
// OS resources.
class BleSession {
public:
	virtual ~BleSession() = default;
};

enum class ConnectStatus { Connected, NotFound, Unsupported, AccessDenied, BluetoothOff, Cancelled, Failed };

enum class ConnectPhase { Connecting, DiscoveringServices, Subscribing };

struct ConnectResult {
	ConnectStatus status = ConnectStatus::Failed;
	std::string detail;
	std::string deviceName;
	std::string windowsDeviceId;
	std::unique_ptr<BleSession> session;
};

class BleBackend {
public:
	using AdvertisementFn = std::function<void(const Advertisement &)>;
	using PhaseFn = std::function<void(ConnectPhase)>;

	virtual ~BleBackend() = default;

	// Called on every manager-owned worker thread before/after use (COM apartment setup).
	virtual void threadBegin() {}
	virtual void threadEnd() {}

	// Non-blocking. (Re)starts the watcher. `params.active` requests scan responses
	// (device names; only while the user has the scan dialog open). When
	// `params.allDevices` is false, advertisements from addresses outside
	// `params.addresses` must be dropped as early and cheaply as possible.
	// The callback may run on any thread until stopScan() returns.
	struct ScanParams {
		bool active = false;
		bool allDevices = false;
		std::vector<uint64_t> addresses; // sorted
		bool operator==(const ScanParams &o) const
		{
			return active == o.active && allDevices == o.allDevices && addresses == o.addresses;
		}
	};
	virtual bool startScan(const ScanParams &params, AdvertisementFn onAdvertisement) = 0;
	virtual void stopScan() = 0;
	virtual bool scanHealthy() = 0; // False if the OS stopped the watcher (e.g. radio off).

	// May block; only called from worker threads.
	virtual bool bluetoothAvailable() = 0;
	virtual ConnectResult connect(const DeviceIdentity &identity, CancelToken &cancel,
				      std::shared_ptr<SessionSink> sink, const PhaseFn &onPhase) = 0;
};

} // namespace blehr
