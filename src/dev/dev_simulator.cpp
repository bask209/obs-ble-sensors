#include "dev_simulator.hpp"

namespace blehr {

DevSimulator::DevSimulator() : owned_(std::make_unique<MockBackend>())
{
	mock_ = owned_.get();
	MockDevice device;
	device.identity.address = kAddress;
	device.name = "Simulated HRM (dev mode)";
	mock_->addDevice(device);

	MockDevice watch;
	watch.identity.address = 0xDE0000000002ULL;
	watch.name = "Simulated watch (unsupported)";
	watch.advertisesHeartRate = false;
	watch.supportsHeartRate = false;
	mock_->addDevice(watch);
}

DevSimulator::~DevSimulator()
{
	stop();
}

std::unique_ptr<BleBackend> DevSimulator::takeBackend()
{
	return std::move(owned_);
}

void DevSimulator::start()
{
	if (running_.exchange(true))
		return;
	thread_ = std::thread(&DevSimulator::run, this);
}

void DevSimulator::stop()
{
	stop_.cancel();
	if (thread_.joinable())
		thread_.join();
	running_ = false;
}

void DevSimulator::run()
{
	for (int t = 0; !stop_.cancelled(); t = (t + 1) % 40) {
		if (t == 30) {
			mock_->setPresent(kAddress, false);
			mock_->dropConnection(kAddress, "simulated sensor off");
		} else if (t == 35) {
			mock_->setPresent(kAddress, true);
		}
		if (t < 20)
			mock_->notify(kAddress, {0x06, uint8_t(70 + (t % 10))}); // contact detected
		mock_->advertise(kAddress);
		mock_->advertise(0xDE0000000002ULL);
		if (stop_.waitFor(std::chrono::milliseconds(1000)))
			break;
	}
}

} // namespace blehr
