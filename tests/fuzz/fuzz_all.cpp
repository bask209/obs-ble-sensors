// Coverage-guided fuzz target (libFuzzer). Everything here can be attacker-controlled:
// radio packets, advertised names and identity keys read from the config file.
//   clang++ -fsanitize=fuzzer,address,undefined ...   (see tests/CMakeLists.txt, BLEHR_FUZZ)
#include "core/device_identity.hpp"
#include "core/heart_rate_parser.hpp"
#include "core/sensor_state.hpp"

#include <cstdint>
#include <cstdlib>
#include <string>

using namespace blehr;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	if (size == 0)
		return 0;
	// First byte picks the format; the rest is the packet.
	const MeasurementKind kind = MeasurementKind(data[0] % 7);
	const HeartRateMeasurement m = parseMeasurement(kind, data + 1, size - 1);
	if (m.status == ParseStatus::Ok && (m.bpm < kMinValidBpm || m.bpm > kMaxValidBpm))
		std::abort();
	if (m.rrCount > HeartRateMeasurement::kMaxRr)
		std::abort();

	// Drive the state machine and the packed render word with the same input.
	SensorLinkMachine machine;
	machine.subscribing();
	machine.subscribed(1000000);
	int64_t t = 1000000;
	for (size_t i = 1; i + 2 <= size; i += 2) {
		t += int64_t(data[i]) * 1000000;
		machine.measurement(parseMeasurement(kind, data + i, 2), t);
		uint16_t bpm = 0;
		uint64_t gen = 0;
		const bool show = RenderWord::evaluate(RenderWord::pack(machine.snapshot()), t, 5000000000LL, bpm, gen);
		if (show && (bpm < kMinValidBpm || bpm > kMaxValidBpm))
			std::abort();
		if (show && !shouldRender(machine.snapshot(), t, 5000000000LL) &&
		    t - machine.snapshot().lastValidNs < 4990000000LL)
			std::abort();
	}

	// Strings: names from advertisements and keys from the config file.
	const std::string text(reinterpret_cast<const char *>(data), size);
	const std::string name = sanitizeDeviceName(text);
	if (name.size() > 64)
		std::abort();
	for (unsigned char c : name)
		if (c < 0x20 || c == 0x7F)
			std::abort();
	if (auto id = identityFromKey(text)) {
		if (identityFromKey(id->key())->address != id->address)
			std::abort();
	}
	return 0;
}
