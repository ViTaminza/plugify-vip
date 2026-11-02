#pragma once
// vip — the text helpers of the original Pisex plugin, engine-free so the tests can call them:
// the chat colour tags, the phrase-language normalisation, printf formatting and the small
// string helpers the plugin shares between its translation units.
//
// The colour tag table is the one the whole Pisex family uses (cs2-menus/utils.h):
//
//   {DEFAULT} {WHITE} {RED} {LIGHTPURPLE} {GREEN} {LIME} {LIGHTGREEN} {DARKRED} {GRAY}
//   {LIGHTOLIVE} {OLIVE} {LIGHTBLUE} {BLUE} {PURPLE} {LIGHTRED} {GRAYBLUE} {\n}
//
// Colorize() replaces them with the exact chat colour bytes (0x01..0x10 and U+2029 for the line
// break) so the chat output of this port is byte-identical to the Metamod server, and
// StripColorTags() removes them (and turns "\n" into a real newline) for the server console,
// which shows the colour bytes as garbage.

#include <array>
#include <cstdarg>
#include <cstdio>
#include <cctype>
#include <string>
#include <string_view>

namespace vip {

struct ColorTag {
	std::string_view tag;
	std::string_view bytes;
};

inline constexpr std::array<ColorTag, 17> kColorTags = {{{"{DEFAULT}", "\x01"},
														 {"{WHITE}", "\x01"},
														 {"{RED}", "\x02"},
														 {"{LIGHTPURPLE}", "\x03"},
														 {"{GREEN}", "\x04"},
														 {"{LIME}", "\x05"},
														 {"{LIGHTGREEN}", "\x06"},
														 {"{DARKRED}", "\x07"},
														 {"{GRAY}", "\x08"},
														 {"{LIGHTOLIVE}", "\x09"},
														 {"{OLIVE}", "\x10"},
														 {"{LIGHTBLUE}", "\x0B"},
														 {"{BLUE}", "\x0C"},
														 {"{PURPLE}", "\x0E"},
														 {"{LIGHTRED}", "\x0F"},
														 {"{GRAYBLUE}", "\x0A"},
														 {"\\n", "\xe2\x80\xa9"}}};

inline std::string ToString(std::string_view view) {
	return std::string(view);
}

inline const char* CStr(const std::string& text) {
	return text.c_str();
}

inline std::string Colorize(std::string_view text) {
	std::string out(text);
	for (const ColorTag& color : kColorTags) {
		std::size_t pos = 0;
		while ((pos = out.find(color.tag, pos)) != std::string::npos) {
			out.replace(pos, color.tag.size(), color.bytes);
			pos += color.bytes.size();
		}
	}
	return out;
}

inline std::string StripColorTags(std::string_view text) {
	std::string out(text);
	for (const ColorTag& color : kColorTags) {
		const std::string_view replacement = color.tag == "\\n" ? std::string_view("\n") : std::string_view();
		std::size_t pos = 0;
		while ((pos = out.find(color.tag, pos)) != std::string::npos) {
			out.replace(pos, color.tag.size(), replacement);
			pos += replacement.size();
		}
	}
	return out;
}

// Maps a Source language name ("russian", "portuguese", ...) to the phrase-file code ("ru",
// "pt-BR", ...). Unknown names are returned unchanged.
inline std::string NormalizeLanguage(std::string_view name) {
	std::string lower;
	lower.reserve(name.size());
	for (const char c : name) {
		lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
	}
	if (lower == "russian" || lower == "ru") return "ru";
	if (lower == "english" || lower == "en") return "en";
	if (lower == "ukrainian" || lower == "ua" || lower == "uk") return "ua";
	if (lower == "portuguese" || lower == "pt-br" || lower == "brazilian") return "pt-BR";
	if (lower == "spanish" || lower == "es") return "es";
	if (lower == "german" || lower == "de") return "de";
	if (lower == "french" || lower == "fr") return "fr";
	if (lower == "polish" || lower == "pl") return "pl";
	if (lower == "turkish" || lower == "tr") return "tr";
	if (lower == "schinese" || lower == "zh-cn") return "zh-CN";
	return std::string(name);
}

// Pisex's 512-byte formatting buffer with the port's headroom; anything longer is cut here,
// never in the engine.
inline constexpr std::size_t kFormatBufferSize = 2048;

inline std::string FormatString(const char* format, ...) __attribute__((format(printf, 1, 2)));
inline std::string FormatString(const char* format, ...) {
	std::array<char, kFormatBufferSize> buffer{};
	va_list args;
	va_start(args, format);
	const int written = std::vsnprintf(buffer.data(), buffer.size(), format != nullptr ? format : "", args);
	va_end(args);
	if (written <= 0) {
		return {};
	}
	const std::size_t size = static_cast<std::size_t>(written) < buffer.size() - 1
								 ? static_cast<std::size_t>(written)
								 : buffer.size() - 1;
	return std::string(buffer.data(), size);
}

// Whether a string is made of digits only (containsOnlyDigits of the original).
inline bool ContainsOnlyDigits(std::string_view text) {
	if (text.empty()) {
		return false;
	}
	return text.find_first_not_of("0123456789") == std::string_view::npos;
}

} // namespace vip
