#include "display_format.hpp"

namespace blehr {

namespace {
const char *const kHeart = "\xE2\x99\xA5"; // U+2665 BLACK HEART SUIT, UTF-8
}

DisplayStyle displayStyleFromInt(int64_t value)
{
	switch (value) {
	case 0:
		return DisplayStyle::NumberOnly;
	case 1:
		return DisplayStyle::NumberUnit;
	case 2:
		return DisplayStyle::HeartNumber;
	case 3:
		return DisplayStyle::HeartNumberUnit;
	default:
		return kDefaultDisplayStyle;
	}
}

static std::string compose(DisplayStyle style, const std::string &number)
{
	switch (style) {
	case DisplayStyle::NumberOnly:
		return number;
	case DisplayStyle::NumberUnit:
		return number + " BPM";
	case DisplayStyle::HeartNumber:
		return std::string(kHeart) + " " + number;
	case DisplayStyle::HeartNumberUnit:
		break;
	}
	return std::string(kHeart) + " " + number + " BPM";
}

std::string formatBpm(DisplayStyle style, uint16_t bpm)
{
	return compose(style, std::to_string(bpm));
}

std::string layoutSample(DisplayStyle style)
{
	return compose(style, "888");
}

} // namespace blehr

namespace blehr {

const char *const kHeartGlyph = "\xE2\x99\xA5";

bool styleShowsHeart(DisplayStyle style)
{
	return style == DisplayStyle::HeartNumber || style == DisplayStyle::HeartNumberUnit;
}

static bool styleShowsUnit(DisplayStyle style)
{
	return style == DisplayStyle::NumberUnit || style == DisplayStyle::HeartNumberUnit;
}

std::string formatNumber(DisplayStyle style, uint16_t bpm)
{
	return std::to_string(bpm) + (styleShowsUnit(style) ? " BPM" : "");
}

std::string numberLayoutSample(DisplayStyle style)
{
	return std::string("888") + (styleShowsUnit(style) ? " BPM" : "");
}

} // namespace blehr
