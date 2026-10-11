// Tests for plugins/vip/include/vip_api.hpp - the consumer side of the vip API (what a ported
// Pisex VIP module compiles against).
//
// The generated binding slots (__vip_*) are filled here with stubs, so the test can check the
// whole wrapper without a live plugin: the plg::string <-> std::string conversions, the
// const char* caches (translations, groups, feature strings, cookies), the variadic
// VIP_PrintToCenter and - most importantly - the static trampolines that dispatch the
// std::function callbacks (captures included) registered through VIP_RegisterFeature and the
// six event registration pairs.
//
// It also checks the "plugin not loaded" behaviour: with null binding slots every wrapper call
// has to return a harmless default instead of dereferencing a null function pointer.

#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <vip_api.hpp>

// A Plugify plugin defines this slot through PLUGIFY_PLUGIN; the consumer header only needs the
// log function, so the test provides it (null = logging stays off, the default in the modules).
namespace plg {
LogFn Log = nullptr;
} // namespace plg

// The ported modules keep the original code's unqualified spellings (VIP_INT, TOGGLABLE,
// DISABLED, ...): the consumer wrapper declares them as plain enums in namespace vip.
using namespace vip;

namespace {

int g_checks = 0;
int g_failures = 0;

#define CHECK(expr)                                                                                \
	do {                                                                                           \
		++g_checks;                                                                                \
		if (!(expr)) {                                                                             \
			++g_failures;                                                                          \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);                            \
		}                                                                                          \
	} while (false)

#define CHECK_EQ_STR(actual, expected)                                                             \
	do {                                                                                           \
		++g_checks;                                                                                \
		const std::string a_{actual};                                                              \
		const std::string e_{expected};                                                            \
		if (a_ != e_) {                                                                            \
			++g_failures;                                                                          \
			std::printf("FAIL %s:%d: got \"%s\", expected \"%s\"\n", __FILE__, __LINE__, a_.c_str(), \
						e_.c_str());                                                               \
		}                                                                                          \
	} while (false)

#define CHECK_EQ_INT(actual, expected)                                                             \
	do {                                                                                           \
		++g_checks;                                                                                \
		const long long a_ = static_cast<long long>(actual);                                       \
		const long long e_ = static_cast<long long>(expected);                                     \
		if (a_ != e_) {                                                                            \
			++g_failures;                                                                          \
			std::printf("FAIL %s:%d: got %lld, expected %lld\n", __FILE__, __LINE__, a_, e_);      \
		}                                                                                          \
	} while (false)

// --- the fake vip plugin ---------------------------------------------------------------------

// The slots carry the *generated* function-pointer types (the wrapper converts its
// std::function trampolines to those - a std::function cannot be compared, the pointers can).
vip::ItemSelectableDelegate g_select = nullptr;
vip::ItemTogglableDelegate g_toggle = nullptr;
vip::ItemDisplayDelegate g_display = nullptr;
vip::ReadyDelegate g_ready = nullptr;
vip::ClientLoadedOrDisconnectDelegate g_loaded = nullptr;
vip::ClientLoadedOrDisconnectDelegate g_disconnected = nullptr;
vip::SpawnDelegate g_spawn = nullptr;
vip::VipRemoveDelegate g_removed = nullptr;
vip::VipAddDelegate g_added = nullptr;
// The Unregister counterparts record what they were handed (the wrapper passes the same constant
// trampoline), so the test can check the pair without dereferencing a null slot.
vip::ReadyDelegate g_ready_unregistered = nullptr;
vip::ClientLoadedOrDisconnectDelegate g_loaded_unregistered = nullptr;
vip::ClientLoadedOrDisconnectDelegate g_disconnected_unregistered = nullptr;
vip::SpawnDelegate g_spawn_unregistered = nullptr;
vip::VipRemoveDelegate g_removed_unregistered = nullptr;
vip::VipAddDelegate g_added_unregistered = nullptr;

std::string g_last_center;
int g_center_slot = -1;

void ClearBindings() {
	__vip_VIP_IsVIPLoaded = nullptr;
	__vip_VIP_GetTranslate = nullptr;
	__vip_VIP_IsClientVIP = nullptr;
	__vip_VIP_GetClientAccessTime = nullptr;
	__vip_VIP_SetClientAccessTime = nullptr;
	__vip_VIP_GetClientVIPGroup = nullptr;
	__vip_VIP_SetClientVIPGroup = nullptr;
	__vip_VIP_GiveClientVIP = nullptr;
	__vip_VIP_RemoveClientVIP = nullptr;
	__vip_VIP_GetClientFeatureInt = nullptr;
	__vip_VIP_GetClientFeatureBool = nullptr;
	__vip_VIP_GetClientFeatureFloat = nullptr;
	__vip_VIP_GetClientFeatureString = nullptr;
	__vip_VIP_RegisterFeature = nullptr;
	__vip_VIP_IsValidVIPGroup = nullptr;
	__vip_VIP_PistolRound = nullptr;
	__vip_VIP_WarmupPeriod = nullptr;
	__vip_VIP_GetTotalRounds = nullptr;
	__vip_VIP_SetClientCookie = nullptr;
	__vip_VIP_GetClientCookie = nullptr;
	__vip_VIP_PrintToCenter = nullptr;
	__vip_VIP_OpenMenu = nullptr;
	__vip_VIP_OnVIPLoaded_Register = nullptr;
	__vip_VIP_OnVIPLoaded_Unregister = nullptr;
	__vip_VIP_OnClientLoaded_Register = nullptr;
	__vip_VIP_OnClientLoaded_Unregister = nullptr;
	__vip_VIP_OnClientDisconnect_Register = nullptr;
	__vip_VIP_OnClientDisconnect_Unregister = nullptr;
	__vip_VIP_OnPlayerSpawn_Register = nullptr;
	__vip_VIP_OnPlayerSpawn_Unregister = nullptr;
	__vip_VIP_OnVIPClientRemoved_Register = nullptr;
	__vip_VIP_OnVIPClientRemoved_Unregister = nullptr;
	__vip_VIP_OnVIPClientAdded_Register = nullptr;
	__vip_VIP_OnVIPClientAdded_Unregister = nullptr;

	g_select = nullptr;
	g_toggle = nullptr;
	g_display = nullptr;
	g_ready = nullptr;
	g_loaded = nullptr;
	g_disconnected = nullptr;
	g_spawn = nullptr;
	g_removed = nullptr;
	g_added = nullptr;
	g_ready_unregistered = nullptr;
	g_loaded_unregistered = nullptr;
	g_disconnected_unregistered = nullptr;
	g_spawn_unregistered = nullptr;
	g_removed_unregistered = nullptr;
	g_added_unregistered = nullptr;
}

// --- stubs -----------------------------------------------------------------------------------

bool StubIsLoaded() {
	return true;
}

plg::string StubTranslate(const plg::string& phrase) {
	if (phrase == plg::string("MenuTitle")) {
		return plg::string("VIP MENU");
	}
	if (phrase == plg::string("On")) {
		return plg::string("ON");
	}
	if (phrase == plg::string("Off")) {
		return plg::string("OFF");
	}
	return plg::string(); // unknown keys answer an empty string, like the original
}

bool StubIsClientVip(int32_t slot) {
	return slot == 3;
}

int32_t StubAccessTime(int32_t slot) {
	return slot == 3 ? -1 : 0;
}

bool StubSetAccessTime(int32_t slot, int32_t time, bool in_db) {
	return slot == 3 && time == 42 && in_db;
}

plg::string StubGroup(int32_t slot) {
	return slot == 3 ? plg::string("PROVIP") : plg::string();
}

bool StubSetGroup(int32_t slot, const plg::string& group, bool in_db) {
	return slot == 3 && group == plg::string("SKILL") && !in_db;
}

bool StubGive(int32_t slot, int32_t time, const plg::string& group, bool add_to_db) {
	return slot == 4 && time == 60 && group == plg::string("SKILL") && add_to_db;
}

bool StubRemove(int32_t slot, bool notify, bool in_db) {
	return slot == 3 && notify && !in_db;
}

int32_t StubFeatureInt(int32_t slot, const plg::string& feature) {
	return slot == 3 && feature == plg::string("jumps") ? 2 : -1;
}

bool StubFeatureBool(int32_t slot, const plg::string& feature) {
	return slot == 3 && feature == plg::string("bhop");
}

float StubFeatureFloat(int32_t slot, const plg::string& feature) {
	return slot == 3 && feature == plg::string("speed") ? 1.25f : 1.0f;
}

plg::string StubFeatureString(int32_t slot, const plg::string& feature) {
	return slot == 3 && feature == plg::string("FOV") ? plg::string("100,90,80,70,60") : plg::string();
}

// The producer side of VIP_RegisterFeature: keeps what the wrapper sent.
struct RegisteredFeature {
	std::string feature;
	vip::ValueType value_type = vip::ValueType::VIP_NULL;
	vip::FeatureType feature_type = vip::FeatureType::TOGGLABLE;
	vip::ItemSelectableDelegate select = nullptr;
	vip::ItemTogglableDelegate toggle = nullptr;
	vip::ItemDisplayDelegate display = nullptr;
};

std::vector<RegisteredFeature> g_registered;

void StubRegisterFeature(const plg::string& feature, vip::ValueType value_type, vip::FeatureType feature_type,
						 vip::ItemSelectableDelegate select, vip::ItemTogglableDelegate toggle,
						 vip::ItemDisplayDelegate display) {
	RegisteredFeature entry;
	entry.feature = std::string(std::string_view(feature));
	entry.value_type = value_type;
	entry.feature_type = feature_type;
	entry.select = select;
	entry.toggle = toggle;
	entry.display = display;
	g_registered.push_back(entry);
}

bool StubIsValidGroup(const plg::string& group) {
	return group == plg::string("PROVIP");
}

bool StubPistolRound() {
	return true;
}

bool StubWarmup() {
	return false;
}

int32_t StubTotalRounds() {
	return 7;
}

struct CookieWrite {
	int32_t slot = 0;
	std::string name;
	std::string data;
};

std::vector<CookieWrite> g_cookie_writes;
std::string g_cookie_value;

bool StubSetCookie(int32_t slot, const plg::string& name, const plg::string& data) {
	g_cookie_writes.push_back({slot, std::string(std::string_view(name)), std::string(std::string_view(data))});
	return true;
}

plg::string StubGetCookie(int32_t slot, const plg::string& name) {
	if (slot == 3 && name == plg::string("jumps")) {
		return plg::string("0");
	}
	(void)g_cookie_value;
	return plg::string("1");
}

void StubPrintToCenter(int32_t slot, const plg::string& msg) {
	g_center_slot = slot;
	g_last_center = std::string(std::string_view(msg));
}

std::vector<int32_t> g_opened_menus;

void StubOpenMenu(int32_t slot) {
	g_opened_menus.push_back(slot);
}

void InstallBindings() {
	ClearBindings();
	__vip_VIP_OnVIPLoaded_Register = [](vip::ReadyDelegate callback) { g_ready = callback; };
	__vip_VIP_OnClientLoaded_Register = [](vip::ClientLoadedOrDisconnectDelegate callback) { g_loaded = callback; };
	__vip_VIP_OnClientDisconnect_Register = [](vip::ClientLoadedOrDisconnectDelegate callback) {
		g_disconnected = callback;
	};
	__vip_VIP_OnPlayerSpawn_Register = [](vip::SpawnDelegate callback) { g_spawn = callback; };
	__vip_VIP_OnVIPClientRemoved_Register = [](vip::VipRemoveDelegate callback) { g_removed = callback; };
	__vip_VIP_OnVIPClientAdded_Register = [](vip::VipAddDelegate callback) { g_added = callback; };
	__vip_VIP_OnVIPLoaded_Unregister = [](vip::ReadyDelegate callback) { g_ready_unregistered = callback; };
	__vip_VIP_OnClientLoaded_Unregister = [](vip::ClientLoadedOrDisconnectDelegate callback) {
		g_loaded_unregistered = callback;
	};
	__vip_VIP_OnClientDisconnect_Unregister = [](vip::ClientLoadedOrDisconnectDelegate callback) {
		g_disconnected_unregistered = callback;
	};
	__vip_VIP_OnPlayerSpawn_Unregister = [](vip::SpawnDelegate callback) { g_spawn_unregistered = callback; };
	__vip_VIP_OnVIPClientRemoved_Unregister = [](vip::VipRemoveDelegate callback) { g_removed_unregistered = callback; };
	__vip_VIP_OnVIPClientAdded_Unregister = [](vip::VipAddDelegate callback) { g_added_unregistered = callback; };
	__vip_VIP_IsVIPLoaded = &StubIsLoaded;
	__vip_VIP_GetTranslate = &StubTranslate;
	__vip_VIP_IsClientVIP = &StubIsClientVip;
	__vip_VIP_GetClientAccessTime = &StubAccessTime;
	__vip_VIP_SetClientAccessTime = &StubSetAccessTime;
	__vip_VIP_GetClientVIPGroup = &StubGroup;
	__vip_VIP_SetClientVIPGroup = &StubSetGroup;
	__vip_VIP_GiveClientVIP = &StubGive;
	__vip_VIP_RemoveClientVIP = &StubRemove;
	__vip_VIP_GetClientFeatureInt = &StubFeatureInt;
	__vip_VIP_GetClientFeatureBool = &StubFeatureBool;
	__vip_VIP_GetClientFeatureFloat = &StubFeatureFloat;
	__vip_VIP_GetClientFeatureString = &StubFeatureString;
	__vip_VIP_RegisterFeature = &StubRegisterFeature;
	__vip_VIP_IsValidVIPGroup = &StubIsValidGroup;
	__vip_VIP_PistolRound = &StubPistolRound;
	__vip_VIP_WarmupPeriod = &StubWarmup;
	__vip_VIP_GetTotalRounds = &StubTotalRounds;
	__vip_VIP_SetClientCookie = &StubSetCookie;
	__vip_VIP_GetClientCookie = &StubGetCookie;
	__vip_VIP_PrintToCenter = &StubPrintToCenter;
	__vip_VIP_OpenMenu = &StubOpenMenu;
}

} // namespace

int main() {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	std::printf("vip_api_tests\n");

	vip::VipApi& api = vip::VipApi::Get();

	// --- not loaded: every call answers the neutral value -------------------------------------
	ClearBindings();
	CHECK(!api.VIP_IsVIPLoaded());
	CHECK(!api.VIP_IsClientVIP(3));
	CHECK_EQ_INT(api.VIP_GetClientAccessTime(3), -1);
	CHECK(!api.VIP_SetClientAccessTime(3, 0));
	CHECK_EQ_STR(api.VIP_GetClientVIPGroup(3), "");
	CHECK(!api.VIP_SetClientVIPGroup(3, "PROVIP"));
	CHECK(!api.VIP_GiveClientVIP(3, 60, "PROVIP"));
	CHECK(!api.VIP_RemoveClientVIP(3, true));
	CHECK_EQ_INT(api.VIP_GetClientFeatureInt(3, "jumps"), -1);
	CHECK(!api.VIP_GetClientFeatureBool(3, "bhop"));
	CHECK(api.VIP_GetClientFeatureFloat(3, "speed") == 1.0f);
	CHECK_EQ_STR(api.VIP_GetClientFeatureString(3, "FOV"), "");
	CHECK(!api.VIP_IsValidVIPGroup("PROVIP"));
	CHECK(!api.VIP_PistolRound());
	CHECK(!api.VIP_WarmupPeriod());
	CHECK_EQ_INT(api.VIP_GetTotalRounds(), 0);
	CHECK(!api.VIP_SetClientCookie(3, "bhop", "0"));
	CHECK_EQ_STR(api.VIP_GetClientCookie(3, "bhop"), "");
	CHECK(api.VIP_GetEntitySystem() == nullptr);
	CHECK_EQ_STR(api.VIP_GetTranslate("MenuTitle"), "");
	api.VIP_PrintToCenter(3, "hello %s", "world"); // must not crash
	api.VIP_OpenMenu(3);
	api.VIP_RegisterFeature("bhop", VIP_BOOL, TOGGLABLE); // must not crash
	api.VIP_OnVIPLoaded([]{});
	api.VIP_OnClientLoaded([](int, bool) {});
	api.VIP_OnClientDisconnect([](int, bool) {});
	api.VIP_OnPlayerSpawn([](int, int, bool) {});
	api.VIP_OnVIPClientRemoved([](int, int) {});
	api.VIP_OnVIPClientAdded([](int) {});

	// --- the converted surface -----------------------------------------------------------------
	InstallBindings();
	api.ReloadTranslations(); // the not-loaded section filled the cache with "" values
	CHECK(api.VIP_IsVIPLoaded());
	CHECK(api.VIP_IsClientVIP(3));
	CHECK(!api.VIP_IsClientVIP(4));
	CHECK_EQ_INT(api.VIP_GetClientAccessTime(3), -1);
	CHECK_EQ_INT(api.VIP_GetClientAccessTime(4), 0);
	CHECK(api.VIP_SetClientAccessTime(3, 42)); // the default bInDB = true reaches the core
	CHECK(!api.VIP_SetClientAccessTime(3, 43));
	CHECK_EQ_STR(api.VIP_GetClientVIPGroup(3), "PROVIP");
	CHECK_EQ_STR(api.VIP_GetClientVIPGroup(4), "");
	CHECK(api.VIP_SetClientVIPGroup(3, "SKILL", false));
	CHECK(api.VIP_GiveClientVIP(4, 60, "SKILL"));
	CHECK(api.VIP_RemoveClientVIP(3, true, false));
	CHECK_EQ_INT(api.VIP_GetClientFeatureInt(3, "jumps"), 2);
	CHECK_EQ_INT(api.VIP_GetClientFeatureInt(3, "bhop"), -1);
	CHECK(api.VIP_GetClientFeatureBool(3, "bhop"));
	CHECK(api.VIP_GetClientFeatureFloat(3, "speed") == 1.25f);
	CHECK_EQ_STR(api.VIP_GetClientFeatureString(3, "FOV"), "100,90,80,70,60");
	CHECK(api.VIP_IsValidVIPGroup("PROVIP"));
	CHECK(!api.VIP_IsValidVIPGroup("SKILL"));
	CHECK(api.VIP_PistolRound());
	CHECK(!api.VIP_WarmupPeriod());
	CHECK_EQ_INT(api.VIP_GetTotalRounds(), 7);

	// The const char* returns point into caches: the translation is cached (the stub is only
	// called once) and stays valid.
	const char* title = api.VIP_GetTranslate("MenuTitle");
	CHECK_EQ_STR(title, "VIP MENU");
	ClearBindings();
	CHECK_EQ_STR(api.VIP_GetTranslate("MenuTitle"), "VIP MENU"); // still cached
	CHECK_EQ_STR(api.VIP_GetTranslate("Nope"), "");              // unknown key -> empty
	api.ReloadTranslations();
	CHECK_EQ_STR(api.VIP_GetTranslate("MenuTitle"), "");         // cache dropped, core gone
	InstallBindings();

	// The per-slot group cache and the single feature/cookie buffers.
	const char* group = api.VIP_GetClientVIPGroup(3);
	CHECK_EQ_STR(group, "PROVIP");
	CHECK_EQ_STR(api.VIP_GetClientVIPGroup(3), "PROVIP");
	CHECK_EQ_STR(api.VIP_GetClientVIPGroup(-1), "");
	CHECK_EQ_STR(api.VIP_GetClientVIPGroup(64), "");
	const char* feature = api.VIP_GetClientFeatureString(3, "FOV");
	CHECK_EQ_STR(feature, "100,90,80,70,60");
	CHECK_EQ_STR(api.VIP_GetClientCookie(3, "jumps"), "0"); // does not clobber the feature buffer
	CHECK_EQ_STR(feature, "100,90,80,70,60");

	// Cookies: set forwards slot/name/data, get prefixes nothing (the core owns the '<group>.' key).
	g_cookie_writes.clear();
	CHECK(api.VIP_SetClientCookie(3, "bhop", "0"));
	CHECK_EQ_INT(g_cookie_writes.size(), 1);
	if (!g_cookie_writes.empty()) {
		CHECK_EQ_INT(g_cookie_writes[0].slot, 3);
		CHECK_EQ_STR(g_cookie_writes[0].name, "bhop");
		CHECK_EQ_STR(g_cookie_writes[0].data, "0");
	}

	// VIP_PrintToCenter formats like the original (256 byte buffer).
	g_last_center.clear();
	g_center_slot = -1;
	api.VIP_PrintToCenter(5, "hello %s (%d)", "world", 7);
	CHECK_EQ_INT(g_center_slot, 5);
	CHECK_EQ_STR(g_last_center, "hello world (7)");

	// --- feature registration and the trampolines ------------------------------------------------
	g_registered.clear();
	int select_calls = 0;
	int toggle_calls = 0;
	int display_calls = 0;
	api.VIP_RegisterFeature(
		"bhop", VIP_BOOL, TOGGLABLE, nullptr,
		[&toggle_calls](int slot, const char* feature, VIP_ToggleState old_status, VIP_ToggleState& new_status) {
			++toggle_calls;
			CHECK_EQ_INT(slot, 3);
			CHECK_EQ_STR(feature, "bhop");
			CHECK_EQ_INT(old_status, ENABLED);
			CHECK_EQ_INT(new_status, DISABLED);
			new_status = ENABLED; // the callback may rewrite the outcome
			return true;          // ... and veto the change
		},
		nullptr);
	api.VIP_RegisterFeature(
		"FOV", VIP_INT, SELECTABLE,
		[&select_calls](int slot, const char* feature) {
			++select_calls;
			CHECK_EQ_INT(slot, 3);
			CHECK_EQ_STR(feature, "FOV");
			return true;
		},
		nullptr,
		[&display_calls](int slot, const char* feature) {
			++display_calls;
			CHECK_EQ_INT(slot, 3);
			CHECK_EQ_STR(feature, "FOV");
			return std::string("Choose FOV");
		});

	CHECK_EQ_INT(g_registered.size(), 2);
	if (g_registered.size() == 2) {
		CHECK_EQ_STR(g_registered[0].feature, "bhop");
		CHECK(g_registered[0].value_type == vip::ValueType::VIP_BOOL);
		CHECK(g_registered[0].feature_type == vip::FeatureType::TOGGLABLE);
		CHECK(g_registered[0].select == nullptr);
		CHECK(g_registered[0].toggle != nullptr);
		CHECK(g_registered[0].display == nullptr);
		CHECK_EQ_STR(g_registered[1].feature, "FOV");
		CHECK(g_registered[1].value_type == vip::ValueType::VIP_INT);
		CHECK(g_registered[1].feature_type == vip::FeatureType::SELECTABLE);
		CHECK(g_registered[1].select != nullptr);
		CHECK(g_registered[1].toggle == nullptr);
		CHECK(g_registered[1].display != nullptr);

		// The toggle trampoline: old/new status for the callback, the rewritten status back, and
		// the veto flag as the return value.
		vip::ToggleState old_state = vip::ToggleState::ENABLED;
		vip::ToggleState new_state = vip::ToggleState::DISABLED;
		CHECK(g_registered[0].toggle(3, plg::string("bhop"), old_state, new_state));
		CHECK_EQ_INT(toggle_calls, 1);
		CHECK(new_state == vip::ToggleState::ENABLED);

		// The select and display trampolines.
		CHECK(g_registered[1].select(3, plg::string("FOV")));
		CHECK_EQ_INT(select_calls, 1);
		CHECK_EQ_STR(std::string(std::string_view(g_registered[1].display(3, plg::string("FOV")))), "Choose FOV");
		CHECK_EQ_INT(display_calls, 1);

		// An unregistered feature reaches no callback (the trampoline returns the neutral value).
		vip::ToggleState a = vip::ToggleState::DISABLED;
		vip::ToggleState b = vip::ToggleState::DISABLED;
		CHECK(!g_registered[0].toggle(3, plg::string("ghost"), a, b));
		CHECK_EQ_INT(toggle_calls, 1);
		CHECK(!g_registered[1].select(3, plg::string("ghost")));
		CHECK_EQ_INT(select_calls, 1);
	}

	// A callback that throws must not escape the trampoline.
	api.VIP_RegisterFeature("boom", VIP_BOOL, TOGGLABLE, nullptr,
							[](int, const char*, VIP_ToggleState, VIP_ToggleState&) -> bool {
								throw std::runtime_error("boom");
							},
							nullptr);
	CHECK_EQ_INT(g_registered.size(), 3);
	if (g_registered.size() == 3) {
		vip::ToggleState a = vip::ToggleState::DISABLED;
		vip::ToggleState b = vip::ToggleState::DISABLED;
		CHECK(!g_registered[2].toggle(3, plg::string("boom"), a, b)); // caught, reported as "no veto"
	}

	// --- the event registrations -----------------------------------------------------------------
	int ready_calls = 0;
	int loaded_calls = 0;
	int disconnected_calls = 0;
	int spawn_calls = 0;
	int removed_calls = 0;
	int added_calls = 0;
	api.VIP_OnVIPLoaded([&ready_calls] { ++ready_calls; });
	api.VIP_OnClientLoaded([&loaded_calls](int slot, bool is_vip) {
		++loaded_calls;
		CHECK_EQ_INT(slot, 3);
		CHECK(is_vip);
	});
	api.VIP_OnClientDisconnect([&disconnected_calls](int slot, bool is_vip) {
		++disconnected_calls;
		CHECK_EQ_INT(slot, 3);
		CHECK(!is_vip);
	});
	api.VIP_OnPlayerSpawn([&spawn_calls](int slot, int team, bool is_vip) {
		++spawn_calls;
		CHECK_EQ_INT(slot, 3);
		CHECK_EQ_INT(team, 2);
		CHECK(is_vip);
	});
	api.VIP_OnVIPClientRemoved([&removed_calls](int slot, int reason) {
		++removed_calls;
		CHECK_EQ_INT(slot, 3);
		CHECK_EQ_INT(reason, 1);
	});
	api.VIP_OnVIPClientAdded([&added_calls](int slot) {
		++added_calls;
		CHECK_EQ_INT(slot, 3);
	});
	CHECK(g_ready != nullptr);
	CHECK(g_loaded != nullptr);
	CHECK(g_disconnected != nullptr);
	CHECK(g_spawn != nullptr);
	CHECK(g_removed != nullptr);
	CHECK(g_added != nullptr);
	g_ready();
	g_loaded(3, true);
	g_disconnected(3, false);
	g_spawn(3, 2, true);
	g_removed(3, 1);
	g_added(3);
	CHECK_EQ_INT(ready_calls, 1);
	CHECK_EQ_INT(loaded_calls, 1);
	CHECK_EQ_INT(disconnected_calls, 1);
	CHECK_EQ_INT(spawn_calls, 1);
	CHECK_EQ_INT(removed_calls, 1);
	CHECK_EQ_INT(added_calls, 1);

	// Re-registering replaces the callback and passes the same (constant) trampoline.
	vip::ClientLoadedOrDisconnectDelegate first = g_loaded;
	api.VIP_OnClientLoaded([&loaded_calls](int, bool) { ++loaded_calls; });
	CHECK(g_loaded == first);
	g_loaded(3, true);
	CHECK_EQ_INT(loaded_calls, 2);

	// Unregistering hands the same (constant) trampoline to the server, drops the callback, and
	// leaves the registration otherwise alone.
	const vip::ClientLoadedOrDisconnectDelegate loaded_slot = g_loaded;
	api.VIP_OnClientLoadedUnregister();
	CHECK(g_loaded == loaded_slot);
	CHECK(g_loaded_unregistered == loaded_slot);
	g_loaded(3, true);
	CHECK_EQ_INT(loaded_calls, 2); // no callback any more
	vip::ReadyDelegate ready_slot = g_ready;
	api.VIP_OnVIPLoadedUnregister();
	CHECK(g_ready == ready_slot);
	CHECK(g_ready_unregistered == ready_slot);
	g_ready();
	CHECK_EQ_INT(ready_calls, 1); // no callback any more

	// A throwing event callback must not escape either.
	api.VIP_OnVIPClientAdded([](int) { throw std::runtime_error("boom"); });
	g_added(3); // caught inside the trampoline

	// --- output / menu plumb-through ----------------------------------------------------------------
	g_opened_menus.clear();
	api.VIP_OpenMenu(3);
	CHECK_EQ_INT(g_opened_menus.size(), 1);
	ClearBindings();
	api.VIP_OpenMenu(3); // not loaded: no crash
	CHECK_EQ_INT(g_opened_menus.size(), 1);

	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
