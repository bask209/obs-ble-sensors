#include "winrt_backend.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <combaseapi.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Devices.Radios.h>
#include <winrt/Windows.Storage.Streams.h>

#include <chrono>
#include <mutex>
#include <vector>

namespace blehr {

namespace {

using namespace std::chrono_literals;
using winrt::Windows::Foundation::AsyncStatus;
using winrt::Windows::Foundation::IAsyncInfo;
namespace wdb = winrt::Windows::Devices::Bluetooth;
namespace adv = winrt::Windows::Devices::Bluetooth::Advertisement;
namespace gatt = winrt::Windows::Devices::Bluetooth::GenericAttributeProfile;
namespace radios = winrt::Windows::Devices::Radios;
namespace streams = winrt::Windows::Storage::Streams;

enum class WaitResult { Completed, Failed, Cancelled, TimedOut };

// Waits for a WinRT async operation without blocking forever, so that link cancellation
// (source removed, device switched, OBS shutting down) takes effect within ~50 ms, and
// cancels the OS operation on timeout or cancellation.
//
// The Completed handler may be assigned only once per operation (C++/WinRT's wait_for()
// assigns it on every call, so it cannot be used in a polling loop). The handler owns a
// shared reference to the event, so a late completion after we gave up stays safe.
template<typename Async> WaitResult waitFor(const Async &op, CancelToken *cancel, std::chrono::milliseconds timeout)
{
	auto event = std::make_shared<winrt::handle>(CreateEventW(nullptr, TRUE, FALSE, nullptr));
	if (!*event)
		return WaitResult::Failed;
	op.Completed([event](const auto &, AsyncStatus) { SetEvent(event->get()); });

	const auto deadline = std::chrono::steady_clock::now() + timeout;
	while (WaitForSingleObject(event->get(), 50) == WAIT_TIMEOUT) {
		const bool cancelled = cancel && cancel->cancelled();
		if (cancelled || std::chrono::steady_clock::now() >= deadline) {
			try {
				op.Cancel();
			} catch (...) {
			}
			return cancelled ? WaitResult::Cancelled : WaitResult::TimedOut;
		}
	}
	return op.Status() == AsyncStatus::Completed ? WaitResult::Completed : WaitResult::Failed;
}

std::string narrow(const winrt::hstring &s)
{
	return winrt::to_string(s);
}

class ScanGate {
public:
	explicit ScanGate(BleBackend::AdvertisementFn fn) : fn_(std::move(fn)) {}
	void deliver(const Advertisement &a)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (fn_)
			fn_(a);
	}
	void kill()
	{
		std::lock_guard<std::mutex> lock(mutex_);
		fn_ = nullptr;
	}
	std::atomic<bool> aborted{false};

private:
	std::mutex mutex_;
	BleBackend::AdvertisementFn fn_;
};

// One connected + subscribed heart-rate characteristic. Destruction revokes the event
// handlers first (the SessionSink was already detached by the manager), then releases
// the OS objects so Windows can drop the connection.
class WinRtSession : public BleSession {
public:
	~WinRtSession() override
	{
		try {
			if (characteristic && valueToken.value)
				characteristic.ValueChanged(valueToken);
		} catch (...) {
		}
		try {
			if (device && statusToken.value)
				device.ConnectionStatusChanged(statusToken);
		} catch (...) {
		}
		try {
			if (session) {
				session.MaintainConnection(false);
				session.Close();
			}
		} catch (...) {
		}
		try {
			if (service)
				service.Close();
		} catch (...) {
		}
		try {
			if (device)
				device.Close();
		} catch (...) {
		}
	}

	wdb::BluetoothLEDevice device{nullptr};
	gatt::GattSession session{nullptr};
	gatt::GattDeviceService service{nullptr};
	gatt::GattCharacteristic characteristic{nullptr};
	winrt::event_token statusToken{};
	winrt::event_token valueToken{};
};

class WinRtBackend : public BleBackend {
public:
	WinRtBackend()
	{
		// Keep the process MTA alive so WinRT calls made from threads that never
		// initialized COM (e.g. OBS's video thread releasing a source) are valid.
		if (FAILED(CoIncrementMTAUsage(&mtaCookie_)))
			mtaCookie_ = nullptr;
	}

	~WinRtBackend() override
	{
		stopScan();
		if (mtaCookie_)
			CoDecrementMTAUsage(mtaCookie_);
	}

	void threadBegin() override
	{
		const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		threadInitialized() = SUCCEEDED(hr);
	}

	void threadEnd() override
	{
		if (threadInitialized())
			CoUninitialize();
		threadInitialized() = false;
	}

	bool startScan(bool active, AdvertisementFn onAdvertisement) override
	{
		std::lock_guard<std::mutex> lock(scanMutex_);
		stopScanLocked();
		try {
			auto gate = std::make_shared<ScanGate>(std::move(onAdvertisement));
			adv::BluetoothLEAdvertisementWatcher watcher;
			watcher.ScanningMode(active ? adv::BluetoothLEScanningMode::Active
						    : adv::BluetoothLEScanningMode::Passive);

			receivedToken_ = watcher.Received([gate](const adv::BluetoothLEAdvertisementWatcher &,
								 const adv::BluetoothLEAdvertisementReceivedEventArgs &args) {
				try {
					Advertisement a;
					a.identity.address = args.BluetoothAddress();
					a.identity.addressType = args.BluetoothAddressType() == wdb::BluetoothAddressType::Random
									 ? AddressType::Random
									 : AddressType::Public;
					const auto advertisement = args.Advertisement();
					a.name = narrow(advertisement.LocalName());
					for (const auto &uuid : advertisement.ServiceUuids())
						if (uuid == gatt::GattServiceUuids::HeartRate())
							a.advertisesHeartRate = true;
					a.rssi = args.RawSignalStrengthInDBm();
					gate->deliver(a);
				} catch (...) {
				}
			});
			stoppedToken_ = watcher.Stopped([gate](const adv::BluetoothLEAdvertisementWatcher &,
							       const adv::BluetoothLEAdvertisementWatcherStoppedEventArgs &) {
				gate->aborted = true;
			});
			watcher.Start();
			watcher_ = watcher;
			gate_ = gate;
			return watcher.Status() != adv::BluetoothLEAdvertisementWatcherStatus::Aborted;
		} catch (...) {
			stopScanLocked();
			return false;
		}
	}

	void stopScan() override
	{
		std::lock_guard<std::mutex> lock(scanMutex_);
		stopScanLocked();
	}

	bool scanHealthy() override
	{
		std::lock_guard<std::mutex> lock(scanMutex_);
		if (!watcher_ || !gate_ || gate_->aborted)
			return false;
		try {
			const auto status = watcher_.Status();
			return status == adv::BluetoothLEAdvertisementWatcherStatus::Started ||
			       status == adv::BluetoothLEAdvertisementWatcherStatus::Created;
		} catch (...) {
			return false;
		}
	}

	bool bluetoothAvailable() override
	{
		try {
			auto adapterOp = wdb::BluetoothAdapter::GetDefaultAsync();
			if (waitFor(adapterOp, nullptr, 5000ms) != WaitResult::Completed)
				return false;
			const auto adapter = adapterOp.GetResults();
			if (!adapter || !adapter.IsLowEnergySupported())
				return false;
			auto radioOp = adapter.GetRadioAsync();
			if (waitFor(radioOp, nullptr, 5000ms) != WaitResult::Completed)
				return true; // Unknown radio state: let the connection attempt decide.
			const auto radio = radioOp.GetResults();
			return !radio || radio.State() == radios::RadioState::On;
		} catch (...) {
			return true;
		}
	}

	ConnectResult connect(const DeviceIdentity &identity, CancelToken &cancel, std::shared_ptr<SessionSink> sink,
			      const PhaseFn &onPhase) override
	{
		ConnectResult r;
		auto fail = [&r](ConnectStatus status, std::string detail) -> ConnectResult {
			r.status = status;
			r.detail = std::move(detail);
			r.session.reset();
			return std::move(r);
		};
		auto waitFail = [&](WaitResult w, const char *what) -> ConnectResult {
			if (w == WaitResult::Cancelled)
				return fail(ConnectStatus::Cancelled, {});
			if (w == WaitResult::TimedOut)
				return fail(ConnectStatus::NotFound, std::string(what) + " timed out");
			return fail(ConnectStatus::Failed, std::string(what) + " failed");
		};

		try {
			onPhase(ConnectPhase::Connecting);
			auto session = std::make_unique<WinRtSession>();

			// 1. Resolve the device by its identity address (works for unpaired sensors);
			//    fall back to the OS device id recorded on a previous connection.
			auto devOp = wdb::BluetoothLEDevice::FromBluetoothAddressAsync(
				identity.address, identity.addressType == AddressType::Random
							  ? wdb::BluetoothAddressType::Random
							  : wdb::BluetoothAddressType::Public);
			WaitResult w = waitFor(devOp, &cancel, 15000ms);
			if (w == WaitResult::Cancelled)
				return fail(ConnectStatus::Cancelled, {});
			if (w == WaitResult::Completed)
				session->device = devOp.GetResults();
			if (!session->device && !identity.windowsDeviceId.empty()) {
				auto idOp = wdb::BluetoothLEDevice::FromIdAsync(winrt::to_hstring(identity.windowsDeviceId));
				w = waitFor(idOp, &cancel, 15000ms);
				if (w == WaitResult::Cancelled)
					return fail(ConnectStatus::Cancelled, {});
				if (w == WaitResult::Completed)
					session->device = idOp.GetResults();
			}
			if (!session->device)
				return fail(ConnectStatus::NotFound, "not in range");

			r.deviceName = narrow(session->device.Name());
			r.windowsDeviceId = narrow(session->device.DeviceId());

			// Ask Windows to keep the link up and re-establish it while we hold the session.
			try {
				auto gsOp = gatt::GattSession::FromDeviceIdAsync(session->device.BluetoothDeviceId());
				if (waitFor(gsOp, &cancel, 5000ms) == WaitResult::Completed) {
					session->session = gsOp.GetResults();
					if (session->session)
						session->session.MaintainConnection(true);
				}
			} catch (...) {
			}
			if (cancel.cancelled())
				return fail(ConnectStatus::Cancelled, {});

			session->statusToken = session->device.ConnectionStatusChanged(
				[sink](const wdb::BluetoothLEDevice &device, const winrt::Windows::Foundation::IInspectable &) {
					try {
						if (device.ConnectionStatus() == wdb::BluetoothConnectionStatus::Disconnected)
							sink->lost("connection lost");
					} catch (...) {
					}
				});

			// 2. Heart Rate service 0x180D (uncached: talks to the device).
			onPhase(ConnectPhase::DiscoveringServices);
			auto svcOp = session->device.GetGattServicesForUuidAsync(gatt::GattServiceUuids::HeartRate(),
										 wdb::BluetoothCacheMode::Uncached);
			w = waitFor(svcOp, &cancel, 20000ms);
			if (w != WaitResult::Completed)
				return waitFail(w, "service discovery");
			const auto services = svcOp.GetResults();
			switch (services.Status()) {
			case gatt::GattCommunicationStatus::Success:
				break;
			case gatt::GattCommunicationStatus::Unreachable:
				return fail(ConnectStatus::NotFound, "not in range");
			case gatt::GattCommunicationStatus::AccessDenied:
				return fail(ConnectStatus::AccessDenied, {});
			default:
				return fail(ConnectStatus::Failed, "service discovery error");
			}
			if (services.Services().Size() == 0)
				return fail(ConnectStatus::Unsupported, "no standard Heart Rate service");
			session->service = services.Services().GetAt(0);

			// 3. Heart Rate Measurement characteristic 0x2A37 with Notify.
			auto chrOp = session->service.GetCharacteristicsForUuidAsync(
				gatt::GattCharacteristicUuids::HeartRateMeasurement(), wdb::BluetoothCacheMode::Uncached);
			w = waitFor(chrOp, &cancel, 15000ms);
			if (w != WaitResult::Completed)
				return waitFail(w, "characteristic discovery");
			const auto chars = chrOp.GetResults();
			if (chars.Status() == gatt::GattCommunicationStatus::Unreachable)
				return fail(ConnectStatus::NotFound, "not in range");
			if (chars.Status() == gatt::GattCommunicationStatus::AccessDenied)
				return fail(ConnectStatus::AccessDenied, {});
			if (chars.Status() != gatt::GattCommunicationStatus::Success)
				return fail(ConnectStatus::Failed, "characteristic discovery error");
			if (chars.Characteristics().Size() == 0)
				return fail(ConnectStatus::Unsupported, "no Heart Rate Measurement characteristic");
			session->characteristic = chars.Characteristics().GetAt(0);
			const auto props = session->characteristic.CharacteristicProperties();
			if ((props & gatt::GattCharacteristicProperties::Notify) != gatt::GattCharacteristicProperties::Notify)
				return fail(ConnectStatus::Unsupported, "heart-rate notifications not supported");

			// 4. Register the handler before enabling notifications so no packet is missed,
			//    then write the CCCD (exactly one subscription per connection).
			onPhase(ConnectPhase::Subscribing);
			session->valueToken = session->characteristic.ValueChanged(
				[sink](const gatt::GattCharacteristic &, const gatt::GattValueChangedEventArgs &args) {
					try {
						const auto reader = streams::DataReader::FromBuffer(args.CharacteristicValue());
						std::vector<uint8_t> bytes(reader.UnconsumedBufferLength());
						if (!bytes.empty())
							reader.ReadBytes(bytes);
						sink->data(bytes.data(), bytes.size());
					} catch (...) {
					}
				});
			auto cccdOp = session->characteristic.WriteClientCharacteristicConfigurationDescriptorAsync(
				gatt::GattClientCharacteristicConfigurationDescriptorValue::Notify);
			w = waitFor(cccdOp, &cancel, 15000ms);
			if (w != WaitResult::Completed)
				return waitFail(w, "subscription");
			switch (cccdOp.GetResults()) {
			case gatt::GattCommunicationStatus::Success:
				break;
			case gatt::GattCommunicationStatus::Unreachable:
				return fail(ConnectStatus::NotFound, "not in range");
			case gatt::GattCommunicationStatus::AccessDenied:
				return fail(ConnectStatus::AccessDenied, {});
			default:
				return fail(ConnectStatus::Failed, "could not subscribe to heart-rate data");
			}

			r.status = ConnectStatus::Connected;
			r.session = std::move(session);
			return std::move(r);
		} catch (const winrt::hresult_error &e) {
			if (e.code() == E_ACCESSDENIED)
				return fail(ConnectStatus::AccessDenied, {});
			if (e.code() == HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_AVAILABLE) ||
			    e.code() == HRESULT_FROM_WIN32(ERROR_NOT_READY))
				return fail(ConnectStatus::BluetoothOff, {});
			return fail(ConnectStatus::Failed, narrow(e.message()));
		} catch (...) {
			return fail(ConnectStatus::Failed, "unexpected error");
		}
	}

private:
	static bool &threadInitialized()
	{
		thread_local bool initialized = false;
		return initialized;
	}

	void stopScanLocked()
	{
		// Kill the gate first: once kill() returns no Received handler is running into
		// the manager, so it is safe for the manager to go away afterwards.
		if (gate_)
			gate_->kill();
		if (watcher_) {
			try {
				watcher_.Received(receivedToken_);
				watcher_.Stopped(stoppedToken_);
				watcher_.Stop();
			} catch (...) {
			}
		}
		watcher_ = nullptr;
		gate_.reset();
	}

	CO_MTA_USAGE_COOKIE mtaCookie_ = nullptr;
	std::mutex scanMutex_;
	adv::BluetoothLEAdvertisementWatcher watcher_{nullptr};
	std::shared_ptr<ScanGate> gate_;
	winrt::event_token receivedToken_{};
	winrt::event_token stoppedToken_{};
};

} // namespace

std::unique_ptr<BleBackend> createWinRtBackend()
{
	return std::make_unique<WinRtBackend>();
}

} // namespace blehr
