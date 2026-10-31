#include "ble_manager.hpp"

#include <algorithm>

namespace blehr {

int64_t monotonicNs()
{
	return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
		.count();
}

int64_t unixTimeSeconds()
{
	return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
		.count();
}

const char *adapterStateText(AdapterState state)
{
	switch (state) {
	case AdapterState::Ready:
		return "ready";
	case AdapterState::Off:
		return "Bluetooth is turned off in Windows";
	case AdapterState::NoAdapter:
		return "no Bluetooth adapter found";
	case AdapterState::NoLowEnergy:
		return "this PC's Bluetooth adapter does not support Bluetooth LE (4.0+)";
	}
	return "unknown";
}

static const char *connectStatusName(ConnectStatus status)
{
	switch (status) {
	case ConnectStatus::Connected:
		return "connected";
	case ConnectStatus::NotFound:
		return "not found";
	case ConnectStatus::Unsupported:
		return "not a supported heart-rate device";
	case ConnectStatus::AccessDenied:
		return "access denied";
	case ConnectStatus::BluetoothOff:
		return "Bluetooth unavailable";
	case ConnectStatus::Cancelled:
		return "cancelled";
	case ConnectStatus::Failed:
		return "failed";
	}
	return "unknown";
}

// The link's state machine. Every change also publishes the packed RenderWord with one
// atomic store, so the per-frame query never takes a lock. Callers hold Link::m.
class LinkMachine : public SensorLinkMachine {
public:
	std::atomic<uint64_t> word{0};

	void searching(const std::string &detail = {}) { SensorLinkMachine::searching(detail); publish(); }
	void connecting() { SensorLinkMachine::connecting(); publish(); }
	void discoveringServices() { SensorLinkMachine::discoveringServices(); publish(); }
	void subscribing() { SensorLinkMachine::subscribing(); publish(); }
	void subscribed(int64_t nowNs) { SensorLinkMachine::subscribed(nowNs); publish(); }
	void measurement(const HeartRateMeasurement &m, int64_t nowNs) { SensorLinkMachine::measurement(m, nowNs); publish(); }
	void disconnected(const std::string &detail = {}) { SensorLinkMachine::disconnected(detail); publish(); }
	void unsupported(const std::string &detail) { SensorLinkMachine::unsupported(detail); publish(); }
	void bluetoothOff(const std::string &detail = {}) { SensorLinkMachine::bluetoothOff(detail); publish(); }
	void error(const std::string &detail) { SensorLinkMachine::error(detail); publish(); }
	void stopped() { SensorLinkMachine::stopped(); publish(); }

private:
	void publish() { word.store(RenderWord::pack(snapshot()), std::memory_order_release); }
};

// Lock order: BleManager::mutex_ -> Link::m -> nothing.
// SessionSink's internal mutex -> Link::m (from backend callbacks); so the worker never
// holds Link::m while calling SessionSink::detach().
struct BleManager::Link {
	DeviceIdentity identity;
	std::string key;

	mutable std::mutex m;
	std::condition_variable cv;
	LinkMachine machine;
	int subscribers = 0;
	int autoVotes = 0;
	bool stop = false;
	bool wake = false;
	bool reconnectRequested = false;
	bool lost = false;
	bool waiting = false; // Waiting between attempts (not connected).
	int64_t lastAdvertWakeNs = 0;
	std::string lastLog; // Worker thread only: suppresses repeated identical log lines.

	CancelToken cancel;
	std::thread thread;
	std::atomic<bool> finished{false};
};

struct BleManager::ScanSlot {
	std::mutex m;
	bool alive = true;
	ScanFn fn;

	void deliver(const Advertisement &adv)
	{
		std::lock_guard<std::mutex> lock(m);
		if (alive && fn)
			fn(adv);
	}
	void kill()
	{
		std::lock_guard<std::mutex> lock(m);
		alive = false;
		fn = nullptr;
	}
};

// ---------------------------------------------------------------------------------------
// Subscription / ScanToken

BleManager::Subscription::Subscription(std::weak_ptr<BleManager> mgr, std::shared_ptr<Link> link, bool autoReconnect)
	: manager_(std::move(mgr)),
	  link_(std::move(link)),
	  key_(link_->key),
	  autoReconnect_(autoReconnect)
{
}

BleManager::Subscription::~Subscription()
{
	if (auto mgr = manager_.lock())
		mgr->release(link_, autoReconnect_);
}

SensorSnapshot BleManager::Subscription::snapshot() const
{
	std::lock_guard<std::mutex> lock(link_->m);
	return link_->machine.snapshot();
}

bool BleManager::Subscription::liveBpm(int64_t nowNs, int64_t timeoutNs, uint16_t &bpm, uint64_t &generation) const
{
	// Wait-free: a single atomic load, safe to call every frame from the render thread.
	return RenderWord::evaluate(link_->machine.word.load(std::memory_order_acquire), nowNs, timeoutNs, bpm,
				    generation);
}

void BleManager::Subscription::setAutoReconnect(bool enabled)
{
	if (enabled == autoReconnect_)
		return;
	if (auto mgr = manager_.lock())
		mgr->setAutoReconnectVote(link_, autoReconnect_, enabled);
	autoReconnect_ = enabled;
}

void BleManager::Subscription::reconnectNow()
{
	{
		std::lock_guard<std::mutex> lock(link_->m);
		link_->reconnectRequested = true;
	}
	link_->cv.notify_all();
}

BleManager::ScanToken::~ScanToken()
{
	slot_->kill();
	if (auto mgr = manager_.lock())
		mgr->removeScanSlot(slot_);
}

// ---------------------------------------------------------------------------------------
// BleManager

std::shared_ptr<BleManager> BleManager::create(std::unique_ptr<BleBackend> backend, const ManagerConfig &config)
{
	return std::shared_ptr<BleManager>(new BleManager(std::move(backend), config));
}

BleManager::BleManager(std::unique_ptr<BleBackend> backend, const ManagerConfig &config)
	: backend_(std::move(backend)),
	  config_(config)
{
}

BleManager::~BleManager()
{
	shutdown();
}

void BleManager::shutdown()
{
	std::vector<std::shared_ptr<Link>> all;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		stopping_ = true;
		for (auto &kv : links_)
			all.push_back(kv.second);
		all.insert(all.end(), zombies_.begin(), zombies_.end());
		links_.clear();
		zombies_.clear();
		for (auto &slot : scanSlots_)
			slot->kill();
		scanSlots_.clear();
	}
	{
		std::lock_guard<std::mutex> lock(scanMutex_);
		if (scanning_)
			backend_->stopScan();
		scanning_ = false;
	}
	for (auto &link : all) {
		{
			std::lock_guard<std::mutex> lock(link->m);
			link->stop = true;
		}
		link->cancel.cancel();
		link->cv.notify_all();
	}
	for (auto &link : all)
		if (link->thread.joinable())
			link->thread.join();
	std::vector<Helper> helpers;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		helpers.swap(helpers_);
	}
	for (auto &h : helpers)
		if (h.thread.joinable())
			h.thread.join();
}

std::unique_ptr<BleManager::Subscription> BleManager::subscribe(const DeviceIdentity &identity, bool autoReconnect)
{
	std::shared_ptr<Link> link;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (stopping_)
			return nullptr;
		reapFinishedLocked();

		const std::string key = identity.key();
		auto &slot = links_[key];
		if (!slot) {
			slot = std::make_shared<Link>();
			slot->identity = identity;
			slot->key = key;
			slot->machine.searching();
			slot->thread = std::thread(&BleManager::runLink, this, slot);
		}
		link = slot;
		std::lock_guard<std::mutex> linkLock(link->m);
		link->subscribers++;
		if (autoReconnect)
			link->autoVotes++;
	}
	link->cv.notify_all();
	return std::unique_ptr<Subscription>(new Subscription(weak_from_this(), link, autoReconnect));
}

void BleManager::release(const std::shared_ptr<Link> &link, bool autoReconnect)
{
	{
		std::lock_guard<std::mutex> lock(mutex_);
		bool last = false;
		{
			std::lock_guard<std::mutex> linkLock(link->m);
			link->subscribers--;
			if (autoReconnect)
				link->autoVotes--;
			if (link->subscribers <= 0) {
				link->stop = true;
				last = true;
			}
		}
		if (last) {
			link->cancel.cancel();
			auto it = links_.find(link->key);
			if (it != links_.end() && it->second == link)
				links_.erase(it);
			zombies_.push_back(link);
		}
		link->cv.notify_all();
		reapFinishedLocked();
	}
	updateScanDemand();
}

void BleManager::setAutoReconnectVote(const std::shared_ptr<Link> &link, bool from, bool to)
{
	{
		std::lock_guard<std::mutex> lock(link->m);
		if (from)
			link->autoVotes--;
		if (to)
			link->autoVotes++;
	}
	link->cv.notify_all();
}

void BleManager::reapFinishedLocked()
{
	// Joining here never blocks for long: `finished` is set as the very last action of the
	// worker function.
	auto it = std::remove_if(zombies_.begin(), zombies_.end(), [](const std::shared_ptr<Link> &l) {
		if (!l->finished.load())
			return false;
		if (l->thread.joinable())
			l->thread.join();
		return true;
	});
	zombies_.erase(it, zombies_.end());
}

std::optional<SensorSnapshot> BleManager::peek(const std::string &key) const
{
	std::lock_guard<std::mutex> lock(mutex_);
	auto it = links_.find(key);
	if (it == links_.end())
		return std::nullopt;
	std::lock_guard<std::mutex> linkLock(it->second->m);
	return it->second->machine.snapshot();
}

std::unique_ptr<BleManager::ScanToken> BleManager::addScanListener(ScanFn fn)
{
	auto slot = std::make_shared<ScanSlot>();
	slot->fn = std::move(fn);
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (stopping_)
			slot->alive = false;
		else
			scanSlots_.push_back(slot);
	}
	updateScanDemand();
	return std::unique_ptr<ScanToken>(new ScanToken(weak_from_this(), slot));
}

void BleManager::removeScanSlot(const std::shared_ptr<ScanSlot> &slot)
{
	{
		std::lock_guard<std::mutex> lock(mutex_);
		scanSlots_.erase(std::remove(scanSlots_.begin(), scanSlots_.end(), slot), scanSlots_.end());
	}
	updateScanDemand();
}

void BleManager::setConnectedCallback(ConnectedFn fn)
{
	std::lock_guard<std::mutex> lock(mutex_);
	connected_ = std::move(fn);
}

void BleManager::setEventLog(EventLogFn fn)
{
	std::lock_guard<std::mutex> lock(mutex_);
	eventLog_ = std::move(fn);
}

void BleManager::logEvent(Link &link, const std::string &message)
{
	if (message == link.lastLog)
		return;
	link.lastLog = message;
	EventLogFn fn;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		fn = eventLog_;
	}
	if (fn)
		fn(link.key, message);
}

void BleManager::requestPairedDevices()
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (stopping_)
		return;
	// Reap finished queries (non-blocking: `done` is set as the last action).
	for (auto it = helpers_.begin(); it != helpers_.end();) {
		if (it->done->load()) {
			it->thread.join();
			it = helpers_.erase(it);
		} else {
			++it;
		}
	}
	if (!helpers_.empty())
		return; // One query at a time.
	auto done = std::make_shared<std::atomic<bool>>(false);
	Helper helper;
	helper.done = done;
	helper.thread = std::thread([this, done] {
		struct Done {
			std::shared_ptr<std::atomic<bool>> flag;
			~Done() { flag->store(true); }
		} markDone{done};
		backend_->threadBegin();
		std::vector<Advertisement> devices = backend_->pairedDevices();
		backend_->threadEnd();
		std::vector<std::shared_ptr<ScanSlot>> slots;
		{
			std::lock_guard<std::mutex> l(mutex_);
			if (stopping_)
				return;
			slots = scanSlots_;
		}
		for (auto &adv : devices) {
			adv.paired = true;
			for (auto &slot : slots)
				slot->deliver(adv);
		}
	});
	helpers_.push_back(std::move(helper));
}

size_t BleManager::activeLinkCount() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return links_.size();
}

bool BleManager::scanWanted() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (stopping_)
		return false;
	if (!scanSlots_.empty())
		return true;
	for (const auto &kv : links_) {
		std::lock_guard<std::mutex> linkLock(kv.second->m);
		if (!config_.backgroundScan)
			break;
		if (kv.second->waiting && kv.second->machine.snapshot().state != SensorState::Unsupported)
			return true;
	}
	return false;
}

void BleManager::updateScanDemand()
{
	// Hold scanMutex_ across the decision so a late call from a worker can never restart
	// scanning after shutdown() stopped it. Lock order: scanMutex_ -> mutex_.
	std::lock_guard<std::mutex> scanLock(scanMutex_);
	BleBackend::ScanParams params;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		// Dialog open: active scan of everything (names, discovery). Otherwise a passive
		// scan that only cares about sensors waiting to reconnect; every other device's
		// advertisement is discarded by the backend before any parsing.
		params.active = params.allDevices = !scanSlots_.empty() && !stopping_;
		for (const auto &kv : links_)
			params.addresses.push_back(kv.second->identity.address);
	}
	std::sort(params.addresses.begin(), params.addresses.end());
	const bool want = scanWanted();

	if (want) {
		if (!scanning_ || !(scanParams_ == params) || !backend_->scanHealthy()) {
			scanning_ = backend_->startScan(params, [this](const Advertisement &adv) { onAdvertisement(adv); });
			scanParams_ = params;
		}
	} else if (scanning_) {
		backend_->stopScan();
		scanning_ = false;
	}
}

void BleManager::onAdvertisement(const Advertisement &adv)
{
	std::vector<std::shared_ptr<ScanSlot>> slots;
	std::vector<std::shared_ptr<Link>> matches;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (stopping_)
			return;
		slots = scanSlots_;
		for (auto &kv : links_)
			if (kv.second->identity.sameDevice(adv.identity))
				matches.push_back(kv.second);
	}

	const int64_t now = monotonicNs();
	const int64_t minInterval = std::chrono::duration_cast<std::chrono::nanoseconds>(config_.advertWakeMinInterval).count();
	for (auto &link : matches) {
		bool notify = false;
		{
			std::lock_guard<std::mutex> lock(link->m);
			const SensorState st = link->machine.snapshot().state;
			const bool reconnectable = st == SensorState::Searching || st == SensorState::Disconnected ||
						   st == SensorState::Error || st == SensorState::BluetoothOff;
			if (link->waiting && reconnectable && link->autoVotes > 0 &&
			    now - link->lastAdvertWakeNs >= minInterval) {
				link->lastAdvertWakeNs = now;
				link->wake = true;
				notify = true;
			}
		}
		if (notify)
			link->cv.notify_all();
	}

	for (auto &slot : slots)
		slot->deliver(adv);
}

void BleManager::runLink(std::shared_ptr<Link> link)
{
	backend_->threadBegin();

	Backoff backoff(config_.backoffInitial, config_.backoffMax, 0.2,
			config_.backoffSeed ? config_.backoffSeed : std::random_device{}());
	int64_t absentSinceNs = 0; // When the device was first not found (0 = present).
	Backoff silentBackoff(config_.silentBackoffInitial, config_.silentBackoffMax, 0.2,
			      config_.backoffSeed ? config_.backoffSeed + 1 : std::random_device{}());
	const int64_t silenceNs = std::chrono::duration_cast<std::chrono::nanoseconds>(config_.silenceReconnect).count();

	for (;;) {
		{
			std::lock_guard<std::mutex> lock(link->m);
			if (link->stop)
				break;
			link->lost = false;
			link->reconnectRequested = false;
			link->wake = false;
			link->machine.connecting();
		}

		std::chrono::milliseconds delay{0};
		bool parked = false;

		const AdapterState adapter = backend_->adapterState();
		if (adapter != AdapterState::Ready) {
			{
				std::lock_guard<std::mutex> lock(link->m);
				link->machine.bluetoothOff(adapterStateText(adapter));
			}
			logEvent(*link, std::string("Bluetooth not ready: ") + adapterStateText(adapter));
			delay = backoff.next();
		} else {
			std::weak_ptr<Link> weak = link;
			auto sink = std::make_shared<SessionSink>(
				[weak](MeasurementKind kind, const uint8_t *data, size_t size) {
					auto l = weak.lock();
					if (!l)
						return;
					const HeartRateMeasurement m = parseMeasurement(kind, data, size);
					std::lock_guard<std::mutex> lock(l->m);
					if (!l->lost)
						l->machine.measurement(m, monotonicNs());
				},
				[weak](const std::string &reason) {
					auto l = weak.lock();
					if (!l)
						return;
					{
						std::lock_guard<std::mutex> lock(l->m);
						if (l->lost)
							return;
						// Invalidate immediately so the renderer hides on the next frame.
						l->lost = true;
						l->machine.disconnected(reason);
					}
					l->cv.notify_all();
				});

			auto onPhase = [&link](ConnectPhase phase) {
				std::lock_guard<std::mutex> lock(link->m);
				if (link->stop)
					return;
				switch (phase) {
				case ConnectPhase::Connecting:
					link->machine.connecting();
					break;
				case ConnectPhase::DiscoveringServices:
					link->machine.discoveringServices();
					break;
				case ConnectPhase::Subscribing:
					link->machine.subscribing();
					break;
				}
			};

			ConnectResult result = backend_->connect(link->identity, link->cancel, sink, onPhase);

			if (result.status == ConnectStatus::Connected && result.session) {
				absentSinceNs = 0;
				{
					std::lock_guard<std::mutex> lock(link->m);
					if (!link->lost && !link->stop)
						link->machine.subscribed(monotonicNs());
				}
				ConnectedFn connected;
				{
					std::lock_guard<std::mutex> lock(mutex_);
					connected = connected_;
				}
				if (connected)
					connected(link->key, result.deviceName, result.windowsDeviceId);
				logEvent(*link, std::string("connected to ") + result.deviceName + " (" + measurementKindName(result.kind) + ")");
				updateScanDemand();

				std::string reason;
				uint64_t validAtStart = 0;
				uint64_t validAtEnd = 0;
				{
					std::unique_lock<std::mutex> lock(link->m);
					validAtStart = link->machine.snapshot().validPackets;
					// Event driven: sleeps until a disconnect/stop is signalled or the silence
					// deadline passes (no periodic wake-ups while data flows).
					while (!link->stop && !link->lost) {
						const int64_t deadlineNs = link->machine.snapshot().lastPacketNs + silenceNs;
						if (monotonicNs() >= deadlineNs) {
							reason = "no data received";
							break;
						}
						link->cv.wait_until(lock, std::chrono::steady_clock::time_point(
										  std::chrono::nanoseconds(deadlineNs)));
					}
					validAtEnd = link->machine.snapshot().validPackets;
				}
				// Only a session that delivered confirmed heart-rate data resets the backoff. A
				// device that connects but stays silent (e.g. a watch with broadcasting turned
				// off) is retried progressively less often, up to every 2 minutes, to spare its
				// battery and the radio.
				const bool deliveredData = validAtEnd != validAtStart;
				if (deliveredData) {
					backoff.reset();
					silentBackoff.reset();
				}

				sink->detach();          // No callbacks after this point...
				result.session.reset(); // ...so tearing down the OS objects is safe.

				std::string why;
				bool stopping = false;
				{
					std::lock_guard<std::mutex> lock(link->m);
					if (!link->lost)
						link->machine.disconnected(reason);
					why = link->machine.snapshot().detail;
					stopping = link->stop; // Read under the lock (found by ThreadSanitizer).
				}
				if (!stopping)
					logEvent(*link, "disconnected" + (why.empty() ? std::string() : " (" + why + ")"));
				delay = deliveredData ? backoff.next() : silentBackoff.next();
			} else {
				sink->detach();
				result.session.reset();
				if (result.status == ConnectStatus::NotFound && link->identity.isResolvablePrivate())
					result.detail = "this device changes its Bluetooth address for privacy; pair it in Windows "
							"Bluetooth settings, then select it again, so it can always be found";
				if (result.status != ConnectStatus::Cancelled)
					logEvent(*link, std::string(connectStatusName(result.status)) +
								(result.detail.empty() ? "" : ": " + result.detail));
				std::lock_guard<std::mutex> lock(link->m);
				switch (result.status) {
				case ConnectStatus::NotFound: {
					link->machine.searching(result.detail);
					delay = backoff.next();
					if (!absentSinceNs)
						absentSinceNs = monotonicNs();
					const auto absent = std::chrono::nanoseconds(monotonicNs() - absentSinceNs);
					if (absent >= config_.longAbsenceAfter && delay < config_.longAbsenceRetry)
						delay = config_.longAbsenceRetry;
					break;
				}
				case ConnectStatus::Unsupported:
					// The device answered and has no heart-rate service: leave it alone.
					// No further connections until the user asks (Reconnect now / reselect).
					link->machine.unsupported(result.detail);
					parked = true;
					break;
				case ConnectStatus::AccessDenied:
					link->machine.error(result.detail.empty()
								    ? "Windows denied access; pair the sensor in Windows Bluetooth settings"
								    : result.detail);
					delay = backoff.next();
					break;
				case ConnectStatus::BluetoothOff:
					link->machine.bluetoothOff();
					delay = backoff.next();
					break;
				case ConnectStatus::Cancelled:
				case ConnectStatus::Failed:
				case ConnectStatus::Connected: // Connected without a session: treat as failure.
					link->machine.error(result.detail);
					delay = backoff.next();
					break;
				}
			}
		}

		{
			std::lock_guard<std::mutex> lock(link->m);
			if (link->stop)
				break;
			link->waiting = true;
			link->wake = false;
		}
		updateScanDemand();

		{
			std::unique_lock<std::mutex> lock(link->m);
			const auto deadline = std::chrono::steady_clock::now() + delay;
			while (!link->stop && !link->reconnectRequested && !link->wake) {
				if (link->autoVotes > 0 && !parked) {
					if (link->cv.wait_until(lock, deadline) == std::cv_status::timeout)
						break;
				} else {
					// Automatic reconnect disabled by every source, or the device is not a
					// heart-rate sensor: wait for an explicit request.
					link->cv.wait(lock);
				}
			}
			parked = false;
			link->waiting = false;
			if (link->reconnectRequested || link->wake)
				backoff.reset();
		}
	}

	{
		std::lock_guard<std::mutex> lock(link->m);
		link->waiting = false;
		link->machine.stopped();
	}
	bool managerStopping = false;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		managerStopping = stopping_;
	}
	if (!managerStopping)
		logEvent(*link, "connection closed (no visible source uses this sensor)");
	backend_->threadEnd();
	link->finished.store(true);
}

} // namespace blehr
