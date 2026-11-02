// vip_runtime_tests — the plugin-level test of the pure-s2sdk vip port (PLUGIN_STANDARD §7: no
// pisexcompat, no plgdb, no wrapper library; hudmenu/permissions are optional plugin APIs).
//
// The plugin TU is compiled into this test (`#include "../src/plugin.cpp"`, with src/commands.cpp
// and src/menus.cpp linked), so the real lifecycle, map gate, config loading, listeners, commands,
// events, spawn queue and the native menu lifecycle run; every __s2sdk_* / __permissions_* /
// __hudmenu_* binding slot is a recording fake and the files the plugin reads come from a
// temporary <configs>/<data> root + working directory.
//
// Covered:
//   1. startup: OnPluginStart answers Ok (never Failed, A1), the map/client listener pairs are
//      registered, the two Pre event hooks, the eight commands (client and server flags), the
//      permissions OnUserLoaded pair and the reserved s2sdk menu id 0; no s2sdk timer is created;
//   2. groups.ini / vip.phrases.txt / databases.cfg loading through the production KeyValues
//      layout, the first-existing-file priority (the raw server path is authoritative, then
//      <configs>/vip/, translations and data), the SQLite fallback in <data>/vip/, and the
//      vip_users row written by vip_give;
//   3. PITFALLS B1/B2: no client/entity/schema call before the first map or through a
//      changelevel; the mid-map latch opens only with all four map bindings, no OnMapEnd and a
//      real GetCurrentMap() name ("<empty>" does not open it); OnMapEnd closes the gate;
//   4. C1/C2: negative controller handles are valid, 0/-1 are not; bots/SourceTV/not-in-game
//      slots are refused; the event slot comes from GetEventPlayerSlot and the spawn action
//      revalidates SteamID64 + controller handle before it fires;
//   5. C11: the server-console commands accept strictly caller == -1 (-2 is rejected), the chat
//      spelling "vip" opens the menu and "!mm_vip" stays a normal chat line;
//   6. C10/A7: the menu lifecycle (create/display/select/toggle/reopen), the exact menu-type
//      table through the optional hudmenu choice, the SteamID64+controller revalidation of a
//      queued selection, CancelClientMenu(Disconnect)+GetClientMenu+SetMenuType/GetMenuType before
//      DestroyMenu on map end/unload, and the explicit A7_UNRESOLVED report when s2sdk refuses;
//   7. A7 pairs: a missing OnMapEnd_Unregister keeps the map gate shut and a missing permissions
//      unregister keeps the cookie listener off; OnPluginEnd removes commands (RemoveCommand for
//      created names, RemoveCommandListener for pre-existing ones), unhooks the events, unregisters
//      every listener and destroys every menu.
//
// No pisexcompat, no Plugify host, no engine. Run by ../test.sh
// (build/tests/vip_runtime_tests).

#include "../src/plugin.cpp"

#include <hudmenu/hudmenu.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <unistd.h>

namespace {

// ---------------------------------------------------------------------------------------------
// checks
// ---------------------------------------------------------------------------------------------

int gChecks = 0;
int gFailures = 0;

#define CHECK(expr)                                                                                \
	do {                                                                                           \
		++gChecks;                                                                                 \
		if (!(expr)) {                                                                             \
			++gFailures;                                                                           \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);                            \
		}                                                                                          \
	} while (false)

#define CHECK_EQ_INT(actual, expected)                                                             \
	do {                                                                                           \
		++gChecks;                                                                                 \
		const long long a_ = static_cast<long long>(actual);                                       \
		const long long e_ = static_cast<long long>(expected);                                     \
		if (a_ != e_) {                                                                            \
			++gFailures;                                                                           \
			std::printf("FAIL %s:%d: got %lld, expected %lld\n", __FILE__, __LINE__, a_, e_);      \
		}                                                                                          \
	} while (false)

#define CHECK_EQ_STR(actual, expected)                                                             \
	do {                                                                                           \
		++gChecks;                                                                                 \
		const std::string a_{actual};                                                              \
		const std::string e_{expected};                                                            \
		if (a_ != e_) {                                                                            \
			++gFailures;                                                                           \
			std::printf("FAIL %s:%d: got \"%s\", expected \"%s\"\n", __FILE__, __LINE__,           \
						a_.c_str(), e_.c_str());                                                   \
		}                                                                                          \
	} while (false)

void Section(const char* name) {
	std::printf("-- %s\n", name);
}

// ---------------------------------------------------------------------------------------------
// log capture
// ---------------------------------------------------------------------------------------------

std::vector<std::string> gInfoLogs;
std::vector<std::string> gWarnLogs;
std::vector<std::string> gErrorLogs;

bool AnyErrorContains(std::string_view needle) {
	for (const std::string& message : gErrorLogs) {
		if (message.find(needle) != std::string::npos) {
			return true;
		}
	}
	return false;
}

bool AnyLogContains(const std::vector<std::string>& logs, std::string_view needle) {
	for (const std::string& message : logs) {
		if (message.find(needle) != std::string::npos) {
			return true;
		}
	}
	return false;
}

void FakeLog(std::string_view message, plg::Severity severity, const plg::source_location&) {
	std::string text(message);
	if (severity == plg::Severity::Error || severity == plg::Severity::Fatal) {
		gErrorLogs.push_back(std::move(text));
	} else if (severity == plg::Severity::Warning) {
		gWarnLogs.push_back(std::move(text));
	} else {
		gInfoLogs.push_back(std::move(text));
	}
}

// ---------------------------------------------------------------------------------------------
// temporary <configs>/<data> tree
// ---------------------------------------------------------------------------------------------

std::filesystem::path gRoot;
std::filesystem::path gConfigs;
std::filesystem::path gData;
std::filesystem::path gPreviousCwd;

plg::string FakeGetConfigsDir() {
	return plg::string(gConfigs.string());
}

plg::string FakeGetDataDir() {
	return plg::string(gData.string());
}

void WriteFileText(const std::filesystem::path& path, std::string_view text) {
	std::error_code ec;
	std::filesystem::create_directories(path.parent_path(), ec);
	std::ofstream file(path, std::ios::binary | std::ios::trunc);
	file.write(text.data(), static_cast<std::streamsize>(text.size()));
	CHECK(file.good());
}

constexpr std::string_view kGroupsIni = R"KV(
"VIP"
{
	"server_id"	"7"

	"Gold"
	{
		"Health"	"150"
		"Speed"		"1.1"
		"legacy"	"Silver"
	}

	"Silver"
	{
		"Speed"		"1.05"
		"Armor"		"100"
	}
}
)KV";

constexpr std::string_view kPhrases = R"KV(
"Phrases"
{
	"language"	"ru"
	"Welcome"	{ "ru" "Добро пожаловать, %s! (%s)" "en" "Welcome, %s! (%s)" }
	"WelcomePerm"	{ "ru" "Добро пожаловать навсегда, %s!" "en" "Welcome forever, %s!" }
	"VIPExpired1"	{ "ru" "{RED}Ваш VIP истёк!" "en" "{RED}Your VIP expired!" }
	"VIPExpired2"	{ "ru" "Строка 2" "en" "Line 2" }
	"VIPExpired3"	{ "ru" "Строка 3" "en" "Line 3" }
	"NotAccess"	{ "ru" "{RED}Нет доступа" "en" "{RED}Not access" }
	"MenuTitle"	{ "ru" "Меню VIP" "en" "VIP menu" }
	"On"	{ "ru" "Вкл" "en" "On" }
	"Off"	{ "ru" "Выкл" "en" "Off" }
	"Health"	{ "ru" "Здоровье" "en" "Health" }
	"Speed"	{ "ru" "Скорость" "en" "Speed" }
}
)KV";

void WriteProductionConfigs() {
	WriteFileText(gConfigs / "vip" / "groups.ini", kGroupsIni);
	WriteFileText(gConfigs / "vip" / "translations" / "vip.phrases.txt", kPhrases);
	WriteFileText(gConfigs / "core.cfg", "\"Core\"\n{\n\t\"ServerLang\"\t\"russian\"\n}\n");
	// No databases.cfg: the plugin must fall back to SQLite in <data>/vip/ (fail soft).
	std::error_code ec;
	std::filesystem::create_directories(gData / "vip", ec);
}

void ResetTempTree() {
	std::error_code ec;
	std::filesystem::remove_all(gRoot, ec);
	std::filesystem::create_directories(gConfigs, ec);
	std::filesystem::create_directories(gData, ec);
	std::filesystem::current_path(gRoot, ec);
	CHECK(!ec);
	WriteProductionConfigs();
}

// ---------------------------------------------------------------------------------------------
// fake players
// ---------------------------------------------------------------------------------------------

constexpr int32_t kFakePlayers = 64;
constexpr uint64_t kSteamId = (1ULL << 32) | 42ULL; // account id 42
constexpr int32_t kValidNegativeHandle = -1879048192; // 0x90000000: serial bit 31 set (C1)
constexpr int32_t kValidPositiveHandle = 0x001234;

struct FakePlayer {
	bool connected = false;
	bool inGame = false;
	bool authorized = false;
	bool fake = false;
	bool sourceTV = false;
	bool alive = false;
	uint64_t steamid = 0;
	std::string name;
	int32_t controllerHandle = 0;
	void* controllerPointer = nullptr;
	int32_t controllerIndex = 0;
	int32_t pawnIndex = 0;
	void* pawnPointer = nullptr;
	int32_t team = 0;
};

std::array<FakePlayer, kFakePlayers> gPlayers{};
long long gFakeClientCalls = 0;
long long gFakeSchemaCalls = 0;
long long gFakeTimerCalls = 0;
std::string gFakeMapName;

void SetHumanPlayer(int32_t slot, uint64_t steamid, std::string name, int32_t handle, int32_t team) {
	FakePlayer& player = gPlayers[static_cast<std::size_t>(slot)];
	player.connected = true;
	player.inGame = true;
	player.authorized = true;
	player.fake = false;
	player.sourceTV = false;
	player.alive = true;
	player.steamid = steamid;
	player.name = std::move(name);
	player.controllerHandle = handle;
	player.controllerPointer = reinterpret_cast<void*>(static_cast<uintptr_t>(0x1000 + slot * 0x10));
	player.controllerIndex = slot + 1;
	player.pawnPointer = reinterpret_cast<void*>(static_cast<uintptr_t>(0x2000 + slot * 0x10));
	player.pawnIndex = 100 + slot;
	player.team = team;
}

int32_t FakeEntityIndex(void* pointer) {
	if (pointer == nullptr) {
		return 0;
	}
	for (int32_t slot = 0; slot < kFakePlayers; ++slot) {
		const FakePlayer& player = gPlayers[static_cast<std::size_t>(slot)];
		if (pointer == player.controllerPointer) {
			return player.controllerIndex;
		}
		if (pointer == player.pawnPointer) {
			return player.pawnIndex;
		}
	}
	return 0;
}

// The game rules buffer the schema fakes read: the offsets below are the fake "schema".
constexpr int32_t kOffsetWarmup = 0;
constexpr int32_t kOffsetTotalRounds = 4;
constexpr int32_t kOffsetOvertime = 8;
constexpr int32_t kOffsetSwitching = 12;
constexpr int32_t kOffsetRestart = 13;
constexpr int32_t kOffsetTeam = 40;

alignas(8) std::array<unsigned char, 64> gGameRules{};

int32_t& GameRulesInt(int32_t offset) {
	return *reinterpret_cast<int32_t*>(gGameRules.data() + offset);
}

// ---------------------------------------------------------------------------------------------
// fake s2sdk bindings
// ---------------------------------------------------------------------------------------------

plg::string FakeGetCurrentMap() {
	return plg::string(gFakeMapName);
}

bool FakeIsClientConnected(int32_t slot) {
	++gFakeClientCalls;
	return slot >= 0 && slot < kFakePlayers && gPlayers[static_cast<std::size_t>(slot)].connected;
}

bool FakeIsClientInGame(int32_t slot) {
	++gFakeClientCalls;
	return slot >= 0 && slot < kFakePlayers && gPlayers[static_cast<std::size_t>(slot)].inGame;
}

bool FakeIsClientSourceTV(int32_t slot) {
	++gFakeClientCalls;
	return slot >= 0 && slot < kFakePlayers && gPlayers[static_cast<std::size_t>(slot)].sourceTV;
}

bool FakeIsFakeClient(int32_t slot) {
	++gFakeClientCalls;
	return slot >= 0 && slot < kFakePlayers && gPlayers[static_cast<std::size_t>(slot)].fake;
}

bool FakeIsClientAuthorized(int32_t slot) {
	++gFakeClientCalls;
	return slot >= 0 && slot < kFakePlayers && gPlayers[static_cast<std::size_t>(slot)].authorized;
}

bool FakeIsClientAlive(int32_t slot) {
	++gFakeClientCalls;
	return slot >= 0 && slot < kFakePlayers && gPlayers[static_cast<std::size_t>(slot)].alive;
}

uint64_t FakeGetClientSteamID64(int32_t slot) {
	++gFakeClientCalls;
	return slot >= 0 && slot < kFakePlayers ? gPlayers[static_cast<std::size_t>(slot)].steamid : 0;
}

plg::string FakeGetClientName(int32_t slot) {
	++gFakeClientCalls;
	return plg::string(slot >= 0 && slot < kFakePlayers ? gPlayers[static_cast<std::size_t>(slot)].name : "");
}

void* FakeGetClientPawn(int32_t slot) {
	++gFakeClientCalls;
	return slot >= 0 && slot < kFakePlayers ? gPlayers[static_cast<std::size_t>(slot)].pawnPointer : nullptr;
}

void* FakePlayerSlotToEntPointer(int32_t slot) {
	++gFakeClientCalls;
	return slot >= 0 && slot < kFakePlayers ? gPlayers[static_cast<std::size_t>(slot)].controllerPointer : nullptr;
}

int32_t FakePlayerSlotToEntHandle(int32_t slot) {
	++gFakeClientCalls;
	return slot >= 0 && slot < kFakePlayers ? gPlayers[static_cast<std::size_t>(slot)].controllerHandle : 0;
}

int32_t FakeEntPointerToEntIndex(void* pointer) {
	++gFakeClientCalls;
	return FakeEntityIndex(pointer);
}

bool FakeIsValidEntPointer(void* pointer) {
	return pointer != nullptr;
}

bool FakeIsValidEntHandle(int32_t handle) {
	// C1: the only invalid values are 0 and -1; a negative handle may be real.
	return handle != 0 && handle != -1;
}

int64_t FakeGetEntData(int32_t handle, int32_t offset, int32_t /*size*/) {
	++gFakeClientCalls;
	if (offset != kOffsetTeam) {
		return 0;
	}
	for (int32_t slot = 0; slot < kFakePlayers; ++slot) {
		if (gPlayers[static_cast<std::size_t>(slot)].controllerHandle == handle) {
			return gPlayers[static_cast<std::size_t>(slot)].team;
		}
	}
	return 0;
}

int32_t FakeGetSchemaOffset(const plg::string& className, const plg::string& fieldName) {
	++gFakeSchemaCalls;
	const std::string cls(className.data(), className.size());
	const std::string field(fieldName.data(), fieldName.size());
	if (cls == "CCSGameRules") {
		if (field == "m_bWarmupPeriod") return kOffsetWarmup;
		if (field == "m_totalRoundsPlayed") return kOffsetTotalRounds;
		if (field == "m_nOvertimePlaying") return kOffsetOvertime;
		if (field == "m_bSwitchingTeamsAtRoundReset") return kOffsetSwitching;
		if (field == "m_bGameRestart") return kOffsetRestart;
		return -1;
	}
	if (cls == "CBaseEntity" && field == "m_iTeamNum") {
		return kOffsetTeam;
	}
	return -1;
}

void* FakeGetGameRules() {
	return gGameRules.data();
}

int32_t FakeGetGameTotalRoundsPlayed() {
	return GameRulesInt(kOffsetTotalRounds);
}

plg::string FakeGetServerLanguage() {
	return plg::string("english");
}

// Output.
struct ChatLineRecord {
	int32_t slot = -1;
	std::string text;
};

std::vector<ChatLineRecord> gChatLines;
std::vector<std::string> gServerLines;
std::vector<ChatLineRecord> gCenterLines;

void FakePrintToChat(int32_t slot, const plg::string& message) {
	gChatLines.push_back(ChatLineRecord{slot, std::string(message.data(), message.size())});
}

void FakePrintToServer(const plg::string& message) {
	gServerLines.push_back(std::string(message.data(), message.size()));
}

void FakePrintCenterText(int32_t slot, const plg::string& message) {
	gCenterLines.push_back(ChatLineRecord{slot, std::string(message.data(), message.size())});
}

// Commands.
struct FakeCommand {
	std::string name;
	std::string description;
	s2sdk::ConVarFlag flags = s2sdk::ConVarFlag::None;
	s2sdk::ConCommandCallback callback = nullptr;
	bool created = false;
};

std::map<std::string, FakeCommand> gFakeCommands;
std::vector<std::string> gFakeRemovedCommands;
std::vector<std::string> gFakeRemovedCommandListeners;
std::set<std::string> gFakePreexistingCommands;
int gFakeAddCommandListenerCalls = 0;

bool FakeAddConsoleCommand(const plg::string& name, const plg::string& description, s2sdk::ConVarFlag flags,
						   s2sdk::ConCommandCallback callback, s2sdk::HookMode /*mode*/) {
	const std::string key(name.data(), name.size());
	FakeCommand command;
	command.name = key;
	command.description = std::string(description.data(), description.size());
	command.flags = flags;
	command.callback = callback;
	command.created = gFakePreexistingCommands.find(key) == gFakePreexistingCommands.end();
	gFakeCommands[key] = std::move(command);
	return gFakeCommands[key].created;
}

bool FakeRemoveCommand(const plg::string& name, s2sdk::ConCommandCallback /*callback*/) {
	const std::string key(name.data(), name.size());
	gFakeRemovedCommands.push_back(key);
	return gFakeCommands.erase(key) > 0;
}

bool FakeAddCommandListener(const plg::string& /*name*/, s2sdk::ConCommandCallback /*callback*/, s2sdk::HookMode) {
	++gFakeAddCommandListenerCalls;
	return true;
}

bool FakeRemoveCommandListener(const plg::string& name, s2sdk::ConCommandCallback /*callback*/, s2sdk::HookMode) {
	gFakeRemovedCommandListeners.push_back(std::string(name.data(), name.size()));
	return true;
}

// Events.
struct FakeEventHook {
	std::string name;
	s2sdk::EventCallback callback = nullptr;
	s2sdk::HookMode mode = s2sdk::HookMode::Pre;
};

std::vector<FakeEventHook> gFakeEventHooks;
std::vector<std::string> gFakeUnhookedEvents;

s2sdk::EventHookError FakeHookEvent(const plg::string& name, s2sdk::EventCallback callback, s2sdk::HookMode mode) {
	gFakeEventHooks.push_back(FakeEventHook{std::string(name.data(), name.size()), callback, mode});
	return s2sdk::EventHookError::Okay;
}

s2sdk::EventHookError FakeUnhookEvent(const plg::string& name, s2sdk::EventCallback /*callback*/, s2sdk::HookMode) {
	gFakeUnhookedEvents.push_back(std::string(name.data(), name.size()));
	return s2sdk::EventHookError::Okay;
}

struct FakeEvent {
	int32_t slot = -1;
};

int32_t FakeGetEventPlayerSlot(void* event, const plg::string& /*key*/) {
	if (event == nullptr) {
		return -1;
	}
	return static_cast<FakeEvent*>(event)->slot;
}

s2sdk::EventCallback FakeEventCallback(const char* name) {
	for (const FakeEventHook& hook : gFakeEventHooks) {
		if (hook.name == name) {
			return hook.callback;
		}
	}
	return nullptr;
}

s2sdk::ResultType FireGameEvent(const char* name, FakeEvent& event) {
	s2sdk::EventCallback callback = FakeEventCallback(name);
	if (callback == nullptr) {
		return s2sdk::ResultType::Continue;
	}
	return callback(plg::string(name), &event, false);
}

// Map/client listeners.
s2sdk::OnMapStartCallback gFakeOnMapStart = nullptr;
s2sdk::OnMapEndCallback gFakeOnMapEnd = nullptr;
s2sdk::OnClientAuthenticatedCallback gFakeOnClientAuthenticated = nullptr;
s2sdk::OnClientPutInServerCallback gFakeOnClientPutInServer = nullptr;
s2sdk::OnClientDisconnectCallback gFakeOnClientDisconnect = nullptr;
int gFakeMapStartRegistrations = 0;
int gFakeMapEndRegistrations = 0;
int gFakeMapStartUnregistrations = 0;
int gFakeMapEndUnregistrations = 0;
int gFakeClientAuthRegistrations = 0;
int gFakeClientAuthUnregistrations = 0;
int gFakeClientPutInServerRegistrations = 0;
int gFakeClientPutInServerUnregistrations = 0;
int gFakeClientDisconnectRegistrations = 0;
int gFakeClientDisconnectUnregistrations = 0;

void FakeOnMapStartRegister(s2sdk::OnMapStartCallback callback) {
	++gFakeMapStartRegistrations;
	gFakeOnMapStart = callback;
}

void FakeOnMapStartUnregister(s2sdk::OnMapStartCallback /*callback*/) {
	++gFakeMapStartUnregistrations;
	gFakeOnMapStart = nullptr;
}

void FakeOnMapEndRegister(s2sdk::OnMapEndCallback callback) {
	++gFakeMapEndRegistrations;
	gFakeOnMapEnd = callback;
}

void FakeOnMapEndUnregister(s2sdk::OnMapEndCallback /*callback*/) {
	++gFakeMapEndUnregistrations;
	gFakeOnMapEnd = nullptr;
}

void FakeOnClientAuthenticatedRegister(s2sdk::OnClientAuthenticatedCallback callback) {
	++gFakeClientAuthRegistrations;
	gFakeOnClientAuthenticated = callback;
}

void FakeOnClientAuthenticatedUnregister(s2sdk::OnClientAuthenticatedCallback /*callback*/) {
	++gFakeClientAuthUnregistrations;
	gFakeOnClientAuthenticated = nullptr;
}

void FakeOnClientPutInServerRegister(s2sdk::OnClientPutInServerCallback callback) {
	++gFakeClientPutInServerRegistrations;
	gFakeOnClientPutInServer = callback;
}

void FakeOnClientPutInServerUnregister(s2sdk::OnClientPutInServerCallback /*callback*/) {
	++gFakeClientPutInServerUnregistrations;
	gFakeOnClientPutInServer = nullptr;
}

void FakeOnClientDisconnectRegister(s2sdk::OnClientDisconnectCallback callback) {
	++gFakeClientDisconnectRegistrations;
	gFakeOnClientDisconnect = callback;
}

void FakeOnClientDisconnectUnregister(s2sdk::OnClientDisconnectCallback /*callback*/) {
	++gFakeClientDisconnectUnregistrations;
	gFakeOnClientDisconnect = nullptr;
}

// Timers: the plugin must never create one (C9/B4).
s2sdk::TimerId FakeCreateTimer(double /*delay*/, s2sdk::TimerCallback /*callback*/, s2sdk::TimerFlag /*flags*/,
							   const plg::vector<plg::any>& /*userData*/) {
	++gFakeTimerCalls;
	return 0;
}

// Menus.
struct FakeMenuItem {
	std::string info;
	std::string display;
};

struct FakeMenu {
	std::string title;
	std::string type;
	s2sdk::MenuHandlerCallback handler = nullptr;
	std::vector<FakeMenuItem> items;
};

std::map<s2sdk::MenuId, FakeMenu> gFakeMenus;
s2sdk::MenuId gFakeNextMenuId = 0;
std::array<s2sdk::MenuId, kFakePlayers> gFakeClientMenu{};
std::set<std::string> gFakeMenuTypes;
int gFakeCreateMenuCalls = 0;
int gFakeDestroyMenuCalls = 0;
int gFakeCancelClientMenuCalls = 0;
std::vector<std::pair<s2sdk::MenuId, s2sdk::MenuCancelReason>> gFakeCancels;
std::vector<std::pair<s2sdk::MenuId, std::string>> gFakeMenuTypeSets;
bool gFakeCancelAnswer = true; // false simulates the documented manager contract violation
bool gFakeDestroyRefuses = false;
std::string gFakeHudmenuChoice = "hud";

void ResetMenuTypes() {
	gFakeMenuTypes = {"chat", "console", "centerhtml", "button", "hud"};
}

s2sdk::MenuId FakeCreateMenu(const plg::string& title, s2sdk::MenuHandlerCallback handler, const plg::string& menuType) {
	++gFakeCreateMenuCalls;
	const s2sdk::MenuId id = gFakeNextMenuId++;
	FakeMenu menu;
	menu.title = std::string(title.data(), title.size());
	menu.type = std::string(menuType.data(), menuType.size());
	menu.handler = handler;
	gFakeMenus[id] = std::move(menu);
	return id;
}

bool FakeDestroyMenu(s2sdk::MenuId id) {
	if (gFakeDestroyRefuses) {
		return false;
	}
	auto found = gFakeMenus.find(id);
	if (found == gFakeMenus.end()) {
		return false;
	}
	s2sdk::MenuHandlerCallback handler = found->second.handler;
	for (int32_t slot = 0; slot < kFakePlayers; ++slot) {
		if (gFakeClientMenu[static_cast<std::size_t>(slot)] != id) {
			continue;
		}
		gFakeClientMenu[static_cast<std::size_t>(slot)] = 0;
		if (handler != nullptr) {
			handler(id, s2sdk::MenuAction::Cancel, slot, static_cast<int32_t>(s2sdk::MenuCancelReason::Destroyed));
			handler(id, s2sdk::MenuAction::End, slot, 0);
		}
	}
	gFakeMenus.erase(found);
	++gFakeDestroyMenuCalls;
	return true;
}

bool FakeIsValidMenu(s2sdk::MenuId id) {
	return gFakeMenus.find(id) != gFakeMenus.end();
}

int32_t FakeAddMenuItem(s2sdk::MenuId id, const plg::string& info, const plg::string& display,
						s2sdk::MenuItemStyle /*style*/) {
	auto found = gFakeMenus.find(id);
	if (found == gFakeMenus.end()) {
		return -1;
	}
	found->second.items.push_back(FakeMenuItem{std::string(info.data(), info.size()),
											   std::string(display.data(), display.size())});
	return static_cast<int32_t>(found->second.items.size() - 1);
}

bool FakeDisplayMenu(s2sdk::MenuId id, int32_t slot, int32_t /*time*/) {
	auto found = gFakeMenus.find(id);
	if (found == gFakeMenus.end() || slot < 0 || slot >= kFakePlayers) {
		return false;
	}
	gFakeClientMenu[static_cast<std::size_t>(slot)] = id;
	if (found->second.handler != nullptr) {
		found->second.handler(id, s2sdk::MenuAction::Start, slot, 0);
	}
	return true;
}

s2sdk::MenuId FakeGetClientMenu(int32_t slot) {
	return slot >= 0 && slot < kFakePlayers ? gFakeClientMenu[static_cast<std::size_t>(slot)] : 0;
}

bool FakeCancelClientMenu(int32_t slot, s2sdk::MenuCancelReason reason) {
	++gFakeCancelClientMenuCalls;
	if (slot < 0 || slot >= kFakePlayers) {
		return false;
	}
	const s2sdk::MenuId id = gFakeClientMenu[static_cast<std::size_t>(slot)];
	if (id == 0) {
		return false;
	}
	gFakeCancels.emplace_back(id, reason);
	if (!gFakeCancelAnswer) {
		return false; // contract violation: the manager says "no display" but keeps it
	}
	gFakeClientMenu[static_cast<std::size_t>(slot)] = 0;
	auto found = gFakeMenus.find(id);
	if (found != gFakeMenus.end() && found->second.handler != nullptr) {
		found->second.handler(id, s2sdk::MenuAction::Cancel, slot, static_cast<int32_t>(reason));
		found->second.handler(id, s2sdk::MenuAction::End, slot, 0);
	}
	return true;
}

int32_t FakeGetClientMenuOffset(int32_t /*slot*/) {
	return 0;
}

bool FakeSetMenuType(s2sdk::MenuId id, const plg::string& typeName) {
	auto found = gFakeMenus.find(id);
	if (found == gFakeMenus.end()) {
		return false;
	}
	const std::string type(typeName.data(), typeName.size());
	if (gFakeMenuTypes.find(type) == gFakeMenuTypes.end()) {
		return false;
	}
	found->second.type = type;
	gFakeMenuTypeSets.emplace_back(id, type);
	return true;
}

plg::string FakeGetMenuType(s2sdk::MenuId id) {
	auto found = gFakeMenus.find(id);
	return found == gFakeMenus.end() ? plg::string() : plg::string(found->second.type);
}

bool FakeIsMenuTypeRegistered(const plg::string& typeName) {
	return gFakeMenuTypes.find(std::string(typeName.data(), typeName.size())) != gFakeMenuTypes.end();
}

bool FakeSetMenuPagination(s2sdk::MenuId id, int32_t /*itemsPerPage*/) {
	return gFakeMenus.find(id) != gFakeMenus.end();
}

bool FakeSetMenuExitButton(s2sdk::MenuId id, bool /*enabled*/) {
	return gFakeMenus.find(id) != gFakeMenus.end();
}

bool FakeSetMenuExitBackButton(s2sdk::MenuId id, bool /*enabled*/) {
	return gFakeMenus.find(id) != gFakeMenus.end();
}

bool FakeSetMenuCloseOnSelect(s2sdk::MenuId id, bool /*enabled*/) {
	return gFakeMenus.find(id) != gFakeMenus.end();
}

// hudmenu (optional dependency).
plg::string FakeGetClientMenuType(int32_t /*slot*/) {
	return plg::string(gFakeHudmenuChoice);
}

bool FakeSetClientMenuType(int32_t /*slot*/, const plg::string& /*type*/) {
	return true;
}

// permissions (optional dependency).
std::map<std::string, plg::any> gFakeCookies;
std::vector<std::pair<uint64_t, std::string>> gFakeCookieWrites;
int gFakeUserLoadedRegistrations = 0;
int gFakeUserLoadedUnregistrations = 0;
permissions::UserLoadedCallback gFakeUserLoaded = nullptr;

std::string CookieKey(uint64_t steamid, const std::string& name) {
	return std::to_string(steamid) + "|" + name;
}

permissions::Status FakeGetCookie(uint64_t targetId, const plg::string& name, plg::any& value) {
	const std::string key(name.data(), name.size());
	auto found = gFakeCookies.find(CookieKey(targetId, key));
	if (found == gFakeCookies.end()) {
		return permissions::Status::CookieNotFound;
	}
	value = found->second;
	return permissions::Status::Success;
}

permissions::Status FakeSetCookie(int64_t /*pluginId*/, uint64_t targetId, const plg::string& name,
								  const plg::any& cookie, bool /*dontBroadcast*/) {
	const std::string key(name.data(), name.size());
	gFakeCookies[CookieKey(targetId, key)] = cookie;
	gFakeCookieWrites.emplace_back(targetId, key);
	return permissions::Status::Success;
}

permissions::Status FakeOnUserLoadedRegister(permissions::UserLoadedCallback callback) {
	++gFakeUserLoadedRegistrations;
	gFakeUserLoaded = callback;
	return permissions::Status::Success;
}

permissions::Status FakeOnUserLoadedUnregister(permissions::UserLoadedCallback /*callback*/) {
	++gFakeUserLoadedUnregistrations;
	gFakeUserLoaded = nullptr;
	return permissions::Status::Success;
}

// The plugin id the SetCookie call passes (0 when the plugify binding is missing).
plg::uuid FakeGetId(plg::hndl /*plugin*/) {
	return 42;
}

// ---------------------------------------------------------------------------------------------
// installing / resetting the fakes
// ---------------------------------------------------------------------------------------------

void ResetFakes() {
	gInfoLogs.clear();
	gWarnLogs.clear();
	gErrorLogs.clear();
	gPlayers = {};
	gFakeClientCalls = 0;
	gFakeSchemaCalls = 0;
	gFakeTimerCalls = 0;
	gFakeMapName.clear();
	gChatLines.clear();
	gServerLines.clear();
	gCenterLines.clear();
	gFakeCommands.clear();
	gFakeRemovedCommands.clear();
	gFakeRemovedCommandListeners.clear();
	gFakePreexistingCommands.clear();
	gFakeAddCommandListenerCalls = 0;
	gFakeEventHooks.clear();
	gFakeUnhookedEvents.clear();
	gFakeOnMapStart = nullptr;
	gFakeOnMapEnd = nullptr;
	gFakeOnClientAuthenticated = nullptr;
	gFakeOnClientPutInServer = nullptr;
	gFakeOnClientDisconnect = nullptr;
	gFakeMapStartRegistrations = 0;
	gFakeMapEndRegistrations = 0;
	gFakeMapStartUnregistrations = 0;
	gFakeMapEndUnregistrations = 0;
	gFakeClientAuthRegistrations = 0;
	gFakeClientAuthUnregistrations = 0;
	gFakeClientPutInServerRegistrations = 0;
	gFakeClientPutInServerUnregistrations = 0;
	gFakeClientDisconnectRegistrations = 0;
	gFakeClientDisconnectUnregistrations = 0;
	gFakeMenus.clear();
	gFakeNextMenuId = 0;
	gFakeClientMenu = {};
	gFakeCreateMenuCalls = 0;
	gFakeDestroyMenuCalls = 0;
	gFakeCancelClientMenuCalls = 0;
	gFakeCancels.clear();
	gFakeMenuTypeSets.clear();
	gFakeCancelAnswer = true;
	gFakeDestroyRefuses = false;
	gFakeHudmenuChoice = "hud";
	ResetMenuTypes();
	gFakeCookies.clear();
	gFakeCookieWrites.clear();
	gFakeUserLoadedRegistrations = 0;
	gFakeUserLoadedUnregistrations = 0;
	gFakeUserLoaded = nullptr;
	gGameRules = {};
}

void InstallFakes() {
	plg::Log = &FakeLog;
	plg::GetConfigsDir = &FakeGetConfigsDir;
	plg::GetDataDir = &FakeGetDataDir;
	plg::plugin::GetId = &FakeGetId;
	plg::plugin::handle = reinterpret_cast<plg::hndl>(static_cast<uintptr_t>(0x1));

	__s2sdk_GetCurrentMap = &FakeGetCurrentMap;
	__s2sdk_IsClientConnected = &FakeIsClientConnected;
	__s2sdk_IsClientInGame = &FakeIsClientInGame;
	__s2sdk_IsClientSourceTV = &FakeIsClientSourceTV;
	__s2sdk_IsFakeClient = &FakeIsFakeClient;
	__s2sdk_IsClientAuthorized = &FakeIsClientAuthorized;
	__s2sdk_IsClientAlive = &FakeIsClientAlive;
	__s2sdk_GetClientSteamID64 = &FakeGetClientSteamID64;
	__s2sdk_GetClientName = &FakeGetClientName;
	__s2sdk_GetClientPawn = &FakeGetClientPawn;
	__s2sdk_PlayerSlotToEntPointer = &FakePlayerSlotToEntPointer;
	__s2sdk_PlayerSlotToEntHandle = &FakePlayerSlotToEntHandle;
	__s2sdk_EntPointerToEntIndex = &FakeEntPointerToEntIndex;
	__s2sdk_IsValidEntPointer = &FakeIsValidEntPointer;
	__s2sdk_IsValidEntHandle = &FakeIsValidEntHandle;
	__s2sdk_GetEntData = &FakeGetEntData;
	__s2sdk_GetSchemaOffset = &FakeGetSchemaOffset;
	__s2sdk_GetGameRules = &FakeGetGameRules;
	__s2sdk_GetGameTotalRoundsPlayed = &FakeGetGameTotalRoundsPlayed;
	__s2sdk_GetServerLanguage = &FakeGetServerLanguage;
	__s2sdk_PrintToChat = &FakePrintToChat;
	__s2sdk_PrintToServer = &FakePrintToServer;
	__s2sdk_PrintCenterText = &FakePrintCenterText;
	__s2sdk_AddConsoleCommand = &FakeAddConsoleCommand;
	__s2sdk_RemoveCommand = &FakeRemoveCommand;
	__s2sdk_AddCommandListener = &FakeAddCommandListener;
	__s2sdk_RemoveCommandListener = &FakeRemoveCommandListener;
	__s2sdk_HookEvent = &FakeHookEvent;
	__s2sdk_UnhookEvent = &FakeUnhookEvent;
	__s2sdk_GetEventPlayerSlot = &FakeGetEventPlayerSlot;
	__s2sdk_OnMapStart_Register = &FakeOnMapStartRegister;
	__s2sdk_OnMapStart_Unregister = &FakeOnMapStartUnregister;
	__s2sdk_OnMapEnd_Register = &FakeOnMapEndRegister;
	__s2sdk_OnMapEnd_Unregister = &FakeOnMapEndUnregister;
	__s2sdk_OnClientAuthenticated_Register = &FakeOnClientAuthenticatedRegister;
	__s2sdk_OnClientAuthenticated_Unregister = &FakeOnClientAuthenticatedUnregister;
	__s2sdk_OnClientPutInServer_Register = &FakeOnClientPutInServerRegister;
	__s2sdk_OnClientPutInServer_Unregister = &FakeOnClientPutInServerUnregister;
	__s2sdk_OnClientDisconnect_Register = &FakeOnClientDisconnectRegister;
	__s2sdk_OnClientDisconnect_Unregister = &FakeOnClientDisconnectUnregister;
	__s2sdk_CreateTimer = &FakeCreateTimer;
	__s2sdk_CreateMenu = &FakeCreateMenu;
	__s2sdk_DestroyMenu = &FakeDestroyMenu;
	__s2sdk_IsValidMenu = &FakeIsValidMenu;
	__s2sdk_AddMenuItem = &FakeAddMenuItem;
	__s2sdk_DisplayMenu = &FakeDisplayMenu;
	__s2sdk_GetClientMenu = &FakeGetClientMenu;
	__s2sdk_CancelClientMenu = &FakeCancelClientMenu;
	__s2sdk_GetClientMenuOffset = &FakeGetClientMenuOffset;
	__s2sdk_SetMenuType = &FakeSetMenuType;
	__s2sdk_GetMenuType = &FakeGetMenuType;
	__s2sdk_IsMenuTypeRegistered = &FakeIsMenuTypeRegistered;
	__s2sdk_SetMenuPagination = &FakeSetMenuPagination;
	__s2sdk_SetMenuExitButton = &FakeSetMenuExitButton;
	__s2sdk_SetMenuExitBackButton = &FakeSetMenuExitBackButton;
	__s2sdk_SetMenuCloseOnSelect = &FakeSetMenuCloseOnSelect;
	__hudmenu_GetClientMenuType = &FakeGetClientMenuType;
	__hudmenu_SetClientMenuType = &FakeSetClientMenuType;
	__permissions_GetCookie = &FakeGetCookie;
	__permissions_SetCookie = &FakeSetCookie;
	__permissions_OnUserLoaded_Register = &FakeOnUserLoadedRegister;
	__permissions_OnUserLoaded_Unregister = &FakeOnUserLoadedUnregister;
}

// ---------------------------------------------------------------------------------------------
// driving the plugin
// ---------------------------------------------------------------------------------------------

void Update(int milliseconds) {
	g_vipPlugin.OnPluginUpdate(std::chrono::milliseconds(milliseconds));
}

bool PumpUntil(const std::function<bool()>& done, int iterations = 600) {
	for (int i = 0; i < iterations; ++i) {
		if (done()) {
			return true;
		}
		Update(10);
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	return done();
}

void Start() {
	const plg::PluginResult result = g_vipPlugin.OnPluginStart();
	CHECK(result.GetCode() == plg::PluginCode::Ok); // never Failed (A1)
}

void Stop() {
	g_vipPlugin.OnPluginEnd();
}

void FireMapStart() {
	CHECK(gFakeOnMapStart != nullptr);
	if (gFakeOnMapStart != nullptr) {
		gFakeOnMapStart();
	}
}

void FireMapEnd() {
	CHECK(gFakeOnMapEnd != nullptr);
	if (gFakeOnMapEnd != nullptr) {
		gFakeOnMapEnd();
	}
}

s2sdk::ResultType InvokeCommand(const std::string& name, int32_t caller, s2sdk::ConCommandContext context,
								plg::vector<plg::string> args) {
	auto found = gFakeCommands.find(name);
	if (found == gFakeCommands.end() || found->second.callback == nullptr) {
		return s2sdk::ResultType::Continue;
	}
	return found->second.callback(caller, context, args);
}

s2sdk::MenuHandlerCallback ClientMenuHandler(int32_t slot) {
	const s2sdk::MenuId id = gFakeClientMenu[static_cast<std::size_t>(slot)];
	auto found = gFakeMenus.find(id);
	return found == gFakeMenus.end() ? nullptr : found->second.handler;
}

const FakeMenu* ClientMenu(int32_t slot) {
	const s2sdk::MenuId id = gFakeClientMenu[static_cast<std::size_t>(slot)];
	auto found = gFakeMenus.find(id);
	return found == gFakeMenus.end() ? nullptr : &found->second;
}

std::string JoinChat(std::string_view separator = "\n") {
	std::string text;
	for (const ChatLineRecord& line : gChatLines) {
		if (!text.empty()) {
			text += separator;
		}
		text += line.text;
	}
	return text;
}

// The VIP callback registrations the modules use (same exported functions).
bool gToggleCalled = false;
int gToggleCalls = 0;
int32_t gToggleSlot = -1;
std::string gToggleFeature;
vip::ToggleState gToggleOld = vip::ToggleState::ENABLED;
vip::ToggleState gToggleNew = vip::ToggleState::DISABLED;
bool gSelectCalled = false;
int gSelectCalls = 0;
bool gSelectReturn = true;

bool FakeToggle(int32_t slot, const plg::string& feature, vip::ToggleState oldState, vip::ToggleState& newState) {
	gToggleCalled = true;
	++gToggleCalls;
	gToggleSlot = slot;
	gToggleFeature = std::string(feature.data(), feature.size());
	gToggleOld = oldState;
	gToggleNew = newState;
	(void)newState;
	return false; // never veto
}

plg::string FakeDisplay(int32_t /*slot*/, const plg::string& /*feature*/) {
	return plg::string();
}

bool FakeSelect(int32_t /*slot*/, const plg::string& /*feature*/) {
	gSelectCalled = true;
	++gSelectCalls;
	return gSelectReturn;
}

int gLoadedCalls = 0;
int32_t gLoadedSlot = -1;
bool gLoadedVip = false;

void OnClientLoaded(int32_t slot, bool isVip) {
	++gLoadedCalls;
	gLoadedSlot = slot;
	gLoadedVip = isVip;
}

int gSpawnCalls = 0;
int32_t gSpawnSlot = -1;
int32_t gSpawnTeam = 0;
bool gSpawnVip = false;

void OnPlayerSpawn(int32_t slot, int32_t team, bool isVip) {
	++gSpawnCalls;
	gSpawnSlot = slot;
	gSpawnTeam = team;
	gSpawnVip = isVip;
}

int gRemovedCalls = 0;
int32_t gRemovedReason = 0;
int gAddedCalls = 0;

void OnVipRemoved(int32_t /*slot*/, int32_t reason) {
	++gRemovedCalls;
	gRemovedReason = reason;
}

void OnVipAdded(int32_t /*slot*/) {
	++gAddedCalls;
}

int gReadyCalls = 0;

void OnReady() {
	++gReadyCalls;
}

// ---------------------------------------------------------------------------------------------
// tests
// ---------------------------------------------------------------------------------------------

void TestStartupAndMapGate() {
	Section("startup + PITFALLS B1/B2 map gate");

	ResetFakes();
	ResetTempTree();
	Start();

	CHECK(g_vipPlugin.IsReady());
	CHECK_EQ_INT(gReadyCalls, 1); // VIP_OnVIPLoaded fired when the core became ready
	CHECK_EQ_INT(gFakeMapStartRegistrations, 1);
	CHECK_EQ_INT(gFakeMapEndRegistrations, 1);
	CHECK_EQ_INT(gFakeClientAuthRegistrations, 1);
	CHECK_EQ_INT(gFakeClientPutInServerRegistrations, 1);
	CHECK_EQ_INT(gFakeClientDisconnectRegistrations, 1);
	CHECK_EQ_INT(gFakeUserLoadedRegistrations, 1);
	CHECK_EQ_INT(static_cast<long long>(gFakeEventHooks.size()), 2);
	for (const FakeEventHook& hook : gFakeEventHooks) {
		CHECK(hook.mode == s2sdk::HookMode::Pre);
	}
	CHECK_EQ_INT(gFakeTimerCalls, 0); // no s2sdk timer, ever (C9)
	CHECK_EQ_INT(gFakeCreateMenuCalls, 1); // the burned id 0 only
	CHECK(gFakeMenus.find(0) != gFakeMenus.end());

	// The eight commands, with the original's rights.
	CHECK_EQ_INT(static_cast<long long>(gFakeCommands.size()), 8);
	const auto mmVip = gFakeCommands.find("mm_vip");
	const auto vipCommand = gFakeCommands.find("vip");
	const auto give = gFakeCommands.find("vip_give");
	CHECK(mmVip != gFakeCommands.end());
	CHECK(vipCommand != gFakeCommands.end());
	CHECK(give != gFakeCommands.end());
	if (mmVip != gFakeCommands.end() && vipCommand != gFakeCommands.end() && give != gFakeCommands.end()) {
		const auto clientFlags = static_cast<int64_t>(mmVip->second.flags);
		CHECK((clientFlags & static_cast<int64_t>(s2sdk::ConVarFlag::ClientCanExecute)) != 0);
		CHECK((clientFlags & static_cast<int64_t>(s2sdk::ConVarFlag::Release)) != 0);
		CHECK(static_cast<int64_t>(vipCommand->second.flags) == clientFlags);
		const auto serverFlags = static_cast<int64_t>(give->second.flags);
		CHECK((serverFlags & static_cast<int64_t>(s2sdk::ConVarFlag::ClientCanExecute)) == 0);
		CHECK((serverFlags & static_cast<int64_t>(s2sdk::ConVarFlag::Release)) != 0);
	}

	// Before the first map: no client, entity or schema call, and none is made by updates either.
	CHECK(!vip::IsGameReady());
	const long long clientCallsBefore = gFakeClientCalls;
	const long long schemaCallsBefore = gFakeSchemaCalls;
	for (int i = 0; i < 10; ++i) {
		Update(16);
	}
	CHECK(!vip::IsGameReady());
	CHECK_EQ_INT(gFakeClientCalls, clientCallsBefore);
	CHECK_EQ_INT(gFakeSchemaCalls, schemaCallsBefore);
	// VIP_WarmupPeriod asks for the schema only with a live map (C8).
	CHECK(!vip::g_vipPlugin.WarmupPeriod());
	CHECK_EQ_INT(gFakeSchemaCalls, schemaCallsBefore);
	// Before the map the pistol state is neutral false and makes no engine or schema call.
	CHECK(!vip::g_vipPlugin.PistolRound());
	CHECK(!::VIP_PistolRound());
	CHECK_EQ_INT(gFakeClientCalls, clientCallsBefore);
	CHECK_EQ_INT(gFakeSchemaCalls, schemaCallsBefore);

	// "<empty>" is not a map either: the mid-map latch must not open the gate.
	gFakeMapName = "<empty>";
	Update(16);
	CHECK(!vip::IsGameReady());

	// Conservative unknown state: a late-loaded map may be mid second-half pistol, so the rules
	// snapshot is not trusted until a real round_prestart.
	GameRulesInt(kOffsetTotalRounds) = 12;
	GameRulesInt(kOffsetSwitching) = 0;
	GameRulesInt(kOffsetRestart) = 0;
	GameRulesInt(kOffsetOvertime) = 0;
	// A real map name lets the first update of a plugin loaded into a running map open the gate.
	gFakeMapName = "de_dust2";
	Update(16);
	CHECK(vip::IsGameReady());
	CHECK(vip::GameMapLoaded());
	CHECK(vip::g_vipPlugin.PistolRound());
	CHECK(::VIP_PistolRound());
	// Modifying the rules snapshot alone never releases the provisional state.
	GameRulesInt(kOffsetTotalRounds) = 13;
	GameRulesInt(kOffsetSwitching) = 1;
	Update(16);
	CHECK(vip::g_vipPlugin.PistolRound());
	GameRulesInt(kOffsetTotalRounds) = 12;
	GameRulesInt(kOffsetSwitching) = 0;

	// OnMapEnd closes it, and updates pumped through the changelevel do not reopen it.
	FireMapEnd();
	CHECK(!vip::IsGameReady());
	const long long schemaCallsAtMapEnd = gFakeSchemaCalls;
	CHECK(!vip::g_vipPlugin.PistolRound());
	CHECK(!::VIP_PistolRound());
	CHECK_EQ_INT(gFakeSchemaCalls, schemaCallsAtMapEnd);
	const long long clientCallsAtMapEnd = gFakeClientCalls;
	for (int i = 0; i < 10; ++i) {
		Update(16);
	}
	CHECK(!vip::IsGameReady());
	CHECK_EQ_INT(gFakeClientCalls, clientCallsAtMapEnd);

	// Only a new OnMapStart reopens it.
	FireMapStart();
	CHECK(vip::IsGameReady());
	CHECK(vip::g_vipPlugin.PistolRound());
	Update(16);
	CHECK(vip::IsGameReady());
}

void TestClientGuardsAndReconcile() {
	Section("PITFALLS C1/C2 + the once-per-second reconcile");

	SetHumanPlayer(0, kSteamId, "E!N", kValidNegativeHandle, 3);
	CHECK(g_vipPlugin.IsSlotHuman(0));
	// C1: a negative handle with the serial bit set is valid.
	CHECK_EQ_INT(g_vipPlugin.ControllerHandleOf(0), kValidNegativeHandle);
	CHECK_EQ_INT(g_vipPlugin.AccountIdOf(0), 0); // not tracked yet

	// -1 and 0 are the only invalid handles.
	gPlayers[0].controllerHandle = -1;
	CHECK_EQ_INT(g_vipPlugin.ControllerHandleOf(0), 0);
	CHECK(!g_vipPlugin.IsSlotHuman(0));
	gPlayers[0].controllerHandle = 0;
	CHECK_EQ_INT(g_vipPlugin.ControllerHandleOf(0), 0);
	gPlayers[0].controllerHandle = kValidNegativeHandle;

	// Bots, SourceTV and a client that is not in game are refused (C2).
	gPlayers[0].fake = true;
	CHECK(!g_vipPlugin.IsSlotHuman(0));
	gPlayers[0].fake = false;
	gPlayers[0].sourceTV = true;
	CHECK(!g_vipPlugin.IsSlotHuman(0));
	gPlayers[0].sourceTV = false;
	gPlayers[0].inGame = false;
	CHECK(!g_vipPlugin.IsSlotHuman(0));
	gPlayers[0].inGame = true;

	// Out-of-range slots are refused before any s2sdk call.
	const long long callsBefore = gFakeClientCalls;
	CHECK(!g_vipPlugin.IsSlotHuman(-1));
	CHECK(!g_vipPlugin.IsSlotHuman(64));
	CHECK_EQ_INT(gFakeClientCalls, callsBefore);

	CHECK(g_vipPlugin.IsSlotConnectedHuman(0));
	CHECK(vip::IsGameReady());

	// The reconcile reads the row (none yet) and fires VIP_OnClientLoaded(slot, false).
	CHECK(PumpUntil([] { return gLoadedCalls == 1; }));
	CHECK_EQ_INT(gLoadedSlot, 0);
	CHECK(!gLoadedVip);
	CHECK_EQ_INT(g_vipPlugin.AccountIdOf(0), 42);
	CHECK_EQ_INT(g_vipPlugin.SteamIdOf(0), kSteamId);
	CHECK_EQ_STR(g_vipPlugin.PlayerName(0), "E!N");

	// The SQLite fallback created the table in <data>/vip/.
	CHECK(std::filesystem::exists(gData / "vip" / "vip.sqlite3"));
}

void TestGiveVipAndDatabase() {
	Section("vip_give, the vip_users row and the SQLite backend");

	// The server-console commands accept strictly caller == -1: -2 is not a bypass.
	gServerLines.clear();
	const auto rejected = InvokeCommand("vip_status", -2, s2sdk::ConCommandContext::Console,
										{"vip_status"});
	CHECK(rejected == s2sdk::ResultType::Handled);
	CHECK(gServerLines.empty());

	gServerLines.clear();
	InvokeCommand("vip_status", -1, s2sdk::ConCommandContext::Console, {"vip_status"});
	CHECK(AnyLogContains(gServerLines, "vip_status slot 0"));
	CHECK(AnyLogContains(gServerLines, "record=none"));

	// A server-console command typed in chat by a player is rejected without running.
	gServerLines.clear();
	const auto fromChat = InvokeCommand("vip_reload", 0, s2sdk::ConCommandContext::Chat, {"vip_reload"});
	CHECK(fromChat == s2sdk::ResultType::Handled);
	CHECK(gServerLines.empty());

	// vip_give on a connected, pawn-owning player: welcome message, callbacks and the DB row.
	const int loadedBefore = gLoadedCalls;
	const auto give = InvokeCommand("vip_give", -1, s2sdk::ConCommandContext::Console,
									{"vip_give", "0", "3600", "Gold"});
	CHECK(give == s2sdk::ResultType::Handled);
	CHECK(g_vipPlugin.IsClientVip(0));
	CHECK_EQ_INT(gLoadedCalls, loadedBefore + 1);
	CHECK_EQ_INT(gAddedCalls, 1);
	CHECK(JoinChat().find("Добро пожаловать") != std::string::npos);
	CHECK(JoinChat().find("E!N") != std::string::npos);
	CHECK(AnyLogContains(gServerLines, "You have successfully granted VIP status"));

	// The INSERT is asynchronous: poll until the row is readable through a second connection.
	const std::string sqlitePath = (gData / "vip" / "vip.sqlite3").string();
	plgdb::Config config;
	config.driver = plgdb::Driver::SQLite;
	config.sqlitePath = sqlitePath;
	std::unique_ptr<plgdb::Database> reader = plgdb::Database::Create(config);
	CHECK(reader != nullptr);
	if (reader == nullptr) {
		return;
	}
	bool rowFound = false;
	PumpUntil([&] {
		const plgdb::Result result = reader->QuerySync(
			"SELECT `name`, `group`, `expires`, `sid` FROM vip_users WHERE account_id = ? AND sid = ?", {42, 7});
		if (!result.Ok() || result.RowCount() == 0) {
			return false;
		}
		rowFound = result.GetString(0, 0) == "E!N" && result.GetString(0, 1) == "Gold" &&
				   result.GetInt(0, 3) == 7 && result.GetInt(0, 2) > 0;
		return rowFound;
	});
	CHECK(rowFound);

	// VIP_GetTranslate: the phrase, and an empty string for an unknown key (the modules rely on it).
	CHECK_EQ_STR(plg::string(::VIP_GetTranslate(plg::string("MenuTitle"))).c_str(), "Меню VIP");
	CHECK_EQ_STR(plg::string(::VIP_GetTranslate(plg::string("NoSuchKey"))).c_str(), "");

	// vip_reload reloads groups.ini through the same first-existing-file rule.
	gServerLines.clear();
	InvokeCommand("vip_reload", -1, s2sdk::ConCommandContext::Console, {"vip_reload"});
	CHECK(AnyLogContains(gServerLines, "VIP groups has been successfully updated"));
	CHECK(vip::g_vipPlugin.IsValidGroup("Gold"));
	CHECK(vip::g_vipPlugin.IsValidGroup("Silver"));
	CHECK(!vip::g_vipPlugin.IsValidGroup("server_id"));
}

void TestFeaturesAndMenu() {
	Section("the native VIP menu (PITFALLS C10)");

	// Health: togglable int. Speed: selectable (closes the menu when its callback asks to).
	::VIP_RegisterFeature(plg::string("Health"), vip::ValueType::VIP_INT, vip::FeatureType::TOGGLABLE,
						nullptr, &FakeToggle, &FakeDisplay);
	::VIP_RegisterFeature(plg::string("Speed"), vip::ValueType::VIP_FLOAT, vip::FeatureType::SELECTABLE,
						&FakeSelect, nullptr, &FakeDisplay);

	// The chat spelling "vip" opens the menu and is swallowed; "!mm_vip" is not a chat spelling.
	const auto chatVip = InvokeCommand("vip", 0, s2sdk::ConCommandContext::Chat, {"vip"});
	CHECK(chatVip == s2sdk::ResultType::Handled);
	const auto chatMm = InvokeCommand("mm_vip", 0, s2sdk::ConCommandContext::Chat, {"mm_vip"});
	CHECK(chatMm == s2sdk::ResultType::Continue);
	Update(16);

	const FakeMenu* menu = ClientMenu(0);
	CHECK(menu != nullptr);
	if (menu == nullptr) {
		return;
	}
	CHECK_EQ_STR(menu->title, "Меню VIP");
	CHECK_EQ_STR(menu->type, "hud"); // hudmenu's "hud", registered by the fake
	CHECK_EQ_INT(static_cast<long long>(menu->items.size()), 2);
	if (menu->items.size() == 2) {
		CHECK_EQ_STR(menu->items[0].info, "Health");
		CHECK_EQ_STR(menu->items[0].display, "Здоровье [150]");
		CHECK_EQ_STR(menu->items[1].info, "Speed");
		CHECK_EQ_STR(menu->items[1].display, "Скорость"); // SELECTABLE: the name only
	}

	// Selecting Health toggles the '<group>.<feature>' cookie and re-opens the menu.
	s2sdk::MenuHandlerCallback handler = ClientMenuHandler(0);
	CHECK(handler != nullptr);
	if (handler != nullptr) {
		const s2sdk::MenuId before = gFakeClientMenu[0];
		handler(before, s2sdk::MenuAction::Select, 0, 0);
		Update(16);
		CHECK_EQ_INT(gToggleCalls, 1);
		CHECK_EQ_INT(gToggleSlot, 0);
		CHECK_EQ_STR(gToggleFeature, "Health");
		CHECK(gToggleOld == vip::ToggleState::ENABLED);
		CHECK(gToggleNew == vip::ToggleState::DISABLED);
		CHECK_EQ_INT(static_cast<long long>(gFakeCookieWrites.size()), 1);
		if (!gFakeCookieWrites.empty()) {
			CHECK_EQ_STR(gFakeCookieWrites[0].second, "Gold.Health");
		}
		const auto cookie = gFakeCookies.find(CookieKey(kSteamId, "Gold.Health"));
		CHECK(cookie != gFakeCookies.end());
		if (cookie != gFakeCookies.end()) {
			CHECK_EQ_STR(vip::CookieToString(cookie->second), "0");
		}
		CHECK_EQ_STR(plg::string(::VIP_GetClientCookie(0, plg::string("Health"))).c_str(), "0");
		CHECK_EQ_STR(plg::string(::VIP_GetClientFeatureString(0, plg::string("Health"))).c_str(), "");
		CHECK_EQ_INT(::VIP_GetClientFeatureInt(0, plg::string("Health")), -1);
		// The old menu is destroyed and a fresh one is displayed for the same slot.
		CHECK(gFakeClientMenu[0] != 0);
		CHECK(gFakeClientMenu[0] != before);
		CHECK(ClientMenu(0) != nullptr);
	}

	// Selecting Speed runs the select callback; a true answer closes the menu.
	gSelectCalls = 0;
	handler = ClientMenuHandler(0);
	if (handler != nullptr) {
		handler(gFakeClientMenu[0], s2sdk::MenuAction::Select, 0, 1);
		Update(16);
		CHECK_EQ_INT(gSelectCalls, 1);
		CHECK_EQ_INT(gFakeClientMenu[0], 0);
	}

	// The menu session owns the identity it was created for (SteamID64 + controller handle), and
	// OnMenuHandler must NOT query the current slot while the MenuManager lock is held.
	g_vipPlugin.OpenMenu(0);
	Update(16);
	handler = ClientMenuHandler(0);
	CHECK(handler != nullptr);
	if (handler != nullptr) {
		const int togglesBefore = gToggleCalls;
		const std::size_t writesBefore = gFakeCookieWrites.size();
		const long long clientCallsBefore = gFakeClientCalls;
		handler(gFakeClientMenu[0], s2sdk::MenuAction::Select, 0, 0);
		// The handler only queues: no client/entity query happened under the menu manager lock.
		CHECK_EQ_INT(gFakeClientCalls, clientCallsBefore);
		// The same SteamID64 reconnected with a new controller serial before the action ran.
		gPlayers[0].controllerHandle = kValidPositiveHandle;
		Update(16);
		CHECK_EQ_INT(gToggleCalls, togglesBefore);
		CHECK_EQ_INT(static_cast<long long>(gFakeCookieWrites.size()), static_cast<long long>(writesBefore));
		CHECK_EQ_INT(gFakeClientMenu[0], 0); // the stale menu was closed, not acted on
		gPlayers[0].controllerHandle = kValidNegativeHandle;
	}

	// The hard case the reviewer named: the slot is reused by ANOTHER player before the engine
	// delivers the Select of the old menu. The action still carries the menu's owner (player A),
	// ProcessMenuWork sees B in the slot, closes the stale menu and calls no feature callback.
	g_vipPlugin.OpenMenu(0);
	Update(16);
	handler = ClientMenuHandler(0);
	CHECK(handler != nullptr);
	if (handler != nullptr) {
		const s2sdk::MenuId staleId = gFakeClientMenu[0];
		const int togglesBefore = gToggleCalls;
		const std::size_t writesBefore = gFakeCookieWrites.size();
		const uint64_t otherSteamId = kSteamId + 1; // account id 43
		gPlayers[0].steamid = otherSteamId;
		gPlayers[0].name = "Player B";
		handler(staleId, s2sdk::MenuAction::Select, 0, 0); // delivered after the slot changed hands
		Update(16);
		CHECK_EQ_INT(gToggleCalls, togglesBefore);
		CHECK_EQ_INT(static_cast<long long>(gFakeCookieWrites.size()), static_cast<long long>(writesBefore));
		CHECK_EQ_INT(gFakeClientMenu[0], 0);
		gPlayers[0].steamid = kSteamId;
		gPlayers[0].name = "E!N";
	}

	// The menu type is the exact Pisex table: the player's choice first, then HUD_LAYOUT.
	gFakeHudmenuChoice = "chat";
	g_vipPlugin.OpenMenu(0);
	Update(16);
	CHECK(ClientMenu(0) != nullptr && ClientMenu(0)->type == "chat");
	gFakeHudmenuChoice = "";
	gFakeMenuTypes.erase("hud");
	g_vipPlugin.OpenMenu(0);
	Update(16);
	CHECK(ClientMenu(0) != nullptr && ClientMenu(0)->type == "button");
	ResetMenuTypes();
	gFakeHudmenuChoice = "hud";
	g_vipPlugin.OpenMenu(0);
	Update(16);
	CHECK(ClientMenu(0) != nullptr && ClientMenu(0)->type == "hud");

	// VIP_PrintToCenter goes through the s2sdk centre destination.
	::VIP_PrintToCenter(0, plg::string("hello"));
	CHECK(!gCenterLines.empty());
	if (!gCenterLines.empty()) {
		CHECK_EQ_STR(gCenterLines.back().text, "hello");
	}
}

void TestAreaMessagesAndExpiry() {
	Section("welcome/expiry messages and the per-second expiry loop");

	// The disable of the feature ("0" cookie) is already covered; re-enable by removing it.
	gFakeCookies.clear();
	CHECK_EQ_INT(::VIP_GetClientFeatureInt(0, plg::string("Health")), 150);

	// Expiry: drop the record, give an already-expired one in memory only, let the tick fire.
	CHECK(g_vipPlugin.RemoveVip(0, /*notify=*/false, /*in_db=*/false));
	CHECK(!vip::g_vipPlugin.IsClientVip(0));
	CHECK(g_vipPlugin.GiveVip(0, /*time=*/-10, "Gold", /*add_to_db=*/false));
	gChatLines.clear();
	const int removedBefore = gRemovedCalls;
	CHECK(PumpUntil([&removedBefore] { return gRemovedCalls > removedBefore; }));
	CHECK_EQ_INT(gRemovedCalls, removedBefore + 1);
	CHECK_EQ_INT(gRemovedReason, vip::kRemoveExpired);
	CHECK(!vip::g_vipPlugin.IsClientVip(0));
	CHECK(JoinChat().find("Ваш VIP истёк") != std::string::npos);
	CHECK(JoinChat().find("Строка 2") != std::string::npos);
	CHECK(JoinChat().find("Строка 3") != std::string::npos);
}

void TestSpawnCallback() {
	Section("PITFALLS C2/C13: the deferred VIP_OnPlayerSpawn");

	// Re-give the VIP status the expiry test removed (in memory only).
	CHECK(g_vipPlugin.GiveVip(0, 0, "Gold", false));
	CHECK(vip::g_vipPlugin.IsClientVip(0));

	FakeEvent event;
	event.slot = 0;
	const s2sdk::ResultType result = FireGameEvent("player_spawn", event);
	CHECK(result == s2sdk::ResultType::Continue); // a Pre hook never vetoes (C13)
	Update(16);
	CHECK_EQ_INT(gSpawnCalls, 1);
	CHECK_EQ_INT(gSpawnSlot, 0);
	CHECK_EQ_INT(gSpawnTeam, 3);
	CHECK(gSpawnVip);

	// A slot/controller change between the event and the deferred action drops it.
	gSpawnCalls = 0;
	FireGameEvent("player_spawn", event);
	gPlayers[0].controllerHandle = kValidPositiveHandle;
	Update(16);
	CHECK_EQ_INT(gSpawnCalls, 0);
	gPlayers[0].controllerHandle = kValidNegativeHandle;

	// An out-of-range event slot never reaches a client call.
	gSpawnCalls = 0;
	const long long callsBefore = gFakeClientCalls;
	event.slot = 64;
	FireGameEvent("player_spawn", event);
	Update(16);
	CHECK_EQ_INT(gSpawnCalls, 0);
	CHECK_EQ_INT(gFakeClientCalls, callsBefore);

	// The pawn requirement of vip_give: without a pawn the name/id search still works, but the
	// command must not treat the player as "giveable".
	gPlayers[0].pawnPointer = nullptr;
	CHECK_EQ_INT(g_vipPlugin.FindPlayerSlot("E!N", /*require_pawn=*/true), -1);
	CHECK_EQ_INT(g_vipPlugin.FindPlayerSlot("E!N", /*require_pawn=*/false), 0);
	gPlayers[0].pawnPointer = reinterpret_cast<void*>(static_cast<uintptr_t>(0x2000));
}

void TestRoundState() {
	Section("the game rules reads (C6/C8)");

	GameRulesInt(kOffsetWarmup) = 1;
	CHECK(vip::g_vipPlugin.WarmupPeriod());
	GameRulesInt(kOffsetWarmup) = 0;
	CHECK(!vip::g_vipPlugin.WarmupPeriod());

	GameRulesInt(kOffsetTotalRounds) = 0;
	FakeEvent event;
	FireGameEvent("round_prestart", event);
	CHECK(vip::g_vipPlugin.PistolRound());

	GameRulesInt(kOffsetTotalRounds) = 5;
	GameRulesInt(kOffsetSwitching) = 0;
	GameRulesInt(kOffsetRestart) = 0;
	FireGameEvent("round_prestart", event);
	CHECK(!vip::g_vipPlugin.PistolRound());

	GameRulesInt(kOffsetSwitching) = 1;
	GameRulesInt(kOffsetOvertime) = 0;
	FireGameEvent("round_prestart", event);
	CHECK(vip::g_vipPlugin.PistolRound()); // a team switch at round reset

	GameRulesInt(kOffsetOvertime) = 2;
	FireGameEvent("round_prestart", event);
	CHECK(!vip::g_vipPlugin.PistolRound());

	GameRulesInt(kOffsetRestart) = 1;
	FireGameEvent("round_prestart", event);
	CHECK(vip::g_vipPlugin.PistolRound());

	CHECK_EQ_INT(::VIP_GetTotalRounds(), 5);

	// Late-map reset: a map start with total 12 and switching 0 stays provisional until prestart.
	GameRulesInt(kOffsetTotalRounds) = 12;
	GameRulesInt(kOffsetSwitching) = 0;
	GameRulesInt(kOffsetRestart) = 0;
	GameRulesInt(kOffsetOvertime) = 0;
	FireMapEnd();
	FireMapStart();
	CHECK(vip::g_vipPlugin.PistolRound());
	// An explicit normal prestart at total 13 settles it to false.
	GameRulesInt(kOffsetTotalRounds) = 13;
	FireGameEvent("round_prestart", event);
	CHECK(!vip::g_vipPlugin.PistolRound());
	// An explicit second-half prestart at total 12 with a team switch is a pistol round.
	GameRulesInt(kOffsetTotalRounds) = 12;
	GameRulesInt(kOffsetSwitching) = 1;
	FireGameEvent("round_prestart", event);
	CHECK(vip::g_vipPlugin.PistolRound());
	// Clearing the switching flag without an event does not change the cached state.
	GameRulesInt(kOffsetSwitching) = 0;
	CHECK(vip::g_vipPlugin.PistolRound());
	// The next normal prestart at total 13 settles it again.
	GameRulesInt(kOffsetTotalRounds) = 13;
	FireGameEvent("round_prestart", event);
	CHECK(!vip::g_vipPlugin.PistolRound());
	// Restore the fixture state later tests rely on.
	GameRulesInt(kOffsetTotalRounds) = 5;
	GameRulesInt(kOffsetSwitching) = 0;
	GameRulesInt(kOffsetRestart) = 0;
	GameRulesInt(kOffsetOvertime) = 0;
	FireGameEvent("round_prestart", event);
	CHECK(!vip::g_vipPlugin.PistolRound());
}

void TestMenuTeardownOnMapEnd() {
	Section("PITFALLS B2/A7: the menu teardown on map end and unload");

	// A normal map end: the display is ended with Disconnect, rechecked and destroyed, and no
	// backend/client call follows the gate close.
	g_vipPlugin.OpenMenu(0);
	Update(16);
	CHECK(gFakeClientMenu[0] != 0);
	const long long clientCallsBefore = gFakeClientCalls;
	FireMapEnd();
	CHECK(!vip::IsGameReady());
	CHECK_EQ_INT(gFakeClientMenu[0], 0);
	CHECK(!gFakeCancels.empty());
	if (!gFakeCancels.empty()) {
		CHECK(gFakeCancels.back().second == s2sdk::MenuCancelReason::Disconnect);
	}
	for (int i = 0; i < 10; ++i) {
		Update(16);
	}
	CHECK_EQ_INT(gFakeClientCalls, clientCallsBefore);
	CHECK(!AnyErrorContains("A7_UNRESOLVED"));

	// The fail-safe: a manager that claims "no display" (false cancel) while still reporting one
	// in GetClientMenu is forced onto the builtin no-op-close type before DestroyMenu (B2).
	FireMapStart();
	CHECK(vip::IsGameReady());
	g_vipPlugin.OpenMenu(0);
	Update(16);
	CHECK(gFakeClientMenu[0] != 0);
	gFakeCancelAnswer = false; // CancelClientMenu answers false and keeps the display
	const std::size_t typeSetsBefore = gFakeMenuTypeSets.size();
	FireMapEnd();
	CHECK(!vip::IsGameReady());
	CHECK_EQ_INT(gFakeClientMenu[0], 0);
	CHECK(gFakeMenuTypeSets.size() > typeSetsBefore);
	if (gFakeMenuTypeSets.size() > typeSetsBefore) {
		CHECK_EQ_STR(gFakeMenuTypeSets.back().second, "chat");
	}
	gFakeCancelAnswer = true;
}

void TestUnloadAndPairedBindings() {
	Section("A7: unload, the A7_UNRESOLVED report and a missing binding pair");

	// Force the impossible cleanup: the manager keeps the display AND refuses DestroyMenu. The
	// unload must report A7_UNRESOLVED for the ids it cannot release.
	FireMapStart();
	g_vipPlugin.OpenMenu(0);
	Update(16);
	CHECK(gFakeClientMenu[0] != 0);
	gFakeCancelAnswer = false;
	gFakeDestroyRefuses = true;
	FireMapEnd(); // the menu is parked (cannot be released in the changelevel window)
	const std::size_t removedBefore = gFakeRemovedCommands.size();
	Stop();

	CHECK(AnyErrorContains("A7_UNRESOLVED"));
	CHECK_EQ_INT(gFakeMapStartUnregistrations, 1);
	CHECK_EQ_INT(gFakeMapEndUnregistrations, 1);
	CHECK_EQ_INT(gFakeClientAuthUnregistrations, 1);
	CHECK_EQ_INT(gFakeClientPutInServerUnregistrations, 1);
	CHECK_EQ_INT(gFakeClientDisconnectUnregistrations, 1);
	CHECK_EQ_INT(gFakeUserLoadedUnregistrations, 1);
	CHECK_EQ_INT(static_cast<long long>(gFakeUnhookedEvents.size()), 2);
	CHECK(gFakeRemovedCommands.size() == removedBefore + 8);
	CHECK(gFakeCommands.empty());
	CHECK_EQ_INT(gFakeAddCommandListenerCalls, 0); // this plugin never uses bare listeners
	gFakeDestroyRefuses = false;
	gFakeCancelAnswer = true;

	// A missing unregister half must keep the whole pair unregistered: the map gate can never
	// open (without the map-end listener it could never close again) and the permissions listener
	// stays off.
	ResetFakes();
	ResetTempTree();
	__s2sdk_OnMapEnd_Unregister = nullptr;
	__permissions_OnUserLoaded_Unregister = nullptr;
	gFakePreexistingCommands = {"mm_vip"}; // AddConsoleCommand answers false: listener only
	gFakeMapName = "de_dust2";
	Start();
	CHECK_EQ_INT(gFakeMapStartRegistrations, 0);
	CHECK_EQ_INT(gFakeMapEndRegistrations, 0);
	CHECK_EQ_INT(gFakeUserLoadedRegistrations, 0);
	CHECK(AnyErrorContains("map listeners are unavailable"));
	CHECK(AnyErrorContains("permissions plugin is not available"));
	for (int i = 0; i < 10; ++i) {
		Update(16);
	}
	CHECK(!vip::IsGameReady());
	CHECK(AnyErrorContains("map listeners are unavailable"));

	// The pre-existing mm_vip is detached with RemoveCommandListener, not RemoveCommand.
	gFakeRemovedCommands.clear();
	gFakeRemovedCommandListeners.clear();
	Stop();
	CHECK(AnyLogContains(gFakeRemovedCommandListeners, "mm_vip"));
	CHECK(!AnyLogContains(gFakeRemovedCommands, "mm_vip"));
	CHECK(gFakeRemovedCommands.size() == 7);

	// A missing GetCurrentMap binding: the mid-map latch cannot open the gate at all.
	ResetFakes();
	ResetTempTree();
	__s2sdk_GetCurrentMap = nullptr;
	Start();
	gFakeMapName = "de_dust2";
	for (int i = 0; i < 10; ++i) {
		Update(16);
	}
	CHECK(!vip::IsGameReady());
	Stop();

	// Restore the complete binding set for a possible later test.
	__s2sdk_GetCurrentMap = &FakeGetCurrentMap;
	__s2sdk_OnMapEnd_Unregister = &FakeOnMapEndUnregister;
	__permissions_OnUserLoaded_Unregister = &FakeOnUserLoadedUnregister;
}

void TestConfigPriority() {
	Section("config priority: the raw server path is authoritative");

	// The previous wrapper resolved "addons/configs/vip/groups.ini" as: the path as given (the
	// server working directory), then <configs>/vip/, then translations and data; the first
	// existing file wins. The same rule applies to the phrases file and databases.cfg.
	ResetFakes();
	ResetTempTree();
	const std::string rawSqlite = (gRoot / "raw.sqlite3").string();
	WriteFileText(gRoot / "addons" / "configs" / "vip" / "groups.ini",
				  "\"VIP\"\n{\n\t\"server_id\"\t\"99\"\n\t\"Raw\"\n\t{\n\t\t\"Health\"\t\"5\"\n\t}\n}\n");
	WriteFileText(gRoot / "vip.phrases.txt",
				  "\"Phrases\"\n{\n\t\"MenuTitle\"\t{ \"ru\" \"RAW TITLE\" \"en\" \"RAW TITLE\" }\n}\n");
	WriteFileText(gRoot / "addons" / "configs" / "databases.cfg",
				  "\"Databases\"\n{\n\t\"vip\"\n\t{\n\t\t\"driver\"\t\"sqlite\"\n\t\t\"sqlite_path\"\t\"" +
					  rawSqlite + "\"\n\t}\n}\n");
	Start();

	CHECK_EQ_INT(g_vipPlugin.Cfg().server_id, 99);
	CHECK(vip::g_vipPlugin.IsValidGroup("Raw"));
	CHECK(!vip::g_vipPlugin.IsValidGroup("Gold"));
	CHECK_EQ_STR(plg::string(::VIP_GetTranslate(plg::string("MenuTitle"))).c_str(), "RAW TITLE");
	CHECK(g_vipPlugin.DatabaseConfig().driver == plgdb::Driver::SQLite);
	CHECK_EQ_STR(g_vipPlugin.DatabaseConfig().sqlitePath, rawSqlite);
	CHECK(std::filesystem::exists(rawSqlite));
	Stop();
}

} // namespace

int main() {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	std::printf("vip_runtime_tests\n");

	gRoot = std::filesystem::temp_directory_path() /
			("vip_runtime_tests_" + std::to_string(static_cast<long long>(::getpid())));
	std::error_code ec;
	gPreviousCwd = std::filesystem::current_path(ec);
	gConfigs = gRoot / "configs";
	gData = gRoot / "data";

	InstallFakes();
	ResetFakes();
	ResetTempTree();

	::VIP_OnVIPLoaded_Register(&OnReady);
	::VIP_OnClientLoaded_Register(&OnClientLoaded);
	::VIP_OnPlayerSpawn_Register(&OnPlayerSpawn);
	::VIP_OnVIPClientRemoved_Register(&OnVipRemoved);
	::VIP_OnVIPClientAdded_Register(&OnVipAdded);

	TestStartupAndMapGate();
	TestClientGuardsAndReconcile();
	TestGiveVipAndDatabase();
	TestFeaturesAndMenu();
	TestAreaMessagesAndExpiry();
	TestSpawnCallback();
	TestRoundState();
	TestMenuTeardownOnMapEnd();
	TestUnloadAndPairedBindings();
	TestConfigPriority();

	// The plugin must not answer anything after the unload.
	const long long callsAtEnd = gFakeClientCalls;
	g_vipPlugin.OnPluginUpdate(std::chrono::milliseconds(1000));
	CHECK_EQ_INT(gFakeClientCalls, callsAtEnd);
	CHECK(!vip::IsGameReady());

	if (!gPreviousCwd.empty()) {
		std::filesystem::current_path(gPreviousCwd, ec);
	}
	std::filesystem::remove_all(gRoot, ec);

	std::printf("%d checks, %d failures\n", gChecks, gFailures);
	return gFailures == 0 ? 0 : 1;
}
