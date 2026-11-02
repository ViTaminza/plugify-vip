// vip — the VIP menu: ShowVIPMenu() and VIPCallback() of the original (vip.cpp), on native s2sdk
// menus (no pisexcompat, PITFALLS C10).
//
// The menu lists every feature of the player's group (the group table with the 'legacy' chain
// applied) that has a value type and is not hidden. SELECTABLE entries run their callback,
// TOGGLABLE entries flip the '<group>.<feature>' cookie - including the veto hook and the
// immediate re-open of the original. Item numbers 1..7 are live (the original ignored anything
// above the seventh entry of its pageless menu).
//
// The s2sdk menu contract this file follows (the same rules the other pure-s2sdk plugins of this
// tree document in handoff/weaponskins/PITFALLS.md C10 and in their own menu teardown):
//   * menu id 0 is a real id (the first CreateMenu of the process returns it) AND the manager's
//     "no menu" sentinel (ClientMenuState::id{}), so it is burned at plugin start, before any menu
//     of this plugin is created. The dummy's handler ignores every action (no session matches).
//   * the complete lifecycle binding set (Create/Destroy/Display/AddItem/GetClientMenu/
//     CancelClientMenu/SetMenuType/GetMenuType) is required before the first CreateMenu - without
//     the teardown half a menu whose handler points into this library could survive OnPluginEnd.
//   * the engine calls MenuHandler under the menu manager's lock, so the handler only records the
//     action in a queue; every menu call (re-open, close, destroy, cookie write, feature callback)
//     runs from OnPluginUpdate outside that lock. Each queued action carries the SteamID64 and the
//     controller entity handle the slot had when the item was clicked and revalidates both before
//     it runs: a player who left, or a slot reused by a reconnect, can never inherit the action.
//   * s2sdk never destroys a menu by itself (EndClientMenu only clears the client state), so every
//     Cancel/End queues DestroyMenu. The slot's previous menu is destroyed before a new one is
//     displayed; OnMapEnd and OnPluginEnd end a still-active display with
//     CancelClientMenu(Disconnect), re-check GetClientMenu and - if a display somehow survives in
//     the changelevel window - switch the menu to the builtin "chat"/"console" backend (verified
//     with GetMenuType) before DestroyMenu, so no backend close touches the torn-down client layer.
//     An id that cannot be released is reported as A7_UNRESOLVED, never silently dropped.
//   * the menu type is the exact table of the old wrapper (menu_type.hpp): the player's own
//     "!menu" choice from the optional hudmenu plugin, else the HUD_LAYOUT default ("hud" when
//     that backend is registered, "button" otherwise).
//
// The page size is the original chat menu's five (SetMenuPagination), with the manager's default
// of seven used for the visible-page guard when that binding is missing.

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <hudmenu/hudmenu.hpp>
#include <plg/string.hpp>
#include <s2sdk/s2sdk.hpp>

#include "menu_type.hpp"
#include "vip_plugin.hpp"

namespace vip {

namespace {

constexpr int kMenuItemsPerPage = 5;          // cs2-menus shows five items per chat page
constexpr int kS2sdkDefaultItemsPerPage = 7;  // MenuData::itemsPerPage default
constexpr int32_t kMenuTimeoutSeconds = 60;   // pisexcompat's default menu timeout
constexpr int kMaxPendingMenuActions = 256;

struct MenuItem {
	std::string info;
	std::string display;
};

struct MenuSession {
	s2sdk::MenuId id = 0; // 0 = none (the reserved dummy id 0 is never a session)
	// The identity of the player the menu was created for. It is captured outside the menu
	// handler (ShowMenuNow) and used by the handler instead of querying the current slot owner:
	// a full MenuManager lock must not run client/entity queries, and by the time an item is
	// clicked the slot may already belong to another player or to the same SteamID64 after a
	// reconnect with a new controller serial.
	uint64_t ownerSteamId = 0;
	int32_t ownerController = 0;
	std::vector<MenuItem> items;
};

std::array<MenuSession, kMaxSlots> g_menu_sessions;
std::vector<s2sdk::MenuId> g_pending_destroy;
std::vector<s2sdk::MenuId> g_parked_menus;
s2sdk::MenuId g_reserved_menu_id = 0;
bool g_has_reserved_menu = false;
bool g_menus_available = false;

// A feature that was never registered: the original's feature map default-constructed an entry
// on lookup, so an unregistered feature behaved like a togglable one without callbacks.
const Feature kNoFeature{};

bool SlotInRange(int32_t slot) {
	return slot >= 0 && slot < kMaxSlots;
}

long CookieAtoi(const std::string& text) {
	return std::atoi(text.c_str());
}

bool MenuTypeRegistered(const char* name) {
	if (__s2sdk_IsMenuTypeRegistered == nullptr) {
		return false;
	}
	try {
		return s2sdk::IsMenuTypeRegistered(plg::string(name));
	} catch (...) {
		return false;
	}
}

// The old wrapper resolved a Pisex MenuType per client: hudmenu's GetClientMenuType() answer is
// the player's own "!menu" choice (already validated by that plugin), otherwise the HUD_LAYOUT
// default - hudmenu's "hud" backend when it is registered, s2sdk's WASD "button" type when not.
std::string ResolveMenuTypeForSlot(int32_t slot) {
	const bool hudRegistered = MenuTypeRegistered("hud");
	const bool wasdRegistered = MenuTypeRegistered("wasd");
	std::string choice;
	if (__hudmenu_GetClientMenuType != nullptr && IsGameReady() && SlotInRange(slot)) {
		try {
			choice = ToString(hudmenu::GetClientMenuType(slot));
		} catch (...) {
			choice.clear();
		}
	}
	return std::string(ResolveMenuTypeName(hudRegistered, wasdRegistered, choice));
}

int MenuPageSize() {
	return __s2sdk_SetMenuPagination != nullptr ? kMenuItemsPerPage : kS2sdkDefaultItemsPerPage;
}

// --- safe menu destruction (PITFALLS B2 + A7) ---------------------------------------------------
//
// DestroyMenu(id) ends a still-active display with MenuCancelReason::Destroyed and then calls the
// backend's close - the builtin "chat"/"console" closers are no-ops, "centerhtml"/"button" and
// hudmenu's "hud"/"wasd" reach the client/entity layer that is torn down between OnMapEnd and the
// next OnMapStart. Therefore every destruction:
//   1. ends the display with CancelClientMenu(Disconnect) (no backend close) and re-checks
//      GetClientMenu when the cancel answered false ("already ended");
//   2. if a display is still there in a map-down window (only possible if the manager violates
//      that contract), switches the menu to a builtin no-op-close type with SetMenuType, verified
//      with GetMenuType, before DestroyMenu;
//   3. reports failure instead of silently keeping a menu whose handler points into this library.

int FindDisplaySlot(s2sdk::MenuId id) {
	if (id == 0 || __s2sdk_GetClientMenu == nullptr) {
		return -1;
	}
	for (int32_t slot = 0; slot < kMaxSlots; ++slot) {
		try {
			if (s2sdk::GetClientMenu(slot) == id) {
				return slot;
			}
		} catch (...) {
		}
	}
	return -1;
}

bool MakeMenuCloseNoOp(s2sdk::MenuId id) {
	if (__s2sdk_SetMenuType == nullptr || __s2sdk_GetMenuType == nullptr) {
		return false;
	}
	for (const char* type : {"chat", "console"}) {
		if (__s2sdk_IsMenuTypeRegistered != nullptr) {
			bool registered = false;
			try {
				registered = s2sdk::IsMenuTypeRegistered(plg::string(type));
			} catch (...) {
				registered = false;
			}
			if (!registered) {
				continue;
			}
		}
		try {
			if (!s2sdk::SetMenuType(id, plg::string(type))) {
				continue;
			}
			if (ToString(s2sdk::GetMenuType(id)) == type) {
				return true;
			}
		} catch (...) {
		}
	}
	return false;
}

// Ends every display of `id` and destroys the menu; the caller must not be inside a menu handler.
// mapReady == false is the changelevel window, where a backend close must not touch a client.
// Returns true when the id is gone from s2sdk's menu table.
bool DestroyMenuChecked(s2sdk::MenuId id, bool mapReady) {
	if (__s2sdk_DestroyMenu == nullptr) {
		return false;
	}
	if (id == 0) {
		// The reserved id: client state 0 is s2sdk's "no menu", so it can never be displayed.
		try {
			s2sdk::DestroyMenu(id);
		} catch (...) {
			return false;
		}
		return true;
	}
	const int displaySlot = FindDisplaySlot(id);
	if (displaySlot >= 0) {
		bool cancelled = false;
		if (__s2sdk_CancelClientMenu != nullptr) {
			try {
				cancelled = s2sdk::CancelClientMenu(displaySlot, s2sdk::MenuCancelReason::Disconnect);
			} catch (...) {
				cancelled = false;
			}
		}
		if (!cancelled && FindDisplaySlot(id) >= 0 && !mapReady && !MakeMenuCloseNoOp(id)) {
			return false; // would need a backend close over the torn-down client layer
		}
	}
	try {
		s2sdk::DestroyMenu(id);
	} catch (...) {
		return false;
	}
	if (__s2sdk_IsValidMenu != nullptr) {
		try {
			return !s2sdk::IsValidMenu(id);
		} catch (...) {
		}
	}
	return true;
}

// True while the id is still a slot's current menu or still displayed to a client.
bool MenuIdStillUsed(s2sdk::MenuId id) {
	for (const MenuSession& session : g_menu_sessions) {
		if (session.id == id) {
			return true;
		}
	}
	return FindDisplaySlot(id) >= 0;
}

void QueueMenuDestroy(s2sdk::MenuId id) {
	if (id == 0) {
		return; // s2sdk's "no menu"
	}
	for (const s2sdk::MenuId queued : g_pending_destroy) {
		if (queued == id) {
			return; // Cancel and End both report the same session end: destroy it once
		}
	}
	g_pending_destroy.push_back(id);
}

void DestroyQueuedMenus() {
	if (g_pending_destroy.empty()) {
		return;
	}
	std::vector<s2sdk::MenuId> ids = std::move(g_pending_destroy);
	g_pending_destroy.clear();
	for (const s2sdk::MenuId id : ids) {
		if (MenuIdStillUsed(id) || !DestroyMenuChecked(id, IsGameReady())) {
			g_pending_destroy.push_back(id);
		}
	}
}

// Destroys the slot's current menu, outside any menu handler.
void CloseSlotMenu(int32_t slot) {
	if (!SlotInRange(slot)) {
		return;
	}
	const s2sdk::MenuId id = g_menu_sessions[static_cast<std::size_t>(slot)].id;
	g_menu_sessions[static_cast<std::size_t>(slot)] = MenuSession{};
	if (id == 0) {
		return;
	}
	if (!DestroyMenuChecked(id, IsGameReady())) {
		g_parked_menus.push_back(id);
		LogWarn("vip: could not close the menu of slot " + std::to_string(slot) +
				" - kept for the map-end/unload path");
	}
}

// Stable identities of every menu id this plugin still owns; the bookkeeping is cleared.
std::vector<s2sdk::MenuId> CollectOwnedMenuIds() {
	std::vector<s2sdk::MenuId> ids;
	for (MenuSession& session : g_menu_sessions) {
		if (session.id != 0) {
			ids.push_back(session.id);
		}
		session = MenuSession{};
	}
	for (const s2sdk::MenuId id : g_pending_destroy) {
		if (id != 0) {
			ids.push_back(id);
		}
	}
	g_pending_destroy.clear();
	for (const s2sdk::MenuId id : g_parked_menus) {
		if (id != 0) {
			ids.push_back(id);
		}
	}
	g_parked_menus.clear();
	std::sort(ids.begin(), ids.end());
	ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
	return ids;
}

void RetryParkedMenus() {
	if (g_parked_menus.empty() || !IsGameReady()) {
		return;
	}
	std::vector<s2sdk::MenuId> ids = std::move(g_parked_menus);
	g_parked_menus.clear();
	for (const s2sdk::MenuId id : ids) {
		if (DestroyMenuChecked(id, /*mapReady=*/true)) {
			continue;
		}
		g_parked_menus.push_back(id);
		LogError("vip: could not destroy menu " + std::to_string(id) + " with a live map - keeping it for the "
				 "unload path");
	}
}

// --- deferred menu actions --------------------------------------------------------------------

enum class PendingKind {
	Show,     // open/replace the VIP menu of the player
	Callback, // a feature item was selected: run the original's VIPCallback
};

struct PendingAction {
	PendingKind kind = PendingKind::Callback;
	int32_t slot = -1;
	// The menu the action was taken from (0 for an open request): lets ProcessMenuWork close
	// exactly the stale menu and never a replacement opened by the slot's new owner.
	s2sdk::MenuId menuId = 0;
	// The identity of the player when the item was clicked. SteamID64 alone is not enough: a
	// reconnect can land the same SteamID in the same slot with a new controller entity.
	uint64_t steamid = 0;
	int32_t controller = 0;
	std::string feature;
	std::string display;
	int button = 0;
};

std::deque<PendingAction> g_pending_actions;

void OnMenuHandler(s2sdk::MenuId id, s2sdk::MenuAction action, int32_t slot, int32_t param);

} // namespace

// --- the menu ---------------------------------------------------------------------------------

void VipPlugin::ShowMenu(int32_t slot, bool reopen) {
	// Every menu action - including opening one from a command or from the toggle callback - goes
	// through the deferred queue and runs from OnPluginUpdate, outside any menu handler or engine
	// lock (PITFALLS C10). The action carries the identity of the player it was built for.
	(void)reopen;
	if (!g_menus_available || !IsGameReady() || !IsSlotHuman(slot)) {
		return;
	}
	const uint64_t steamid = ClientSteamIdOf(slot);
	const int32_t controller = ControllerHandleOf(slot);
	if (steamid == 0 || controller == 0) {
		return;
	}
	if (g_pending_actions.size() >= static_cast<std::size_t>(kMaxPendingMenuActions)) {
		return;
	}
	PendingAction action;
	action.kind = PendingKind::Show;
	action.slot = slot;
	action.menuId = 0;
	action.steamid = steamid;
	action.controller = controller;
	g_pending_actions.push_back(std::move(action));
}

void VipPlugin::ShowMenuNow(int32_t slot) {
	// PARITY (ShowVIPMenu): bots and players without access get nothing - the latter the
	// "NotAccess" message. The identity was revalidated by ProcessMenuWork right before this call.
	if (!g_menus_available || !IsGameReady() || !IsSlotHuman(slot)) {
		return;
	}
	// The owner of this menu, captured here (outside the menu handler) and stored in the session:
	// every select of this menu is revalidated against it. 0 and -1 can never identify a player
	// (0 = none, -1 = INVALID_EHANDLE_INDEX), so a menu is only created for a real owner.
	const uint64_t ownerSteamId = ClientSteamIdOf(slot);
	const int32_t ownerController = ControllerHandleOf(slot);
	if (ownerSteamId == 0 || ownerController == 0 || ownerController == -1) {
		return;
	}
	const GroupFeatures* group = GroupFeaturesForSlot(slot);
	if (group == nullptr) {
		ChatLine(slot, Phrase("NotAccess"));
		return;
	}

	std::vector<MenuItem> items;
	for (const auto& pair : *group) {
		const std::string& key = pair.first;
		const char* feature = key.c_str();
		if (std::strcmp(feature, "legacy") == 0) {
			continue;
		}
		const std::string translated = Phrase(feature);
		const std::string featureText = FeatureString(slot, feature);

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
			std::string shown;
			if (!featureText.empty()) {
				shown = functions.value_type == kValueBool ? Phrase("On") : featureText;
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
		items.push_back(MenuItem{key, display.empty() ? label : display});
	}

	// Replace whatever this client had open (the old wrapper's DisplayPlayerMenu did the same).
	CloseSlotMenu(slot);

	s2sdk::MenuId id = 0;
	try {
		id = s2sdk::CreateMenu(plg::string(Phrase("MenuTitle")), &OnMenuHandler,
							   plg::string(ResolveMenuTypeForSlot(slot)));
	} catch (...) {
		return;
	}
	if (id == 0) {
		LogError("vip: s2sdk refused to create the VIP menu");
		return;
	}

	bool ok = true;
	try {
		for (const MenuItem& item : items) {
			if (s2sdk::AddMenuItem(id, plg::string(item.info), plg::string(item.display),
								   s2sdk::MenuItemStyle::Default) < 0) {
				ok = false;
			}
		}
		if (__s2sdk_SetMenuPagination) {
			s2sdk::SetMenuPagination(id, kMenuItemsPerPage);
		}
		if (__s2sdk_SetMenuExitButton) {
			s2sdk::SetMenuExitButton(id, true);
		}
		if (__s2sdk_SetMenuExitBackButton) {
			s2sdk::SetMenuExitBackButton(id, false);
		}
		// PARITY: Pisex keeps the menu open after a selection; the deferred callback decides
		// what happens next (a toggle re-opens the menu, a select may close it).
		if (__s2sdk_SetMenuCloseOnSelect) {
			s2sdk::SetMenuCloseOnSelect(id, false);
		}
	} catch (...) {
		ok = false;
	}
	if (!ok) {
		LogError("vip: could not display the VIP menu on slot " + std::to_string(slot));
		(void)DestroyMenuChecked(id, IsGameReady());
		return;
	}

	MenuSession& session = g_menu_sessions[static_cast<std::size_t>(slot)];
	session.id = id;
	session.ownerSteamId = ownerSteamId;
	session.ownerController = ownerController;
	session.items = std::move(items);
	if (!s2sdk::DisplayMenu(id, slot, kMenuTimeoutSeconds)) {
		LogError("vip: could not display the VIP menu on slot " + std::to_string(slot));
		session = MenuSession{};
		(void)DestroyMenuChecked(id, IsGameReady());
	}
}

void VipPlugin::HandleMenuCallback(const char* back, const char* front, int item, int slot) {
	(void)front;
	// PARITY (VIPCallback): only the first seven entries are live, and a feature that is not
	// registered behaves like a togglable one without callbacks.
	if (back == nullptr || !SlotInRange(slot) || item >= 7) {
		return;
	}
	const auto entry = m_features.find(back);
	const Feature& functions = entry == m_features.end() ? kNoFeature : entry->second;

	if (functions.feature_type != kFeatureSelectable) {
		const std::string cookie = GetCookie(slot, back);
		int oldStatus = 0;
		int newStatus = 0;
		if (cookie.empty() || CookieAtoi(cookie) != 0) {
			oldStatus = kToggleEnabled;
			newStatus = kToggleDisabled;
		} else {
			oldStatus = kToggleDisabled;
			newStatus = kToggleEnabled;
		}

		bool blocked = false;
		if (functions.toggle != nullptr) {
			::vip::ToggleState oldState = static_cast<::vip::ToggleState>(oldStatus);
			::vip::ToggleState newState = static_cast<::vip::ToggleState>(newStatus);
			blocked = functions.toggle(slot, plg::string(back), oldState, newState);
			newStatus = static_cast<int>(newState);
		}
		// PARITY: a vetoed toggle keeps the old status in the cookie, otherwise the new one.
		SetCookie(slot, back, std::to_string(blocked ? oldStatus : newStatus));
		ShowMenu(slot, false);
		return;
	}

	bool close = false;
	if (functions.select != nullptr) {
		close = functions.select(slot, plg::string(back));
	}
	if (close) {
		// PARITY: the original closed the menu one frame later; this callback already runs
		// outside the menu handler, so the close is immediate.
		CloseSlotMenu(slot);
	}
}

namespace {

void OnMenuHandler(s2sdk::MenuId id, s2sdk::MenuAction action, int32_t slot, int32_t param) {
	// Called by the engine under the menu manager's lock: no menu call, no feature callback, no
	// client query happens here - everything is queued for OnPluginUpdate.
	try {
		if (!SlotInRange(slot)) {
			return;
		}
		MenuSession& session = g_menu_sessions[static_cast<std::size_t>(slot)];
		if (session.id != id) {
			return; // stale (the plugin already closed/replaced it)
		}

		switch (action) {
		case s2sdk::MenuAction::Start:
			return;
		case s2sdk::MenuAction::Select: {
			if (param < 0 || static_cast<std::size_t>(param) >= session.items.size()) {
				return;
			}
			// s2sdk accepts digits 1..7 even when the page shows five items and maps them to
			// currentOffset + digit - 1, so 6/7 select an item of the next page that was never
			// displayed. Pisex numbers the visible items 1..5: derive the number from the page
			// offset and ignore selections outside the page.
			const int pageSize = MenuPageSize();
			int offset = param - (param % pageSize);
			if (__s2sdk_GetClientMenuOffset != nullptr) {
				try {
					offset = s2sdk::GetClientMenuOffset(slot);
				} catch (...) {
				}
			}
			const int button = param - offset + 1;
			if (button < 1 || button > pageSize) {
				return;
			}
			const MenuItem item = session.items[static_cast<std::size_t>(param)];
			// NOT g_vipPlugin.ClientSteamIdOf/ControllerHandleOf here: this runs under the menu
			// manager's lock and the slot may already belong to somebody else. The owner of THIS
			// menu is what the action must carry.
			const uint64_t steamid = session.ownerSteamId;
			const int32_t controller = session.ownerController;
			if (steamid == 0 || controller == 0 || controller == -1) {
				return;
			}
			if (g_pending_actions.size() >= static_cast<std::size_t>(kMaxPendingMenuActions)) {
				return;
			}
			PendingAction pendingAction;
			pendingAction.kind = PendingKind::Callback;
			pendingAction.slot = slot;
			pendingAction.menuId = id;
			pendingAction.steamid = steamid;
			pendingAction.controller = controller;
			pendingAction.feature = item.info;
			pendingAction.display = item.display;
			pendingAction.button = button;
			g_pending_actions.push_back(std::move(pendingAction));
			return;
		}
		case s2sdk::MenuAction::Cancel:
			// The display session is over (End follows); the id has to be destroyed or its
			// MenuData leaks. Pisex's exit/back callbacks were ignored by the VIP menu (the
			// callback only handles items < 7), so nothing is queued here.
			QueueMenuDestroy(id);
			g_menu_sessions[static_cast<std::size_t>(slot)] = MenuSession{};
			return;
		case s2sdk::MenuAction::End:
			QueueMenuDestroy(id);
			g_menu_sessions[static_cast<std::size_t>(slot)] = MenuSession{};
			return;
		}
	} catch (...) {
	}
}

} // namespace

// --- lifecycle (called by plugin.cpp) ---------------------------------------------------------

bool MenusAvailable() {
	return g_menus_available;
}

bool SetupMenus() {
	g_menu_sessions = {};
	g_pending_destroy.clear();
	g_parked_menus.clear();
	g_pending_actions.clear();
	g_reserved_menu_id = 0;
	g_has_reserved_menu = false;

	// PITFALLS C10/A7: the plugin may only create a menu (including the dummy id 0) when it can
	// also destroy it and end a display through the manager.
	g_menus_available = __s2sdk_CreateMenu != nullptr && __s2sdk_DestroyMenu != nullptr &&
						__s2sdk_DisplayMenu != nullptr && __s2sdk_AddMenuItem != nullptr &&
						__s2sdk_GetClientMenu != nullptr && __s2sdk_CancelClientMenu != nullptr &&
						__s2sdk_SetMenuType != nullptr && __s2sdk_GetMenuType != nullptr;
	if (!g_menus_available) {
		LogError("vip: the s2sdk menu bindings are unavailable (Create/Destroy/Display/AddItem/"
				 "GetClientMenu/CancelClientMenu/SetMenuType/GetMenuType) - the !vip menu stays off");
		return false;
	}

	// Burn s2sdk menu id 0: it is both a real id and the manager's "no menu" sentinel.
	bool zeroAlive = false;
	if (__s2sdk_IsValidMenu != nullptr) {
		try {
			zeroAlive = s2sdk::IsValidMenu(0);
		} catch (...) {
			zeroAlive = false;
		}
	}
	try {
		g_reserved_menu_id = s2sdk::CreateMenu(plg::string("vip:reserved-id-0"), &OnMenuHandler,
											   plg::string("chat"));
		g_has_reserved_menu = true;
		if (g_reserved_menu_id != 0 && zeroAlive) {
			LogInfo("vip: s2sdk menu id 0 already belongs to another plugin; this plugin's menus start at id " +
					std::to_string(g_reserved_menu_id));
		}
	} catch (...) {
		g_has_reserved_menu = false;
	}
	return true;
}

void ProcessMenuWork() {
	if (!g_menus_available) {
		return;
	}
	RetryParkedMenus();
	DestroyQueuedMenus();
	if (!IsGameReady()) {
		return;
	}
	while (!g_pending_actions.empty()) {
		const PendingAction action = std::move(g_pending_actions.front());
		g_pending_actions.pop_front();
		if (!SlotInRange(action.slot)) {
			continue;
		}
		// The slot may have emptied, changed hands or the same player may have reconnected with a
		// new controller entity. Both halves of the identity are revalidated right now.
		const bool samePlayer = g_vipPlugin.IsSlotHuman(action.slot) &&
								g_vipPlugin.ClientSteamIdOf(action.slot) == action.steamid &&
								g_vipPlugin.ControllerHandleOf(action.slot) == action.controller;
		if (!samePlayer) {
			// The action belongs to a menu of a previous occupant. Close exactly that stale menu
			// (never the replacement the new owner may have opened meanwhile) and drop the action.
			if (action.menuId != 0 &&
				g_menu_sessions[static_cast<std::size_t>(action.slot)].id == action.menuId) {
				CloseSlotMenu(action.slot);
			}
			continue;
		}
		switch (action.kind) {
		case PendingKind::Show:
			g_vipPlugin.ShowMenuNow(action.slot);
			break;
		case PendingKind::Callback:
			g_vipPlugin.HandleMenuCallback(action.feature.c_str(), action.display.c_str(), action.button, action.slot);
			break;
		}
	}
}

void TeardownMenusOnMapEnd() {
	g_pending_actions.clear();
	for (const s2sdk::MenuId id : CollectOwnedMenuIds()) {
		if (DestroyMenuChecked(id, /*mapReady=*/false)) {
			continue;
		}
		g_parked_menus.push_back(id);
		LogWarn("vip: could not close menu " + std::to_string(id) +
				" in the changelevel window - retried when the next map is up");
	}
}

void TeardownMenusOnUnload() {
	g_pending_actions.clear();
	const bool mapReady = IsGameReady();
	std::vector<s2sdk::MenuId> ids = CollectOwnedMenuIds();
	if (g_has_reserved_menu) {
		ids.push_back(g_reserved_menu_id);
		g_has_reserved_menu = false;
		g_reserved_menu_id = 0;
	}
	std::vector<s2sdk::MenuId> unresolved;
	for (const s2sdk::MenuId id : ids) {
		if (DestroyMenuChecked(id, mapReady)) {
			continue;
		}
		unresolved.push_back(id);
	}
	g_menu_sessions = {};
	g_pending_destroy.clear();
	g_parked_menus.clear();
	if (!unresolved.empty()) {
		std::string list;
		for (const s2sdk::MenuId id : unresolved) {
			if (!list.empty()) {
				list += ", ";
			}
			list += std::to_string(id);
		}
		LogError("vip: A7_UNRESOLVED: s2sdk refused to release menu id(s) " + list +
				 " - their handler points into this library and cannot be removed by the plugin API. This is an "
				 "s2sdk MenuManager contract violation; do not deploy until it is understood");
	}
}

void ShowVipMenu(int32_t slot) {
	g_vipPlugin.ShowMenu(slot, true);
}

} // namespace vip
