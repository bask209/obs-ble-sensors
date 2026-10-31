#include "device_identity.hpp"

#include <cctype>
#include <cstdio>

namespace blehr {

namespace {
constexpr const char *kPrefixPublic = "ble:public:";
constexpr const char *kPrefixRandom = "ble:random:";

int hexValue(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	return -1;
}

bool startsWith(const std::string &s, const char *prefix, size_t &len)
{
	len = std::char_traits<char>::length(prefix);
	return s.compare(0, len, prefix) == 0;
}
} // namespace

std::string formatAddress(uint64_t address)
{
	char buf[18];
	std::snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", unsigned((address >> 40) & 0xFF),
		      unsigned((address >> 32) & 0xFF), unsigned((address >> 24) & 0xFF),
		      unsigned((address >> 16) & 0xFF), unsigned((address >> 8) & 0xFF), unsigned(address & 0xFF));
	return buf;
}

std::optional<uint64_t> parseAddress(const std::string &text)
{
	// Expect exactly XX:XX:XX:XX:XX:XX.
	if (text.size() != 17)
		return std::nullopt;
	uint64_t value = 0;
	for (size_t i = 0; i < 6; ++i) {
		const size_t p = i * 3;
		const int hi = hexValue(text[p]);
		const int lo = hexValue(text[p + 1]);
		if (hi < 0 || lo < 0)
			return std::nullopt;
		if (i < 5 && text[p + 2] != ':')
			return std::nullopt;
		value = (value << 8) | static_cast<uint64_t>(hi * 16 + lo);
	}
	if (value == 0)
		return std::nullopt;
	return value;
}

std::string DeviceIdentity::key() const
{
	return std::string(addressType == AddressType::Random ? kPrefixRandom : kPrefixPublic) + formatAddress(address);
}

std::optional<DeviceIdentity> identityFromKey(const std::string &key)
{
	size_t len = 0;
	DeviceIdentity id;
	if (startsWith(key, kPrefixPublic, len))
		id.addressType = AddressType::Public;
	else if (startsWith(key, kPrefixRandom, len))
		id.addressType = AddressType::Random;
	else
		return std::nullopt;

	auto address = parseAddress(key.substr(len));
	if (!address)
		return std::nullopt;
	id.address = *address;
	return id;
}

} // namespace blehr
