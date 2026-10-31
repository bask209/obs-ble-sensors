#include "mock_backend.hpp"

#include <algorithm>

namespace blehr {

class MockBackend::Session : public BleSession {
public:
	Session(MockBackend *owner, uint64_t address, SessionSink *sink) : owner_(owner), address_(address), sink_(sink) {}
	~Session() override { owner_->sessionClosed(address_, sink_); }

private:
	MockBackend *owner_;
	uint64_t address_;
	SessionSink *sink_;
};

void MockBackend::addDevice(const MockDevice &device)
{
	std::lock_guard<std::mutex> lock(mutex_);
	devices_[device.identity.address].device = device;
}

void MockBackend::setPresent(uint64_t address, bool present)
{
	std::lock_guard<std::mutex> lock(mutex_);
	devices_[address].device.present = present;
}

void MockBackend::setBluetoothOn(bool on)
{
	std::vector<std::shared_ptr<SessionSink>> dropped;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		bluetoothOn_ = on;
		if (!on)
			for (auto &kv : devices_)
				dropped.insert(dropped.end(), kv.second.sinks.begin(), kv.second.sinks.end());
	}
	for (auto &sink : dropped)
		sink->lost("Bluetooth turned off");
}

void MockBackend::setConnectDelay(std::chrono::milliseconds delay)
{
	std::lock_guard<std::mutex> lock(mutex_);
	connectDelay_ = delay;
}

bool MockBackend::notify(uint64_t address, const std::vector<uint8_t> &payload)
{
	std::vector<std::shared_ptr<SessionSink>> sinks;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		auto it = devices_.find(address);
		if (it == devices_.end())
			return false;
		sinks = it->second.sinks;
	}
	for (auto &sink : sinks)
		sink->data(payload.data(), payload.size());
	return !sinks.empty();
}

bool MockBackend::dropConnection(uint64_t address, const std::string &reason)
{
	std::vector<std::shared_ptr<SessionSink>> sinks;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		auto it = devices_.find(address);
		if (it == devices_.end())
			return false;
		sinks = it->second.sinks;
	}
	for (auto &sink : sinks)
		sink->lost(reason);
	return !sinks.empty();
}

void MockBackend::advertise(uint64_t address)
{
	Advertisement adv;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		auto it = devices_.find(address);
		if (it == devices_.end() || !it->second.device.present || !bluetoothOn_)
			return;
		adv.identity = it->second.device.identity;
		adv.name = it->second.device.name;
		adv.advertisesHeartRate = it->second.device.advertisesHeartRate;
		adv.rssi = -60;
	}
	std::lock_guard<std::mutex> lock(scanMutex_);
	if (scanning_ && scanFn_)
		scanFn_(adv);
}

int MockBackend::connectAttempts(uint64_t address) const
{
	std::lock_guard<std::mutex> lock(mutex_);
	auto it = devices_.find(address);
	return it == devices_.end() ? 0 : it->second.attempts;
}

int MockBackend::successfulConnections(uint64_t address) const
{
	std::lock_guard<std::mutex> lock(mutex_);
	auto it = devices_.find(address);
	return it == devices_.end() ? 0 : it->second.successes;
}

int MockBackend::liveSessions(uint64_t address) const
{
	std::lock_guard<std::mutex> lock(mutex_);
	auto it = devices_.find(address);
	return it == devices_.end() ? 0 : int(it->second.sinks.size());
}

bool MockBackend::scanning() const
{
	std::lock_guard<std::mutex> lock(scanMutex_);
	return scanning_;
}

bool MockBackend::scanningActive() const
{
	std::lock_guard<std::mutex> lock(scanMutex_);
	return scanning_ && scanActive_;
}

bool MockBackend::startScan(bool active, AdvertisementFn onAdvertisement)
{
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (!bluetoothOn_)
			return false;
	}
	std::lock_guard<std::mutex> lock(scanMutex_);
	scanFn_ = std::move(onAdvertisement);
	scanning_ = true;
	scanActive_ = active;
	return true;
}

void MockBackend::stopScan()
{
	std::lock_guard<std::mutex> lock(scanMutex_);
	scanning_ = false;
	scanFn_ = nullptr;
}

bool MockBackend::scanHealthy()
{
	std::lock_guard<std::mutex> lock(mutex_);
	return bluetoothOn_;
}

bool MockBackend::bluetoothAvailable()
{
	std::lock_guard<std::mutex> lock(mutex_);
	return bluetoothOn_;
}

ConnectResult MockBackend::connect(const DeviceIdentity &identity, CancelToken &cancel,
				   std::shared_ptr<SessionSink> sink, const PhaseFn &onPhase)
{
	ConnectResult result;
	std::chrono::milliseconds delay;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		devices_[identity.address].attempts++;
		delay = connectDelay_;
	}
	onPhase(ConnectPhase::Connecting);
	if (delay.count() > 0 && cancel.waitFor(delay)) {
		result.status = ConnectStatus::Cancelled;
		return result;
	}
	if (cancel.cancelled()) {
		result.status = ConnectStatus::Cancelled;
		return result;
	}

	std::lock_guard<std::mutex> lock(mutex_);
	if (!bluetoothOn_) {
		result.status = ConnectStatus::BluetoothOff;
		return result;
	}
	auto it = devices_.find(identity.address);
	if (it == devices_.end() || !it->second.device.present || it->second.device.identity.address == 0) {
		result.status = ConnectStatus::NotFound;
		result.detail = "not in range";
		return result;
	}
	State &st = it->second;
	if (st.device.accessDenied) {
		result.status = ConnectStatus::AccessDenied;
		return result;
	}
	onPhase(ConnectPhase::DiscoveringServices);
	if (!st.device.supportsHeartRate) {
		result.status = ConnectStatus::Unsupported;
		result.detail = "no Heart Rate service (0x180D)";
		return result;
	}
	onPhase(ConnectPhase::Subscribing);
	st.successes++;
	st.sinks.push_back(sink);
	result.status = ConnectStatus::Connected;
	result.deviceName = st.device.name;
	result.windowsDeviceId = "MOCK#" + identity.key();
	result.session = std::make_unique<Session>(this, identity.address, sink.get());
	return result;
}

void MockBackend::sessionClosed(uint64_t address, SessionSink *sink)
{
	std::lock_guard<std::mutex> lock(mutex_);
	auto it = devices_.find(address);
	if (it == devices_.end())
		return;
	auto &sinks = it->second.sinks;
	sinks.erase(std::remove_if(sinks.begin(), sinks.end(),
				   [sink](const std::shared_ptr<SessionSink> &s) { return s.get() == sink; }),
		    sinks.end());
}

} // namespace blehr
