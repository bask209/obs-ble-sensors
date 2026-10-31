// BleManager: owns one managed connection ("link") per physical sensor, shared by every
// source that selects it. Each link runs a small worker thread that connects,
// subscribes, watches for silence and reconnects with backoff. Nothing here blocks the
// caller: subscribing/unsubscribing only signals worker threads.
#pragma once

#include "backoff.hpp"
#include "ble_backend.hpp"
#include "sensor_state.hpp"

#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace blehr {

struct ManagerConfig {
	std::chrono::milliseconds backoffInitial{1000};
	std::chrono::milliseconds backoffMax{15000};
	// For devices that connect but send no heart rate (e.g. watch broadcast turned off).
	std::chrono::milliseconds silentBackoffInitial{15000};
	std::chrono::milliseconds silentBackoffMax{120000};
	// A device missing for longer than this is retried at most every longAbsenceRetry
	// (halves radio work for a sensor that is simply put away).
	std::chrono::milliseconds longAbsenceAfter{600000};
	std::chrono::milliseconds longAbsenceRetry{30000};
	// Reconnect when a connection delivers no notification at all for this long.
	std::chrono::milliseconds silenceReconnect{20000};
	// Minimum spacing between advertisement-triggered early reconnect attempts.
	std::chrono::milliseconds advertWakeMinInterval{2000};
	uint32_t backoffSeed = 0; // 0 = random
	// Passive scan while a sensor is missing, to reconnect as soon as it advertises.
	// Windows delivers every nearby advertisement to the process, so this is opt-in.
	bool backgroundScan = false; // Off by default: costs ~10x more CPU than periodic retries.

};

class BleManager : public std::enable_shared_from_this<BleManager> {
	struct Link;
	struct ScanSlot;

public:
	using ConnectedFn = std::function<void(const std::string &key, const std::string &name, const std::string &winId)>;
	using ScanFn = std::function<void(const Advertisement &)>;

	// RAII handle held by a source. Destroying it releases the shared link.
	class Subscription {
	public:
		~Subscription();
		Subscription(const Subscription &) = delete;
		Subscription &operator=(const Subscription &) = delete;

		const std::string &key() const { return key_; }
		SensorSnapshot snapshot() const;
		// Wait-free per-frame query (one atomic load): true and the BPM only when the
		// visibility rule holds. Never blocks, whatever the Bluetooth threads are doing.
		bool liveBpm(int64_t nowNs, int64_t timeoutNs, uint16_t &bpm, uint64_t &generation) const;
		void setAutoReconnect(bool enabled);
		void reconnectNow();

	private:
		friend class BleManager;
		Subscription(std::weak_ptr<BleManager> mgr, std::shared_ptr<Link> link, bool autoReconnect);
		std::weak_ptr<BleManager> manager_;
		std::shared_ptr<Link> link_;
		std::string key_;
		bool autoReconnect_;
	};

	// Opaque scan-listener token; destroying it unregisters (waits for in-flight callback).
	class ScanToken {
	public:
		~ScanToken();
		ScanToken(const ScanToken &) = delete;
		ScanToken &operator=(const ScanToken &) = delete;

	private:
		friend class BleManager;
		ScanToken(std::weak_ptr<BleManager> mgr, std::shared_ptr<ScanSlot> slot) : manager_(std::move(mgr)), slot_(std::move(slot)) {}
		std::weak_ptr<BleManager> manager_;
		std::shared_ptr<ScanSlot> slot_;
	};

	static std::shared_ptr<BleManager> create(std::unique_ptr<BleBackend> backend, ManagerConfig config = {});
	~BleManager();

	// Stops every worker and joins them. Safe to call more than once.
	void shutdown();

	// Returns nullptr after shutdown.
	std::unique_ptr<Subscription> subscribe(const DeviceIdentity &identity, bool autoReconnect);

	// Current snapshot for a device that some source is using, if any.
	std::optional<SensorSnapshot> peek(const std::string &key) const;

	std::unique_ptr<ScanToken> addScanListener(ScanFn fn);

	void setConnectedCallback(ConnectedFn fn);
	// Receives short, human-readable connection events (deduplicated per sensor).
	// Called from worker threads until shutdown() returns, so everything it touches must
	// outlive the manager.
	using EventLogFn = std::function<void(const std::string &key, const std::string &message)>;
	void setEventLog(EventLogFn fn);
	// Asynchronously reports the devices paired with Windows to the scan listeners
	// (Advertisement::paired = true). Non-blocking.
	void requestPairedDevices();

	// Diagnostics / tests.
	size_t activeLinkCount() const;
	bool scanWanted() const;

private:
	BleManager(std::unique_ptr<BleBackend> backend, ManagerConfig config);

	void release(const std::shared_ptr<Link> &link, bool autoReconnect);
	void setAutoReconnectVote(const std::shared_ptr<Link> &link, bool from, bool to);
	void removeScanSlot(const std::shared_ptr<ScanSlot> &slot);
	void runLink(std::shared_ptr<Link> link);
	void onAdvertisement(const Advertisement &adv);
	void updateScanDemand();
	void reapFinishedLocked();
	void logEvent(Link &link, const std::string &message);

	std::unique_ptr<BleBackend> backend_;
	ManagerConfig config_;

	mutable std::mutex mutex_; // Guards links_, zombies_, scanSlots_, stopping_, connected_.
	std::map<std::string, std::shared_ptr<Link>> links_;
	std::vector<std::shared_ptr<Link>> zombies_; // Stopping links awaiting join.
	std::vector<std::shared_ptr<ScanSlot>> scanSlots_;
	ConnectedFn connected_;
	EventLogFn eventLog_;
	struct Helper {
		std::thread thread;
		std::shared_ptr<std::atomic<bool>> done;
	};
	std::vector<Helper> helpers_; // Paired-device queries; reaped when done, joined at shutdown.
	bool stopping_ = false;

	std::mutex scanMutex_; // Serializes backend scan start/stop. Order: scanMutex_ -> mutex_.
	bool scanning_ = false;
	BleBackend::ScanParams scanParams_;
};

} // namespace blehr
