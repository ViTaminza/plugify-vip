#pragma once
// vip — the exact s2sdk menu-type table of the previous wrapper
// (libs/pisexcompat/src/menu.cpp, ResolveS2sdkMenuTypeName with the plugin's MenuType::HUD_LAYOUT
// default and no forced override), engine-free so the tests can check every branch.
//
// The player's own "!menu" choice comes from the optional hudmenu plugin:
//
//   "chat" / "centerhtml" / "button" -> that builtin type (always registered by s2sdk);
//   "hud"  -> "hud" when that backend is registered, otherwise "button";
//   "wasd" -> "wasd" when that backend is registered, otherwise "button" (never "hud");
//   anything else -> the HUD_LAYOUT default;
//
// and the HUD_LAYOUT default (no choice, no hudmenu plugin, an unknown answer) is "hud" when the
// hudmenu backend is registered and "button" otherwise - exactly what the old wrapper did.

#include <string>
#include <string_view>

namespace vip {

inline std::string_view ResolveMenuTypeName(bool hudRegistered, bool wasdRegistered,
											std::string_view playerChoice) {
	if (!playerChoice.empty()) {
		if (playerChoice == "chat" || playerChoice == "centerhtml" || playerChoice == "button") {
			return playerChoice;
		}
		if (playerChoice == "hud") {
			return hudRegistered ? "hud" : "button";
		}
		if (playerChoice == "wasd") {
			return wasdRegistered ? "wasd" : "button";
		}
	}
	return hudRegistered ? "hud" : "button";
}

} // namespace vip
