// vip — the VIP menu: ShowVIPMenu() and VIPCallback() of the original (vip.cpp), on the
// pisexcompat menu API (the IMenusApi of the original).
//
// The menu lists every feature of the player's group (the group table with the 'legacy' chain
// applied) that has a value type and is not hidden. SELECTABLE entries run their callback,
// TOGGLABLE entries flip the '<group>.<feature>' cookie - including the veto hook and the
// immediate re-open of the original. Item numbers 0..6 are live (the original ignored anything
// above the seventh entry of its pageless menu).

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <utility>

#include <pisex/pisex.hpp>

#include "vip_plugin.hpp"

namespace vip {

namespace {

// A feature that was never registered: the original's feature map default-constructed an entry
// on lookup, so an unregistered feature behaved like a togglable one without callbacks.
const Feature kNoFeature{};

// atoi() of the original for the cookie ("0" = off, an unparsable value counts as 0 = off).
long CookieAtoi(const std::string& text) {
	return std::atoi(text.c_str());
}

void VipMenuCallback(const char* back, const char* front, int item, int slot) {
	try {
		g_vipPlugin.HandleMenuCallback(back, front, item, slot);
	} catch (const std::exception& ex) {
		LogError(std::string("vip: exception in the VIP menu callback: ") + ex.what());
	} catch (...) {
		LogError("vip: unknown exception in the VIP menu callback");
	}
}

} // namespace

void ShowVipMenu(int32_t slot) {
	g_vipPlugin.ShowMenu(slot, true);
}

void VipPlugin::ShowMenu(int32_t slot, bool reopen) {
	// PARITY (ShowVIPMenu): bots and players without access get nothing - the latter the
	// "NotAccess" message.
	if (m_core == nullptr || pisex::g_pPlayers->IsFakeClient(slot)) {
		return;
	}
	const GroupFeatures* group = GroupFeaturesForSlot(slot);
	if (group == nullptr) {
		pisex::PrintToChat(slot, "%s", CStr(Phrase("NotAccess")));
		return;
	}

	pisex::Menu menu;
	pisex::SetTitleMenu(menu, CStr(Phrase("MenuTitle")));

	for (const auto& pair : *group) {
		const std::string& key = pair.first;
		const char* feature = key.c_str();
		if (std::strcmp(feature, "legacy") == 0) {
			continue;
		}
		const std::string& translated = pisex::GetPhrase(feature);
		const std::string feature_text = FeatureString(slot, feature);

		const auto entry = m_features.find(key);
		const Feature& functions = entry == m_features.end() ? kNoFeature : entry->second;
		if (functions.value_type == kValueNull || functions.feature_type == kFeatureHide) {
			continue;
		}

		std::string label;
		if (functions.feature_type == kFeatureSelectable) {
			// PARITY: only the translated name.
			label = translated.empty() ? feature : translated;
		} else {
			// PARITY: "name [value]", where a boolean shows On/Off and a missing value Off.
			// The text is kept in a std::string: Phrase() returns by value, so a bare
			// CStr(Phrase(...)) would hand out a pointer into a destroyed temporary.
			std::string shown;
			if (!feature_text.empty()) {
				shown = functions.value_type == kValueBool ? Phrase("On") : feature_text;
			} else {
				shown = Phrase("Off");
			}
			label = translated.empty() ? feature : translated;
			label += " [";
			label += shown;
			label += "]";
		}

		std::string display;
		if (functions.display != nullptr) {
			display = ToString(functions.display(slot, plg::string(feature)));
		}
		pisex::AddItemMenu(menu, feature, (display.empty() ? label : display).c_str());
	}

	// PARITY: no back menu, an exit menu, and the menu opened with the re-open flag.
	pisex::SetBackMenu(menu, false);
	pisex::SetExitMenu(menu, true);
	pisex::SetCallback(menu, &VipMenuCallback);
	pisex::DisplayPlayerMenu(menu, slot, true, reopen);
}

void VipPlugin::HandleMenuCallback(const char* back, const char* front, int item, int slot) {
	(void)front;
	// PARITY (VIPCallback): only the first seven entries are live, and a feature that is not
	// registered behaves like a togglable one without callbacks.
	if (back == nullptr || slot < 0 || slot >= kMaxSlots || item >= 7) {
		return;
	}
	const auto entry = m_features.find(back);
	const Feature& functions = entry == m_features.end() ? kNoFeature : entry->second;

	if (functions.feature_type != kFeatureSelectable) {
		const std::string cookie = GetCookie(slot, back);
		int old_status = 0;
		int new_status = 0;
		if (cookie.empty() || CookieAtoi(cookie) != 0) {
			old_status = kToggleEnabled;
			new_status = kToggleDisabled;
		} else {
			old_status = kToggleDisabled;
			new_status = kToggleEnabled;
		}

		bool blocked = false;
		if (functions.toggle != nullptr) {
			::vip::ToggleState old_state = static_cast<::vip::ToggleState>(old_status);
			::vip::ToggleState new_state = static_cast<::vip::ToggleState>(new_status);
			blocked = functions.toggle(slot, plg::string(back), old_state, new_state);
			new_status = static_cast<int>(new_state);
		}
		// PARITY: a vetoed toggle keeps the old status in the cookie, otherwise the new one.
		SetCookie(slot, back, std::to_string(blocked ? old_status : new_status));
		ShowMenu(slot, false);
		return;
	}

	bool close = false;
	if (functions.select != nullptr) {
		close = functions.select(slot, plg::string(back));
	}
	if (close) {
		// PARITY: the menu is closed one frame later.
		pisex::NextFrame([slot] {
			try {
				pisex::ClosePlayerMenu(slot);
			} catch (...) {
			}
		});
	}
}

} // namespace vip
