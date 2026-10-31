// Text shown by the source for a valid measurement.
#pragma once

#include <cstdint>
#include <string>

namespace blehr {

enum class DisplayStyle : int {
	NumberOnly = 0,     // "82"
	NumberUnit = 1,     // "82 BPM"
	HeartNumber = 2,    // "♥ 82"
	HeartNumberUnit = 3 // "♥ 82 BPM" (default)
};

constexpr DisplayStyle kDefaultDisplayStyle = DisplayStyle::HeartNumberUnit;

DisplayStyle displayStyleFromInt(int64_t value);
std::string formatBpm(DisplayStyle style, uint16_t bpm);

// Widest text for a style; used only to give the source a stable layout size while it
// is invisible. Never rendered.
std::string layoutSample(DisplayStyle style);

} // namespace blehr

namespace blehr {

// The renderer draws the heart as its own (animated) glyph; these give the parts.
bool styleShowsHeart(DisplayStyle style);
std::string formatNumber(DisplayStyle style, uint16_t bpm); // "82" or "82 BPM"
std::string numberLayoutSample(DisplayStyle style);         // "888" or "888 BPM"
extern const char *const kHeartGlyph;                        // U+2665, UTF-8

} // namespace blehr
