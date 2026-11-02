// vip — engine side: lifecycle, configuration, the s2sdk listeners, the update-driven
// reconcile+expiry tick, the vip_core hook bodies, the messages of the original and the 34
// exported methods (the IVIPApi of the Pisex cs2-vip plugin).
//
// Ported from the original cs2-vip (vip.cpp) onto pure s2sdk + vip_core + the optional permissions
// plugin (the cookie store; the ICookiesApi of the original) + the optional hudmenu plugin (the
// player's own "!menu" choice). Commands live in commands.cpp, the menu in menus.cpp.
//
// No pisexcompat and no wrapper library (docs/PLUGIN_STANDARD.md §7); the safety rules of
// handoff/weaponskins/PITFALLS.md live right here:
//
//   * B1/B2 map gate. The four map-listener bindings (OnMapStart/OnMapEnd Register+Unregister) are
//     checked as a complete pair before anything is registered; readiness is granted by OnMapStart
//     or by the first OnPluginUpdate of a plugin loaded into a running map - and that mid-map latch
//     requires all four bindings registered, no OnMapEnd since OnPluginStart and
//     s2sdk::GetCurrentMap() naming a real map (non-empty, not "<empty>"). OnMapEnd only clears
//     state. NO client, entity or schema call is made while the gate is shut: before the first
//     map, through a changelevel and after OnPluginEnd.
//   * A1/A7: OnPluginStart never returns Failed; every registration checks its complete
//     add+remove pair first and OnPluginEnd removes everything symmetrically.
//   * C1: entity handles are signed int32 views of the engine's uint32 CEntityHandle. 0 and -1 are
//     the only invalid values; a negative handle may be real (the serial's bit 31) and is checked
//     with s2sdk::IsValidEntHandle. A handle is never compared with zero as an ordering - only
//     the two invalid sentinels are compared for equality.
//   * C2: a "human slot" is a connected, authorized, in-game, non-bot, non-SourceTV client with a
//     non-null controller pointer, a controller entity index and a valid controller handle.
//   * C8: schema offsets are resolved only with the map gate open and only successful results
//     (>= 0) are cached.
//   * C9/B4: no s2sdk timer. The spawn delay of VIP_OnPlayerSpawn, the once-per-second reconcile
//     and expiry loop and the plgdb Poll() pump are tick_accumulator.hpp accumulators fed by
//     OnPluginUpdate(dt); every deferred action revalidates SteamID64 + controller handle.
//   * C11/C13: the command callbacks answer only their intended context and the server-console
//     commands accept strictly caller == -1 (a -2 caller is not a bypass); the game-event hooks
//     are Pre hooks that always answer ResultType::Continue and read slots only through
//     s2sdk::GetEventPlayerSlot.
//   * nothing here blocks on the database except the startup path (CreateSchema in EnsureCore);
//     every s2sdk/plugify callback catches its exceptions (a throw into the module would kill the
//     server, not the plugin).

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <plg/any.hpp>
#include <plg/plugin.hpp>
#include <plg/string.hpp>
#include <plg/vector.hpp>
#include <plugin_export.h>
#include <s2sdk/s2sdk.hpp>

#include <permissions/permissions.hpp>

#include "vip_plugin.hpp"

namespace vip {

VipPlugin g_vipPlugin;

namespace {

// --- PITFALLS B1/B2: the map gate --------------------------------------------------------------
// g_mapReady: s2sdk client/entity/schema calls are safe right now.
// g_mapStartSeen: an OnMapStart arrived since OnPluginStart / the last OnMapEnd.
// g_mapEndSeen: an OnMapEnd arrived since OnPluginStart (only a new OnMapStart reopens the gate).
// g_mapListenersRegistered: all four map-listener bindings were registered by this plugin.
bool g_mapReady = false;
bool g_mapStartSeen = false;
bool g_mapEndSeen = false;
bool g_mapListenersRegistered = false;

constexpr std::string_view kPluginName = "vip";
constexpr std::string_view kEmptyMapName = "<empty>";

void LogMessageImpl(std::string_view message, plg::Severity severity) {
	if (plg::Log) {
		plg::Log(message, severity, plg::source_location::current());
	}
}

template <typename Callback>
void AddListener(std::vector<Callback>& list, Callback callback) {
	if (callback == nullptr) {
		return;
	}
	if (std::find(list.begin(), list.end(), callback) == list.end()) {
		list.push_back(callback);
	}
}

template <typename Callback>
void RemoveListener(std::vector<Callback>& list, Callback callback) {
	list.erase(std::remove(list.begin(), list.end(), callback), list.end());
}

// The 32 bit account id of a steamid64: the `account_id` column of the original.
uint32_t AccountId(uint64_t steamid64) {
	return static_cast<uint32_t>(steamid64 & 0xFFFFFFFFULL);
}

std::string ToStd(const plg::string& text) {
	return std::string(text.data(), text.size());
}

std::string JoinPath(std::string_view a, std::string_view b) {
	if (a.empty()) {
		return std::string(b);
	}
	if (b.empty()) {
		return std::string(a);
	}
	std::string out(a);
	if (out.back() != '/') {
		out.push_back('/');
	}
	if (b.front() == '/') {
		b.remove_prefix(1);
	}
	out.append(b);
	return out;
}

bool FileExists(std::string_view path) {
	if (path.empty()) {
		return false;
	}
	std::ifstream file{std::string(path), std::ios::binary};
	return file.good();
}

std::string FirstExisting(const std::vector<std::string>& candidates) {
	for (const std::string& candidate : candidates) {
		if (!candidate.empty() && FileExists(candidate)) {
			return candidate;
		}
	}
	return {};
}

// The plugify configs/data roots; empty when the host did not set the binding (the previous
// wrapper behaved the same way and fell back to relative paths).
std::string ConfigsRoot() {
	if (!plg::GetConfigsDir) {
		return {};
	}
	return std::string(std::string_view(plg::GetConfigsDir()));
}

std::string DataRoot() {
	if (!plg::GetDataDir) {
		return {};
	}
	return std::string(std::string_view(plg::GetDataDir()));
}

std::string ConfigDir() {
	const std::string root = ConfigsRoot();
	return root.empty() ? std::string(kPluginName) : JoinPath(root, kPluginName);
}

std::string TranslationsDir() {
	return JoinPath(ConfigDir(), "translations");
}

std::string DataDir() {
	const std::string root = DataRoot();
	return root.empty() ? std::string(kPluginName) : JoinPath(root, kPluginName);
}

// --- C2 client guards --------------------------------------------------------------------------
// Every s2sdk client query needs a running map (B1/B2). The helpers below check the map gate, the
// slot range and the specific binding before the call; a missing binding or an exception answers
// the neutral value.

bool SlotInRange(int32_t slot) {
	return slot >= 0 && slot < kMaxSlots;
}

bool ClientConnected(int32_t slot) {
	if (!g_mapReady || !SlotInRange(slot) || !__s2sdk_IsClientConnected) {
		return false;
	}
	try {
		return s2sdk::IsClientConnected(slot);
	} catch (...) {
		return false;
	}
}

bool ClientInGame(int32_t slot) {
	if (!g_mapReady || !SlotInRange(slot) || !__s2sdk_IsClientInGame) {
		return false;
	}
	try {
		return s2sdk::IsClientInGame(slot);
	} catch (...) {
		return false;
	}
}

bool ClientSourceTV(int32_t slot) {
	if (!g_mapReady || !SlotInRange(slot) || !__s2sdk_IsClientSourceTV) {
		return false;
	}
	try {
		return s2sdk::IsClientSourceTV(slot);
	} catch (...) {
		return false;
	}
}

bool ClientFake(int32_t slot) {
	if (!g_mapReady || !SlotInRange(slot) || !__s2sdk_IsFakeClient) {
		return false;
	}
	try {
		return s2sdk::IsFakeClient(slot);
	} catch (...) {
		return true; // treat as unavailable rather than as a real player
	}
}

bool ClientAuthorized(int32_t slot) {
	if (!g_mapReady || !SlotInRange(slot) || !__s2sdk_IsClientAuthorized) {
		return false;
	}
	try {
		return s2sdk::IsClientAuthorized(slot);
	} catch (...) {
		return false;
	}
}

bool ClientAlive(int32_t slot) {
	if (!g_mapReady || !SlotInRange(slot) || !__s2sdk_IsClientAlive) {
		return false;
	}
	try {
		return s2sdk::IsClientAlive(slot);
	} catch (...) {
		return false;
	}
}

// The validated controller entity handle of a slot: C1's rule (0 and -1 invalid, anything else
// checked by the engine) plus a non-null pointer and a real entity index.
int32_t ControllerHandle(int32_t slot) {
	if (!g_mapReady || !SlotInRange(slot)) {
		return 0;
	}
	if (!__s2sdk_PlayerSlotToEntPointer || !__s2sdk_PlayerSlotToEntHandle ||
		!__s2sdk_EntPointerToEntIndex || !__s2sdk_IsValidEntHandle) {
		return 0;
	}
	try {
		void* pointer = s2sdk::PlayerSlotToEntPointer(slot);
		if (pointer == nullptr) {
			return 0;
		}
		if (__s2sdk_IsValidEntPointer && !s2sdk::IsValidEntPointer(pointer)) {
			return 0;
		}
		const int32_t handle = s2sdk::PlayerSlotToEntHandle(slot);
		if (handle == 0 || handle == -1 || !s2sdk::IsValidEntHandle(handle)) {
			return 0;
		}
		const int32_t entityIndex = s2sdk::EntPointerToEntIndex(pointer);
		// An entity index, not a handle: the world is 0 and negative indices do not exist.
		if (entityIndex < 1) {
			return 0;
		}
		return handle;
	} catch (...) {
		return 0;
	}
}

// The engine steamid of a connected client; 0 for a slot without a client (s2sdk's own answer for
// such a slot keeps the previous occupant's id, which must not be compared).
uint64_t ClientSteamId64(int32_t slot) {
	if (!ClientConnected(slot) || !__s2sdk_GetClientSteamID64) {
		return 0;
	}
	try {
		return s2sdk::GetClientSteamID64(slot);
	} catch (...) {
		return 0;
	}
}

// The controller's name; s2sdk's "<unknown>" placeholder counts as "no name".
std::string ClientNameOf(int32_t slot) {
	if (ControllerHandle(slot) == 0 || !__s2sdk_GetClientName) {
		return {};
	}
	try {
		std::string name = ToStd(s2sdk::GetClientName(slot));
		if (name == "<unknown>") {
			name.clear();
		}
		return name;
	} catch (...) {
		return {};
	}
}

// The current pawn's entity index (an index, not a handle; 0 = none), validated in the same tick.
int32_t PawnIndex(int32_t slot) {
	if (!g_mapReady || !SlotInRange(slot) || !ClientConnected(slot)) {
		return 0;
	}
	if (!__s2sdk_GetClientPawn || !__s2sdk_EntPointerToEntIndex) {
		return 0;
	}
	try {
		void* pawn = s2sdk::GetClientPawn(slot);
		if (pawn == nullptr) {
			return 0;
		}
		if (__s2sdk_IsValidEntPointer && !s2sdk::IsValidEntPointer(pawn)) {
			return 0;
		}
		const int32_t index = s2sdk::EntPointerToEntIndex(pawn);
		return index < 1 ? 0 : index;
	} catch (...) {
		return 0;
	}
}

// A person the reconcile loop and the expiry checks work on: connected, authorized, not a bot.
// This is the layer the original's IsClientVIP used; it does not need the in-game/controller state
// (a client right after OnClientAuthenticated may not have a controller yet).
bool IsConnectedHuman(int32_t slot) {
	return g_mapReady && SlotInRange(slot) && ClientConnected(slot) && ClientAuthorized(slot) &&
		   !ClientFake(slot);
}

// --- map gate ------------------------------------------------------------------------------------

void PollMapGate() {
	// PITFALLS B1: a plugin loaded into a running map never saw OnMapStart, so its first update is
	// the "a map is up" signal - but only when the whole listener pair is registered (without the
	// map-end listener the gate could never close again), no OnMapEnd was seen since start and
	// s2sdk::GetCurrentMap() names a real map. The host keeps pumping updates through a
	// changelevel, so after an OnMapEnd only a new OnMapStart reopens the gate.
	if (g_mapReady || !g_mapListenersRegistered || g_mapEndSeen) {
		return;
	}
	if (!GameMapLoaded()) {
		return;
	}
	g_mapReady = true;
}

void OnMapStartListener() {
	try {
		g_vipPlugin.HandleMapStart();
	} catch (...) {
	}
}

void OnMapEndListener() {
	try {
		g_vipPlugin.HandleMapEnd();
	} catch (...) {
	}
}

void OnClientAuthenticatedTrampoline(int32_t slot, uint64_t steamid) {
	try {
		g_vipPlugin.HandleClientAuthenticated(slot, steamid);
	} catch (const std::exception& ex) {
		LogError(std::string("vip: exception in OnClientAuthenticated: ") + ex.what());
	} catch (...) {
		LogError("vip: unknown exception in OnClientAuthenticated");
	}
}

void OnClientPutInServerTrampoline(int32_t slot) {
	try {
		g_vipPlugin.HandleClientPutInServer(slot);
	} catch (const std::exception& ex) {
		LogError(std::string("vip: exception in OnClientPutInServer: ") + ex.what());
	} catch (...) {
		LogError("vip: unknown exception in OnClientPutInServer");
	}
}

void OnClientDisconnectTrampoline(int32_t slot, s2sdk::NetworkDisconnectionReason reason) {
	(void)reason;
	try {
		g_vipPlugin.HandleClientDisconnect(slot);
	} catch (const std::exception& ex) {
		LogError(std::string("vip: exception in OnClientDisconnect: ") + ex.what());
	} catch (...) {
		LogError("vip: unknown exception in OnClientDisconnect");
	}
}

void OnUserLoadedTrampoline(int64_t pluginId, uint64_t targetId, permissions::PlayerState state) {
	(void)pluginId;
	(void)state;
	try {
		g_vipPlugin.HandleUserLoaded(targetId);
	} catch (const std::exception& ex) {
		LogError(std::string("vip: exception in the permissions OnUserLoaded listener: ") + ex.what());
	} catch (...) {
		LogError("vip: unknown exception in the permissions OnUserLoaded listener");
	}
}

// PITFALLS C13: a Pre hook always answers Continue (the ported callback cannot veto an event).
s2sdk::ResultType OnPlayerSpawnEvent(const plg::string& name, void* event, bool dontBroadcast) {
	(void)name;
	(void)dontBroadcast;
	try {
		g_vipPlugin.HandlePlayerSpawn(event);
	} catch (const std::exception& ex) {
		LogError(std::string("vip: exception in the player_spawn handler: ") + ex.what());
	} catch (...) {
		LogError("vip: unknown exception in the player_spawn handler");
	}
	return s2sdk::ResultType::Continue;
}

s2sdk::ResultType OnRoundPreStartEvent(const plg::string& name, void* event, bool dontBroadcast) {
	(void)name;
	(void)event;
	(void)dontBroadcast;
	try {
		g_vipPlugin.HandleRoundPreStart();
	} catch (const std::exception& ex) {
		LogError(std::string("vip: exception in the round_prestart handler: ") + ex.what());
	} catch (...) {
		LogError("vip: unknown exception in the round_prestart handler");
	}
	return s2sdk::ResultType::Continue;
}

} // namespace

// --- logging / small shared helpers ------------------------------------------------------

void LogMessage(std::string_view message, plg::Severity severity) {
	LogMessageImpl(message, severity);
}

void LogInfo(std::string_view message) {
	LogMessage(message, plg::Severity::Info);
}

void LogWarn(std::string_view message) {
	LogMessage(message, plg::Severity::Warning);
}

void LogError(std::string_view message) {
	LogMessage(message, plg::Severity::Error);
}

bool IsGameReady() {
	return g_mapReady;
}

bool GameMapLoaded() {
	if (__s2sdk_GetCurrentMap == nullptr) {
		return false;
	}
	try {
		const std::string map = ToStd(s2sdk::GetCurrentMap());
		return !map.empty() && std::string_view(map) != kEmptyMapName;
	} catch (...) {
		return false;
	}
}

std::string FormatTimestamp(int64_t timestamp) {
	// PARITY: the original formatted local time with std::strftime as "%Y-%m-%d %H:%M:%S".
	const std::time_t seconds = static_cast<std::time_t>(timestamp);
	std::tm local{};
	if (localtime_r(&seconds, &local) == nullptr) {
		return {};
	}
	char buffer[80];
	if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &local) == 0) {
		return {};
	}
	return std::string(buffer);
}

std::string CookieToString(const plg::any& value) {
	// The core writes strings ("0"/"1"); the other scalar forms are accepted so a cookie set by
	// another plugin (or by the Metamod Pisex cookie store) still reads back.
	if (plg::holds_alternative<plg::string>(value)) {
		return ToStd(plg::get<plg::string>(value));
	}
	if (plg::holds_alternative<bool>(value)) {
		return plg::get<bool>(value) ? "1" : "0";
	}
	if (plg::holds_alternative<int64_t>(value)) {
		return std::to_string(plg::get<int64_t>(value));
	}
	if (plg::holds_alternative<int32_t>(value)) {
		return std::to_string(plg::get<int32_t>(value));
	}
	if (plg::holds_alternative<uint64_t>(value)) {
		return std::to_string(plg::get<uint64_t>(value));
	}
	if (plg::holds_alternative<uint32_t>(value)) {
		return std::to_string(plg::get<uint32_t>(value));
	}
	if (plg::holds_alternative<double>(value)) {
		return std::to_string(plg::get<double>(value));
	}
	if (plg::holds_alternative<float>(value)) {
		return std::to_string(plg::get<float>(value));
	}
	return {};
}

void ChatLine(int32_t slot, std::string_view text) {
	if (!g_mapReady || !SlotInRange(slot) || !__s2sdk_PrintToChat) {
		return;
	}
	if (!ClientConnected(slot)) {
		return;
	}
	try {
		s2sdk::PrintToChat(slot, plg::string(Colorize(text)));
	} catch (...) {
	}
}

void ServerConsole(std::string_view text) {
	const std::string plain = StripColorTags(text);
	if (__s2sdk_PrintToServer) {
		try {
			s2sdk::PrintToServer(plg::string(plain));
			return;
		} catch (...) {
		}
	}
	LogInfo(plain);
}

// --- configuration path / parsing -------------------------------------------------------------

std::string VipPlugin::ResolveConfigPath(std::string_view path) const {
	// The previous wrapper's candidate list: the path as given (relative to the server working
	// directory), then the plugin's config dir, translations dir and data dir. The first existing
	// file is authoritative.
	if (path.empty()) {
		return {};
	}
	const std::string raw(path);
	const bool absolute = raw.front() == '/';
	std::vector<std::string> candidates{raw};
	if (absolute) {
		return FirstExisting(candidates);
	}

	std::string_view relative = path;
	bool fromTranslations = false;
	bool fromData = false;
	constexpr std::string_view kConfigsPrefix = "addons/configs/";
	constexpr std::string_view kTranslationsPrefix = "addons/translations/";
	constexpr std::string_view kDataPrefix = "addons/data/";
	constexpr std::string_view kAddonsPrefix = "addons/";
	if (path.size() >= kConfigsPrefix.size() && path.compare(0, kConfigsPrefix.size(), kConfigsPrefix) == 0) {
		relative = path.substr(kConfigsPrefix.size());
		if (relative.size() > kPluginName.size() && relative.compare(0, kPluginName.size(), kPluginName) == 0 &&
			relative[kPluginName.size()] == '/') {
			relative = relative.substr(kPluginName.size() + 1);
		}
	} else if (path.size() >= kTranslationsPrefix.size() &&
			   path.compare(0, kTranslationsPrefix.size(), kTranslationsPrefix) == 0) {
		relative = path.substr(kTranslationsPrefix.size());
		fromTranslations = true;
	} else if (path.size() >= kDataPrefix.size() && path.compare(0, kDataPrefix.size(), kDataPrefix) == 0) {
		relative = path.substr(kDataPrefix.size());
		fromData = true;
	} else if (path.size() >= kAddonsPrefix.size() && path.compare(0, kAddonsPrefix.size(), kAddonsPrefix) == 0) {
		relative = path.substr(kAddonsPrefix.size());
	}

	const std::string rel(relative);
	if (fromTranslations) {
		candidates.push_back(JoinPath(TranslationsDir(), rel));
	} else if (fromData) {
		candidates.push_back(JoinPath(DataDir(), rel));
	} else {
		candidates.push_back(JoinPath(ConfigDir(), rel));
		candidates.push_back(JoinPath(TranslationsDir(), rel));
		candidates.push_back(JoinPath(DataDir(), rel));
	}
	return FirstExisting(candidates);
}

std::string VipPlugin::ResolveTranslationsPath(std::string_view path) const {
	// Pisex's LoadTranslations() takes "name.phrases" and appends ".txt".
	std::string file(path);
	if (file.size() < 4 || file.compare(file.size() - 4, 4, ".txt") != 0) {
		file += ".txt";
	}
	const std::string direct = FirstExisting({file, JoinPath(TranslationsDir(), file), JoinPath(ConfigDir(), file)});
	if (!direct.empty()) {
		return direct;
	}
	std::string_view relative = file;
	constexpr std::string_view kTranslationsPrefix = "addons/translations/";
	if (file.size() >= kTranslationsPrefix.size() &&
		file.compare(0, kTranslationsPrefix.size(), kTranslationsPrefix) == 0) {
		relative = std::string_view(file).substr(kTranslationsPrefix.size());
	}
	return JoinPath(TranslationsDir(), relative);
}

std::string VipPlugin::ResolveDataPath(std::string_view path) const {
	std::string file(path);
	constexpr std::string_view kDataPrefix = "addons/data/";
	if (file.size() >= kDataPrefix.size() && file.compare(0, kDataPrefix.size(), kDataPrefix) == 0) {
		file = file.substr(kDataPrefix.size());
	}
	return JoinPath(DataDir(), file);
}

bool VipPlugin::LoadDatabaseConfig() {
	// The "vip" section of databases.cfg, the same file and section name the original read. A
	// missing file or section falls back to a local SQLite file so the plugin still works without
	// a database server (PLUGIN_STANDARD §4, fail soft).
	plgdb::Config fallback;
	fallback.driver = plgdb::Driver::SQLite;
	fallback.sqlitePath = ResolveDataPath("vip.sqlite3");

	const std::string path = ResolveConfigPath("addons/configs/databases.cfg");
	if (path.empty() || !FileExists(path)) {
		LogError("vip: cannot read databases.cfg - using a local SQLite database");
		m_db_config = std::move(fallback);
		return false;
	}
	const std::optional<kv::KvNode> kvFile = kv::LoadKv1File(path);
	if (!kvFile.has_value()) {
		LogError("vip: databases.cfg is malformed - using a local SQLite database");
		m_db_config = std::move(fallback);
		return false;
	}
	const kv::KvNode* section = kvFile->Find("vip");
	if (section == nullptr) {
		LogError("vip: databases.cfg has no 'vip' section - using a local SQLite database");
		m_db_config = std::move(fallback);
		return false;
	}

	plgdb::Config config;
	const std::string driver(section->GetString("driver", "mysql"));
	config.host = std::string(section->GetString("host", "127.0.0.1"));
	config.port = static_cast<uint16_t>(section->GetInt("port", 3306));
	config.user = std::string(section->GetString("user", ""));
	config.password = std::string(section->GetString("pass", ""));
	config.database = std::string(section->GetString("database", ""));
	const std::string sqlitePath(section->GetString("sqlite_path", "vip.sqlite3"));
	if (driver == "sqlite") {
		config.driver = plgdb::Driver::SQLite;
		config.sqlitePath = sqlitePath;
	} else {
		config.driver = plgdb::Driver::MySQL;
	}
	m_db_config = std::move(config);
	return true;
}

bool VipPlugin::LoadGroups() {
	// LoadVips() of the original: server_id + the group blocks of groups.ini. A file that cannot
	// be read leaves the previous table in place (the original cleared it first and then failed;
	// keeping the old groups is the safe direction for a running server - deliberate deviation).
	const std::string path = ResolveConfigPath("addons/configs/vip/groups.ini");
	if (path.empty() || !FileExists(path)) {
		LogError("vip: failed to load the vip config 'addons/configs/vip/groups.ini'");
		return false;
	}
	const std::optional<kv::KvNode> parsed = kv::LoadKv1File(path);
	if (!parsed.has_value()) {
		LogError("vip: groups.ini is malformed - keeping the previous groups");
		return false;
	}

	Config config = m_config;
	config.server_id = parsed->GetInt("server_id", 0);

	GroupTable groups;
	// The original's key walk advanced from the current key. Advancing from the root instead
	// stopped after the first child - the "server_id" value - so no group was ever loaded and
	// every VIP got "NotAccess".
	std::size_t groupCount = 0;
	for (const kv::KvNode& key : parsed->children) {
		GroupFeatures group;
		for (const kv::KvNode& value : key.children) {
			if (value.hasValue) {
				group[value.name] = value.value;
			}
		}
		if (!group.empty()) {
			++groupCount; // "server_id" is a plain value, not a group
		}
		groups[key.name] = std::move(group);
	}
	LogInfo("vip: " + std::to_string(groupCount) + " group(s) loaded from groups.ini (server_id " +
			std::to_string(config.server_id) + ")");

	m_config = config;
	if (m_core != nullptr) {
		m_core->SetConfig(m_config);
		m_core->SetGroups(std::move(groups));
	}
	return true;
}

void VipPlugin::LoadPhrasesLanguage() {
	// PARITY: the previous port read the phrase file's own "language" key and, only when it is
	// empty, detected the server language (core.cfg "ServerLang" -> s2sdk's ServerLanguage -> en).
	std::string language;
	const std::string languagePath = ResolveConfigPath("addons/translations/vip.phrases.txt");
	if (!languagePath.empty() && FileExists(languagePath)) {
		const std::optional<kv::KvNode> parsed = kv::LoadKv1File(languagePath);
		if (parsed.has_value()) {
			const std::string_view value = parsed->GetString("language", "");
			if (!value.empty()) {
				language = NormalizeLanguage(value);
			}
		}
	}
	if (language.empty()) {
		// The plugify equivalent of the shared addons/configs/core.cfg.
		const std::string configs = ConfigsRoot();
		if (!configs.empty()) {
			const std::optional<kv::KvNode> core = kv::LoadKv1File(JoinPath(configs, "core.cfg"));
			if (core.has_value()) {
				const std::string_view value = core->GetString("ServerLang", "");
				if (!value.empty()) {
					language = NormalizeLanguage(value);
				}
			}
		}
	}
	if (language.empty() && __s2sdk_GetServerLanguage) {
		try {
			const plg::string serverLanguage = s2sdk::GetServerLanguage();
			if (!serverLanguage.empty()) {
				language = NormalizeLanguage(ToStd(serverLanguage));
			}
		} catch (...) {
		}
	}
	if (language.empty()) {
		language = "en";
	}
	m_language = std::move(language);
}

bool VipPlugin::LoadPhrases() {
	// The g_vecPhrases map of the original, from vip.phrases.txt (the same file the package
	// ships), resolved for one language with the "en" fallback.
	LoadPhrasesLanguage();
	const std::string path = ResolveTranslationsPath("vip.phrases.txt");
	if (path.empty() || !FileExists(path)) {
		LogError("vip: failed to load 'vip.phrases.txt' - phrases print as empty strings");
		return false;
	}
	const std::optional<kv::KvNode> parsed = kv::LoadKv1File(path);
	if (!parsed.has_value()) {
		LogError("vip: 'vip.phrases.txt' is malformed - phrases print as empty strings");
		return false;
	}
	m_phrases = kv::ResolvePhrases(*parsed, m_language);
	return true;
}

std::string VipPlugin::Phrase(std::string_view key) const {
	const auto found = m_phrases.find(std::string(key));
	return found == m_phrases.end() ? std::string() : found->second;
}

// --- startup ---------------------------------------------------------------------------------

void VipPlugin::ResetState() {
	m_config = Config{};
	m_db_config = plgdb::Config{};
	m_db.reset();
	m_core.reset();
	m_phrases.clear();
	m_language.clear();
	m_running = false;
	m_ready = false;
	m_startup_pending = false;
	m_permissions_listener_registered = false;
	m_permissions_warned = false;
	m_auth_listener = false;
	m_put_in_server_listener = false;
	m_disconnect_listener = false;
	m_spawn_event_hooked = false;
	m_prestart_event_hooked = false;
	m_map_listeners_registered = false;
	m_core_retry_wait = false;
	m_core_starting = false;
	m_core_promote = false;
	m_core_drop = false;
	m_core_retry_at = {};
	m_core_retry_delay = kCoreRetryAfter;
	m_features.clear();
	m_slots = {};
	m_pending_spawns.clear();
	m_pistol_round = true;
	m_second_tick.Reset();
	m_database_tick.Reset();
	m_database_tick.DueNow();
	m_offset_warmup = -1;
	m_offset_switching = -1;
	m_offset_overtime = -1;
	m_offset_restart = -1;
	m_offset_total_rounds = -1;
	m_offset_team = -1;
	// The callback registries are deliberately NOT cleared: a consumer plugin may have
	// registered through the exported API before this OnPluginStart ran, and `plg reload`
	// reloads the whole library (fresh statics) anyway.
	g_mapReady = false;
	g_mapStartSeen = false;
	g_mapEndSeen = false;
	g_mapListenersRegistered = false;
}

bool VipPlugin::CreateCore() {
	// A missing or unusable databases.cfg means a local SQLite file (a fail-soft fallback); only
	// a config that cannot produce a connection stops the core.
	LoadDatabaseConfig();
	if (m_db_config.driver == plgdb::Driver::SQLite && !m_db_config.sqlitePath.empty() &&
		m_db_config.sqlitePath != ":memory:") {
		// The plugin writes only into its data directory (PLUGIN_STANDARD §2); making the parent
		// of its SQLite file keeps a fresh deployment working even when the directory was not
		// created yet.
		std::error_code ec;
		const std::filesystem::path parent = std::filesystem::path(m_db_config.sqlitePath).parent_path();
		if (!parent.empty()) {
			std::filesystem::create_directories(parent, ec);
		}
	}
	m_db = plgdb::Database::Create(m_db_config);
	if (m_db == nullptr) {
		LogError("vip: the database configuration is invalid");
		return false;
	}

	m_core = std::make_unique<VipCore>(*m_db);
	m_core->onLoaded = [](int32_t slot, const VipLoadOutcome& outcome, LoadPurpose purpose) {
		g_vipPlugin.HandleLoaded(slot, outcome, purpose);
	};
	m_core->onError = [](std::string_view message) { g_vipPlugin.HandleCoreError(message); };

	LoadGroups(); // sets m_config.server_id
	return true;
}

bool VipPlugin::EnsureCore() {
	if (!CreateCore()) {
		return false;
	}

	try {
		m_core->CreateSchema();
	} catch (const std::exception& ex) {
		LogError(std::string("vip: ") + ex.what());
		m_core.reset();
		m_db.reset();
		return false;
	}

	m_ready = true;
	m_database_tick.DueNow();
	// PARITY: the OnVIPLoaded callbacks fire and the core goes ready once the CREATE TABLE ran.
	FireReady();
	LogInfo("vip: core ready (server_id " + std::to_string(m_config.server_id) + ")");
	return true;
}

void VipPlugin::ScheduleCoreRetry() {
	m_core_retry_at = std::chrono::steady_clock::now() + m_core_retry_delay;
	m_core_retry_delay = std::min(m_core_retry_delay * 2, kCoreRetryMax);
}

void VipPlugin::RetryCore() {
	// Reuse the objects when an earlier attempt could not be torn down (its worker still had a
	// statement queued); otherwise re-read databases.cfg, so a fixed file is picked up.
	if (m_core == nullptr && !CreateCore()) {
		ScheduleCoreRetry();
		return;
	}
	m_core_starting = true;
	m_core->CreateSchemaAsync([this](bool ok, const std::string& error) {
		m_core_starting = false;
		if (!ok) {
			LogError("vip: creating the vip_users table failed: " + error);
			// Both the teardown and the promotion happen in the update pump, after Poll() returned:
			// destroying the Database object inside its own Poll() callback would pull the object
			// out from under the loop that is running it.
			m_core_drop = true;
			return;
		}
		m_core_promote = true;
	});
}

void VipPlugin::DropFailedCore() {
	// Drop the connection so the next attempt re-reads databases.cfg. Only when the worker has
	// nothing queued: ~Database joins the worker, and a database that is down holds every
	// statement for the connect timeout.
	if (m_db == nullptr || m_db->Pending() == 0) {
		m_core.reset();
		m_db.reset();
	}
}

void VipPlugin::PromoteRetriedCore() {
	m_core_promote = false;
	m_core_retry_wait = false;
	m_core_retry_delay = kCoreRetryAfter;
	m_ready = true;
	m_database_tick.DueNow();
	// PARITY: the OnVIPLoaded callbacks fire and the core goes ready once the CREATE TABLE ran.
	FireReady();
	LogInfo("vip: core ready (server_id " + std::to_string(m_config.server_id) + ")");
	// What OnPluginStart skipped when the core failed there.
	m_startup_pending = true;
	LogInfo("vip: plugin started after the database became available");
}

void VipPlugin::EnsurePermissionsListener() {
	// The permissions plugin is an optional dependency, so this may run before (or without) it:
	// it is retried from the update pump until the complete register+unregister pair is there. A
	// listener that could not be removed again would point into this library after unload (A7).
	if (m_permissions_listener_registered) {
		return;
	}
	if (__permissions_OnUserLoaded_Register == nullptr || __permissions_OnUserLoaded_Unregister == nullptr) {
		if (!m_permissions_warned) {
			m_permissions_warned = true;
			LogError("vip: the permissions plugin is not available - cookies are disabled until it loads");
		}
		return;
	}
	m_permissions_listener_registered = true;
	try {
		if (permissions::OnUserLoaded_Register(&OnUserLoadedTrampoline) != permissions::Status::Success) {
			m_permissions_listener_registered = false;
			if (!m_permissions_warned) {
				m_permissions_warned = true;
				LogError("vip: the permissions plugin rejected the OnUserLoaded listener - cookies stay off");
			}
			return;
		}
		LogInfo("vip: the permissions cookie store is available");
	} catch (const std::exception& ex) {
		m_permissions_listener_registered = false;
		if (!m_permissions_warned) {
			m_permissions_warned = true;
			LogError(std::string("vip: registering the permissions OnUserLoaded listener failed: ") + ex.what());
		}
	} catch (...) {
		m_permissions_listener_registered = false;
		if (!m_permissions_warned) {
			m_permissions_warned = true;
			LogError("vip: registering the permissions OnUserLoaded listener failed");
		}
	}
}

void VipPlugin::ReleasePermissionsListener() {
	if (!m_permissions_listener_registered) {
		return;
	}
	m_permissions_listener_registered = false;
	if (__permissions_OnUserLoaded_Unregister == nullptr) {
		return;
	}
	try {
		permissions::OnUserLoaded_Unregister(&OnUserLoadedTrampoline);
	} catch (...) {
	}
}

void VipPlugin::RegisterMapListeners() {
	if (m_map_listeners_registered) {
		return;
	}
	// A7: a callback that cannot be unregistered would point into this library after unload. The
	// whole register/unregister pair of BOTH listeners is required before anything is registered;
	// a partial pair fails soft and leaves the plugin inactive (without the map-end listener the
	// gate could never close again anyway).
	if (__s2sdk_OnMapStart_Register == nullptr || __s2sdk_OnMapEnd_Register == nullptr ||
		__s2sdk_OnMapStart_Unregister == nullptr || __s2sdk_OnMapEnd_Unregister == nullptr) {
		LogError("vip: the s2sdk map listeners are unavailable (both register and unregister bindings are "
				 "required) - the plugin stays inactive");
		return;
	}
	try {
		s2sdk::OnMapStart_Register(&OnMapStartListener);
		s2sdk::OnMapEnd_Register(&OnMapEndListener);
	} catch (...) {
		try {
			s2sdk::OnMapStart_Unregister(&OnMapStartListener);
		} catch (...) {
		}
		try {
			s2sdk::OnMapEnd_Unregister(&OnMapEndListener);
		} catch (...) {
		}
		LogError("vip: registering the s2sdk map listeners failed - the plugin stays inactive");
		return;
	}
	m_map_listeners_registered = true;
	g_mapListenersRegistered = true;
}

void VipPlugin::UnregisterMapListeners() {
	if (!m_map_listeners_registered) {
		return;
	}
	m_map_listeners_registered = false;
	g_mapListenersRegistered = false;
	if (__s2sdk_OnMapStart_Unregister != nullptr) {
		try {
			s2sdk::OnMapStart_Unregister(&OnMapStartListener);
		} catch (...) {
		}
	}
	if (__s2sdk_OnMapEnd_Unregister != nullptr) {
		try {
			s2sdk::OnMapEnd_Unregister(&OnMapEndListener);
		} catch (...) {
		}
	}
}

void VipPlugin::RegisterClientListeners() {
	// Every listener needs its complete register+unregister pair before it is installed (A7).
	const bool authPair = __s2sdk_OnClientAuthenticated_Register != nullptr &&
						  __s2sdk_OnClientAuthenticated_Unregister != nullptr;
	const bool putInServerPair = __s2sdk_OnClientPutInServer_Register != nullptr &&
								 __s2sdk_OnClientPutInServer_Unregister != nullptr;
	const bool disconnectPair = __s2sdk_OnClientDisconnect_Register != nullptr &&
								__s2sdk_OnClientDisconnect_Unregister != nullptr;
	if (!authPair) {
		LogError("vip: the OnClientAuthenticated listener pair is unavailable - only the once-per-second "
				 "reconcile loads VIP rows");
	}
	if (!putInServerPair) {
		LogError("vip: the OnClientPutInServer listener pair is unavailable - no welcome on join");
	}
	if (!disconnectPair) {
		LogError("vip: the OnClientDisconnect listener pair is unavailable - VIP_OnClientDisconnect fires only "
				 "from the reconcile");
	}
	try {
		if (authPair) {
			s2sdk::OnClientAuthenticated_Register(&OnClientAuthenticatedTrampoline);
			m_auth_listener = true;
		}
		if (putInServerPair) {
			s2sdk::OnClientPutInServer_Register(&OnClientPutInServerTrampoline);
			m_put_in_server_listener = true;
		}
		if (disconnectPair) {
			s2sdk::OnClientDisconnect_Register(&OnClientDisconnectTrampoline);
			m_disconnect_listener = true;
		}
	} catch (...) {
		UnregisterClientListeners();
		LogError("vip: registering the s2sdk client listeners failed");
	}
}

void VipPlugin::UnregisterClientListeners() {
	if (m_auth_listener && __s2sdk_OnClientAuthenticated_Unregister != nullptr) {
		try {
			s2sdk::OnClientAuthenticated_Unregister(&OnClientAuthenticatedTrampoline);
		} catch (...) {
		}
	}
	if (m_put_in_server_listener && __s2sdk_OnClientPutInServer_Unregister != nullptr) {
		try {
			s2sdk::OnClientPutInServer_Unregister(&OnClientPutInServerTrampoline);
		} catch (...) {
		}
	}
	if (m_disconnect_listener && __s2sdk_OnClientDisconnect_Unregister != nullptr) {
		try {
			s2sdk::OnClientDisconnect_Unregister(&OnClientDisconnectTrampoline);
		} catch (...) {
		}
	}
	m_auth_listener = false;
	m_put_in_server_listener = false;
	m_disconnect_listener = false;
}

void VipPlugin::RegisterEvents() {
	// PITFALLS C13/A7: both the hook and the unhook binding must exist before the first hook.
	if (__s2sdk_HookEvent == nullptr || __s2sdk_UnhookEvent == nullptr) {
		LogError("vip: the s2sdk event bindings are unavailable - player_spawn and round_prestart are not hooked");
		return;
	}
	try {
		if (s2sdk::HookEvent(plg::string("player_spawn"), &OnPlayerSpawnEvent, s2sdk::HookMode::Pre) ==
			s2sdk::EventHookError::Okay) {
			m_spawn_event_hooked = true;
		} else {
			LogError("vip: hooking player_spawn was rejected");
		}
	} catch (...) {
		LogError("vip: hooking player_spawn failed");
	}
	try {
		if (s2sdk::HookEvent(plg::string("round_prestart"), &OnRoundPreStartEvent, s2sdk::HookMode::Pre) ==
			s2sdk::EventHookError::Okay) {
			m_prestart_event_hooked = true;
		} else {
			LogError("vip: hooking round_prestart was rejected");
		}
	} catch (...) {
		LogError("vip: hooking round_prestart failed");
	}
}

void VipPlugin::UnregisterEvents() {
	if (__s2sdk_UnhookEvent == nullptr) {
		m_spawn_event_hooked = false;
		m_prestart_event_hooked = false;
		return;
	}
	if (m_spawn_event_hooked) {
		try {
			s2sdk::UnhookEvent(plg::string("player_spawn"), &OnPlayerSpawnEvent, s2sdk::HookMode::Pre);
		} catch (...) {
		}
	}
	if (m_prestart_event_hooked) {
		try {
			s2sdk::UnhookEvent(plg::string("round_prestart"), &OnRoundPreStartEvent, s2sdk::HookMode::Pre);
		} catch (...) {
		}
	}
	m_spawn_event_hooked = false;
	m_prestart_event_hooked = false;
}

void VipPlugin::RegisterCommandsAndEvents() {
	RegisterCommands();
	RegisterEvents();
}

plg::PluginResult VipPlugin::OnPluginStart() {
	try {
		ResetState();
		LoadPhrases();
		EnsurePermissionsListener();

		// The listeners come before the core: the map gate and the handshake of a player are
		// independent of the database, and a core that is not reachable yet is retried below.
		RegisterMapListeners();
		RegisterClientListeners();
		RegisterCommandsAndEvents();
		SetupMenus();

		if (!EnsureCore()) {
			// Fail soft: the plugin stays loaded, the API answers with neutral values, and the
			// update pump retries the core - a database that starts after the game server (a
			// daily reboot, a network hiccup) is picked up without a plg reload.
			LogError("vip: the core is not ready - the plugin runs as a no-op until the database answers");
			m_core_retry_wait = true;
			m_core_retry_at = std::chrono::steady_clock::now() + kCoreRetryAfter;
			m_running = true;
			return plg::PluginResult{};
		}

		// OnStartupServer of the original: the expired rows of this server are deleted once the
		// first update runs (the statement is asynchronous; no client call is involved).
		m_startup_pending = true;
		m_running = true;
		LogInfo("vip: plugin started");
	} catch (const std::exception& ex) {
		LogError(std::string("vip: exception in OnPluginStart: ") + ex.what());
	} catch (...) {
		LogError("vip: unknown exception in OnPluginStart");
	}
	return plg::PluginResult{}; // never Failed (PITFALLS A1)
}

plg::PluginResult VipPlugin::OnPluginUpdate(std::chrono::milliseconds dt) {
	try {
		if (!m_running) {
			return {};
		}
		// The plgdb pump: results are only applied from Poll() on the game thread, and the
		// accumulator keeps it at ~10 ms without an s2sdk timer (C9). DueNow() after startup and
		// after every CREATE TABLE result means the first callback runs on the first update.
		if (m_db != nullptr && m_database_tick.Feed(dt) > 0) {
			m_db->Poll();
		}
		// PITFALLS B1: the mid-map latch (all four map-listener bindings + no OnMapEnd + a real
		// map name). Nothing client-related below runs while the gate is shut.
		PollMapGate();
		// The optional permissions dependency may appear after this plugin started.
		EnsurePermissionsListener();
		if (m_core_retry_wait) {
			if (m_core_promote) {
				PromoteRetriedCore();
			} else if (m_core_drop) {
				m_core_drop = false;
				DropFailedCore();
				ScheduleCoreRetry();
			} else if (!m_core_starting && std::chrono::steady_clock::now() >= m_core_retry_at) {
				RetryCore();
			}
		}
		if (m_startup_pending && m_ready) {
			m_startup_pending = false;
			OnStartupServer();
		}
		if (!g_mapReady) {
			return {};
		}
		// The one-second loop of the original (reconcile + expiry), driven by the accumulator.
		if (m_second_tick.Feed(dt) > 0) {
			OnSecondTick();
		}
		ProcessPendingSpawns(dt);
		ProcessMenuWork();
	} catch (const std::exception& ex) {
		LogError(std::string("vip: exception in OnPluginUpdate: ") + ex.what());
	} catch (...) {
		LogError("vip: unknown exception in OnPluginUpdate");
	}
	return plg::PluginResult{};
}

plg::PluginResult VipPlugin::OnPluginEnd() {
	try {
		m_running = false;
		// A7/C11/C12: everything that was hooked or created is removed again, in the reverse order
		// of the registrations; each step is guarded on its own so one throwing teardown cannot
		// keep the rest registered.
		RemoveCommands();
		UnregisterEvents();
		UnregisterClientListeners();
		UnregisterMapListeners();
		ReleasePermissionsListener();
		// Menus go before the gate closes: the teardown needs the "is the map up" answer to know
		// whether a backend close may touch a client.
		TeardownMenusOnUnload();
		m_pending_spawns.clear();
		g_mapReady = false;
		g_mapStartSeen = false;
		g_mapEndSeen = true;
		m_second_tick.Reset();
		m_database_tick.Reset();
		// The core has to go before the database (its callbacks hold a lifetime token).
		m_core.reset();
		m_db.reset();
		m_ready = false;
		m_startup_pending = false;
		m_core_retry_wait = false;
		m_core_starting = false;
		m_core_promote = false;
		m_core_drop = false;
		m_slots = {};
		m_features.clear();
		LogInfo("vip: unloaded");
	} catch (const std::exception& ex) {
		LogError(std::string("vip: exception in OnPluginEnd: ") + ex.what());
	} catch (...) {
		LogError("vip: unknown exception in OnPluginEnd");
	}
	return plg::PluginResult{};
}

// --- map lifecycle --------------------------------------------------------------------------

void VipPlugin::HandleMapStart() {
	// The engine calls this once the new map and its client list exist: every client call is legal
	// from here on.
	g_mapReady = true;
	g_mapStartSeen = true;
	g_mapEndSeen = false;
	m_pending_spawns.clear();
	m_pistol_round = true;
	m_database_tick.DueNow();
}

void VipPlugin::HandleMapEnd() {
	// PITFALLS B2: from here until the next OnMapStart the client list is torn down and any client
	// query crashes - only state changes happen here. The menu teardown uses the s2sdk menu
	// manager with the "Disconnect" reason, which skips every backend close that would touch a
	// client.
	g_mapReady = false;
	g_mapStartSeen = false;
	g_mapEndSeen = true;
	m_pending_spawns.clear();
	TeardownMenusOnMapEnd();
}

// --- engine side ------------------------------------------------------------------------------

void VipPlugin::HandleClientAuthenticated(int32_t slot, uint64_t steamid) {
	if (!g_mapReady || !m_ready || !SlotInRange(slot) || steamid == 0) {
		return;
	}
	SlotState& state = m_slots[static_cast<size_t>(slot)];
	if (state.requested && state.steamid == steamid) {
		return; // already requested (the reconcile may have been faster than the event)
	}
	// A new connection in this slot: forget the previous player's state. The vip_users record of
	// the old player stays in the core (g_VipPlayer of the original did not clean up either).
	state = SlotState{};
	state.steamid = steamid;
	state.account_id = AccountId(steamid);
	state.name = PlayerName(slot);
	state.requested = true;
	state.loading = true;
	state.requested_at = std::chrono::steady_clock::now();
	m_core->LoadPlayer(slot, IdentityOf(slot), LoadPurpose::Connect);
}

void VipPlugin::HandleClientPutInServer(int32_t slot) {
	// OnClientPutInServer of the original: when the record of the player is already known (a
	// reconnect in the same session) the Welcome message is printed right away.
	if (!IsConnectedHuman(slot)) {
		return;
	}
	const SlotState& state = m_slots[static_cast<size_t>(slot)];
	const uint64_t steamid = state.steamid != 0 ? state.steamid : ClientSteamId64(slot);
	if (steamid == 0 || m_core == nullptr) {
		return;
	}
	const VipPlayer* player = m_core->FindPlayer(AccountId(steamid));
	if (player == nullptr) {
		return;
	}
	if (m_core->IsValidGroup(player->group)) {
		Welcome(slot, player->group, player->time_end);
	}
}

void VipPlugin::HandleClientDisconnect(int32_t slot) {
	if (!SlotInRange(slot)) {
		return;
	}
	if (m_core != nullptr) {
		// s2sdk's OnClientDisconnect runs before the client data is reset, but the check
		// deliberately uses the tracked slot state instead of the live engine queries: the tracked
		// identity is the one the VIP flags were loaded for, regardless of how far the engine
		// teardown has progressed. No client/entity call is made here (the map may be ending).
		const uint32_t accountId = AccountIdOf(slot);
		const bool wasVip = accountId != 0 && m_core->IsClientVip(TrackedIdentityOf(slot), m_core->Now());
		FireClientDisconnect(slot, wasVip);
	}
	ClearSlot(slot);
}

void VipPlugin::QueuePlayerSpawn(int32_t slot, uint64_t steamid, int32_t controller) {
	// The original deferred the spawn work by one frame with NextFrame(); this queue does the same
	// from OnPluginUpdate. The bound keeps a broken event stream from growing it without limit.
	if (m_pending_spawns.size() >= static_cast<size_t>(kMaxSlots) * 2) {
		return;
	}
	m_pending_spawns.push_back(PendingSpawn{slot, steamid, controller});
}

void VipPlugin::ProcessPendingSpawns(std::chrono::milliseconds dt) {
	if (m_pending_spawns.empty()) {
		return;
	}
	std::vector<PendingSpawn> pending = std::move(m_pending_spawns);
	m_pending_spawns.clear();
	for (PendingSpawn& action : pending) {
		// The original deferred this work by one frame (NextFrame); the delay is counted down by
		// the update delta, so a 0 ms update never spins on it.
		if (dt.count() > 0) {
			action.remaining -= dt;
		}
		if (action.remaining > std::chrono::milliseconds(0)) {
			m_pending_spawns.push_back(std::move(action));
			continue;
		}
		// The slot may have emptied, changed hands or the player may have reconnected with a new
		// controller entity while the action waited: revalidate both identities right now.
		if (!IsSlotHuman(action.slot)) {
			continue;
		}
		if (ClientSteamId64(action.slot) != action.steamid) {
			continue;
		}
		if (ControllerHandle(action.slot) != action.controller) {
			continue;
		}
		if (!ClientAlive(action.slot)) {
			continue;
		}
		const bool isVip = IsClientVip(action.slot);
		FirePlayerSpawn(action.slot, TeamOfSlot(action.slot), isVip);
	}
}

void VipPlugin::HandlePlayerSpawn(void* event) {
	// PARITY (OnPlayerSpawn): the controller is looked up from the event, bots are skipped, and
	// the work happens one update later (the pawn is not always alive yet inside the event).
	if (!g_mapReady || event == nullptr || __s2sdk_GetEventPlayerSlot == nullptr) {
		return;
	}
	int32_t slot = -1;
	try {
		slot = s2sdk::GetEventPlayerSlot(event, plg::string("userid"));
	} catch (...) {
		return;
	}
	if (!SlotInRange(slot)) {
		return;
	}
	const uint64_t steamid = ClientSteamId64(slot);
	if (steamid == 0) {
		return; // ignore bots
	}
	const int32_t controller = ControllerHandle(slot);
	if (controller == 0) {
		return;
	}
	QueuePlayerSpawn(slot, steamid, controller);
}

void VipPlugin::HandleRoundPreStart() {
	if (!g_mapReady || __s2sdk_GetGameRules == nullptr) {
		return;
	}
	void* rules = nullptr;
	try {
		rules = s2sdk::GetGameRules();
	} catch (...) {
		return;
	}
	if (rules == nullptr) {
		return;
	}
	// PARITY (OnRoundPreStart): a pistol round is one with no rounds played yet, a team switch at
	// round reset with no overtime playing, or a game restart.
	const int total = GameRulesInt("m_totalRoundsPlayed", TotalRounds(), m_offset_total_rounds);
	const bool switching = GameRulesBool("m_bSwitchingTeamsAtRoundReset", false, m_offset_switching);
	const int overtime = GameRulesInt("m_nOvertimePlaying", 0, m_offset_overtime);
	const bool restart = GameRulesBool("m_bGameRestart", false, m_offset_restart);
	m_pistol_round = total == 0 || (switching && overtime == 0) || restart;
}

void VipPlugin::OnStartupServer() {
	if (m_core != nullptr) {
		m_core->DeleteExpiredRows();
	}
}

void VipPlugin::OnSecondTick() {
	// Not before the schema exists: a retried core is usable only after its CREATE TABLE answered.
	if (!m_ready) {
		return;
	}
	ReconcileSlots();
	ExpireLoop();
}

void VipPlugin::ReconcileSlots() {
	// In practice OnClientAuthenticated may not fire, so every slot is checked once per second and
	// the vip_users row is read for whoever appeared. The original relied on HookOnClientAuthorized
	// alone.
	const auto now = std::chrono::steady_clock::now();
	for (int32_t slot = 0; slot < kMaxSlots; ++slot) {
		SlotState& state = m_slots[static_cast<size_t>(slot)];
		if (!IsConnectedHuman(slot)) {
			if (state.requested && !ClientConnected(slot)) {
				ClearSlot(slot); // the disconnect event was missed
			}
			continue;
		}
		const uint64_t steamid = ClientSteamId64(slot);
		if (steamid == 0) {
			continue;
		}
		if (state.requested && state.steamid == steamid) {
			// A load that failed (database down) is retried once the previous statement answered
			// and its delay passed. The in-flight flag keeps a long outage from queueing one
			// SELECT per slot every attempt; the delay doubles after every failure.
			if (!state.loaded && !state.loading && now - state.requested_at >= state.retry_delay) {
				state.requested_at = now;
				state.loading = true;
				m_core->LoadPlayer(slot, IdentityOf(slot), LoadPurpose::Connect);
			}
			continue;
		}
		// A new player in this slot (or a state that predates the core).
		state = SlotState{};
		state.steamid = steamid;
		state.account_id = AccountId(steamid);
		state.name = PlayerName(slot);
		state.requested = true;
		state.loading = true;
		state.requested_at = now;
		m_core->LoadPlayer(slot, IdentityOf(slot), LoadPurpose::Connect);
	}
}

void VipPlugin::ExpireLoop() {
	// PARITY (GameFrame): every second, for every connected/authenticated non-bot player, a VIP
	// whose TimeEnd passed is forgotten: callback, the three chat lines and the DELETE.
	const int64_t now = m_core->Now();
	for (int32_t slot = 0; slot < kMaxSlots; ++slot) {
		if (!IsConnectedHuman(slot)) {
			continue;
		}
		const uint32_t accountId = AccountIdOf(slot);
		if (accountId == 0) {
			continue;
		}
		// The slot may have been reused between the reconcile and this pass: only the player the
		// tracked state belongs to may be notified/expired here (SteamID64 identity, C2/B4).
		const uint64_t steamid = SteamIdOf(slot);
		if (steamid == 0 || ClientSteamId64(slot) != steamid) {
			continue;
		}
		if (m_core->ExpirePlayer(accountId, now)) {
			m_core->ClearSlotFeatures(slot);
			FireVipRemoved(slot, kRemoveExpired);
			NotifyExpired(slot);
			m_core->DeleteRow(accountId);
		}
	}
}

// --- vip_core hooks -----------------------------------------------------------------------------

void VipPlugin::HandleLoaded(int32_t slot, const VipLoadOutcome& outcome, LoadPurpose purpose) {
	if (!SlotInRange(slot)) {
		return;
	}
	SlotState& state = m_slots[static_cast<size_t>(slot)];
	if (state.account_id != outcome.account_id) {
		// The slot was reused (or cleared) while the statement was in flight: the result belongs
		// to a previous occupant and must not mark the new player's slot as loaded.
		return;
	}
	state.loading = false;
	if (outcome.failed) {
		// The read did not answer: keep the slot unloaded and retry later, waiting longer after
		// every failure (see ReconcileSlots).
		state.retry_delay = std::min(state.retry_delay * 2, kLoadRetryMax);
		return;
	}
	state.loaded = true;
	state.retry_delay = kLoadRetryAfter;

	const bool isVip = outcome.row_found && !outcome.expired && m_core != nullptr &&
					   m_core->IsClientVip(IdentityOf(slot), m_core->Now());

	// PARITY: both OnClientAuthorized and mm_reload_vip fired Call_VIP_OnClientLoaded with the
	// state right after the row was read.
	FireClientLoaded(slot, isVip);
	state.client_loaded_fired = true;

	if (purpose == LoadPurpose::Reload && isVip) {
		// PARITY (mm_reload_vip): the Welcome message for a valid reload.
		if (m_core->IsValidGroup(outcome.group)) {
			Welcome(slot, outcome.group, outcome.expires);
		}
	}
}

void VipPlugin::HandleCoreError(std::string_view message) {
	LogError(message);
}

void VipPlugin::HandleUserLoaded(uint64_t steamid64) {
	// The permissions user - and with it the '<group>.<feature>' cookies - became available. The
	// original fired VIP_OnClientLoaded from HookClientCookieLoaded as well; here it fires a
	// second time when the cookie store arrives *after* the vip_users row, so a module that reads
	// toggles in its handler sees them (see API.md "When VIP_OnClientLoaded fires").
	if (m_core == nullptr || steamid64 == 0) {
		return;
	}
	for (int32_t slot = 0; slot < kMaxSlots; ++slot) {
		SlotState& state = m_slots[static_cast<size_t>(slot)];
		if (!state.requested || state.steamid != steamid64) {
			continue;
		}
		if (state.user_loaded_fired) {
			return;
		}
		if (!state.loaded) {
			// The vip_users row is still in flight: its callback fires once, with the cookies
			// already in place.
			state.user_loaded_seen = true;
			return;
		}
		state.user_loaded_fired = true;
		if (IsSlotHuman(slot)) {
			FireClientLoaded(slot, IsClientVip(slot));
		}
		return;
	}
}

// --- slot data / helpers ------------------------------------------------------------------------

uint64_t VipPlugin::SteamIdOf(int32_t slot) const {
	if (!SlotInRange(slot)) {
		return 0;
	}
	return m_slots[static_cast<size_t>(slot)].steamid;
}

uint32_t VipPlugin::AccountIdOf(int32_t slot) const {
	if (!SlotInRange(slot)) {
		return 0;
	}
	return m_slots[static_cast<size_t>(slot)].account_id;
}

bool VipPlugin::IsSlotConnectedHuman(int32_t slot) const {
	return IsConnectedHuman(slot);
}

bool VipPlugin::IsSlotHuman(int32_t slot) const {
	// PITFALLS C2: the in-game/controller layer every menu, chat or feature action needs.
	if (!IsConnectedHuman(slot)) {
		return false;
	}
	if (!ClientInGame(slot) || ClientSourceTV(slot)) {
		return false;
	}
	return ControllerHandle(slot) != 0;
}

uint64_t VipPlugin::ClientSteamIdOf(int32_t slot) const {
	return ClientSteamId64(slot);
}

std::string VipPlugin::PlayerName(int32_t slot) const {
	if (!SlotInRange(slot)) {
		return {};
	}
	const SlotState& state = m_slots[static_cast<size_t>(slot)];
	if (!state.name.empty()) {
		return state.name;
	}
	return ClientNameOf(slot);
}

VipIdentity VipPlugin::IdentityOf(int32_t slot) const {
	VipIdentity identity;
	if (!SlotInRange(slot)) {
		return identity;
	}
	const SlotState& state = m_slots[static_cast<size_t>(slot)];
	identity.slot = slot;
	identity.account_id = state.account_id;
	identity.steamid64 = state.steamid;
	identity.name = PlayerName(slot);
	return identity;
}

VipIdentity VipPlugin::TrackedIdentityOf(int32_t slot) const {
	VipIdentity identity;
	if (!SlotInRange(slot)) {
		return identity;
	}
	const SlotState& state = m_slots[static_cast<size_t>(slot)];
	identity.slot = slot;
	identity.account_id = state.account_id;
	identity.steamid64 = state.steamid;
	identity.name = state.name;
	return identity;
}

int32_t VipPlugin::ControllerHandleOf(int32_t slot) const {
	return ControllerHandle(slot);
}

void VipPlugin::ClearSlot(int32_t slot) {
	if (!SlotInRange(slot)) {
		return;
	}
	m_slots[static_cast<size_t>(slot)] = SlotState{};
	if (m_core != nullptr) {
		m_core->ClearSlotFeatures(slot);
	}
}

void VipPlugin::Welcome(int32_t slot, const std::string& group, int64_t expires) const {
	// PARITY (the WelcomePerm / Welcome messages): permanent VIPs get WelcomePerm, timed ones get
	// Welcome with the formatted expiry. The phrase carries the printf placeholders, exactly like
	// the original's PrintToChat(translation, name, buffer) calls.
	const std::string name = PlayerName(slot);
	if (expires == 0) {
		ChatLine(slot, FormatString(CStr(Phrase("WelcomePerm")), CStr(name)));
	} else {
		const std::string when = FormatTimestamp(expires);
		ChatLine(slot, FormatString(CStr(Phrase("Welcome")), CStr(name), CStr(when)));
	}
	(void)group;
}

void VipPlugin::NotifyExpired(int32_t slot) const {
	// PARITY: VIPExpired1..3.
	ChatLine(slot, Phrase("VIPExpired1"));
	ChatLine(slot, Phrase("VIPExpired2"));
	ChatLine(slot, Phrase("VIPExpired3"));
}

int VipPlugin::SchemaOffset(const char* className, const char* field, int& cache) const {
	// PITFALLS C8: the lookup itself needs a live map, and a failed one is never cached (the
	// caller keeps asking until the class/field is available).
	if (cache >= 0) {
		return cache;
	}
	if (!g_mapReady || className == nullptr || field == nullptr || __s2sdk_GetSchemaOffset == nullptr) {
		return -1;
	}
	try {
		const int offset = s2sdk::GetSchemaOffset(plg::string(className), plg::string(field));
		if (offset >= 0) {
			cache = offset;
		}
		return offset;
	} catch (...) {
		return -1;
	}
}

int VipPlugin::GameRulesInt(const char* field, int def, int& cache) const {
	// The game rules object is not an entity: the raw offset read is the C6-safe path, and the
	// offset is only resolved with the map gate open.
	if (!g_mapReady || __s2sdk_GetGameRules == nullptr) {
		return def;
	}
	void* rules = nullptr;
	try {
		rules = s2sdk::GetGameRules();
	} catch (...) {
		return def;
	}
	if (rules == nullptr) {
		return def;
	}
	const int offset = SchemaOffset("CCSGameRules", field, cache);
	if (offset < 0) {
		return def;
	}
	int value = 0;
	std::memcpy(&value, static_cast<const char*>(rules) + offset, sizeof(value));
	return value;
}

bool VipPlugin::GameRulesBool(const char* field, bool def, int& cache) const {
	if (!g_mapReady || __s2sdk_GetGameRules == nullptr) {
		return def;
	}
	void* rules = nullptr;
	try {
		rules = s2sdk::GetGameRules();
	} catch (...) {
		return def;
	}
	if (rules == nullptr) {
		return def;
	}
	const int offset = SchemaOffset("CCSGameRules", field, cache);
	if (offset < 0) {
		return def;
	}
	unsigned char value = 0;
	std::memcpy(&value, static_cast<const char*>(rules) + offset, sizeof(value));
	return value != 0;
}

int32_t VipPlugin::TeamOfSlot(int32_t slot) const {
	// The team lives on the controller (CBaseEntity::m_iTeamNum, 1 byte - sdk.json), read with
	// the validated controller HANDLE (C1) at a map-gated, cached offset (C8).
	const int32_t handle = ControllerHandle(slot);
	if (handle == 0 || __s2sdk_GetEntData == nullptr) {
		return 0;
	}
	const int offset = SchemaOffset("CBaseEntity", "m_iTeamNum", m_offset_team);
	if (offset < 0) {
		return 0;
	}
	try {
		return static_cast<int32_t>(s2sdk::GetEntData(handle, offset, 1) & 0xFF);
	} catch (...) {
		return 0;
	}
}

// --- VIPApi surface -------------------------------------------------------------------------------

bool VipPlugin::IsClientVip(int32_t slot) {
	// PARITY (VIP_IsClientVIP): authenticated client with a record, unexpired and a valid group.
	if (m_core == nullptr || !IsSlotHuman(slot)) {
		return false;
	}
	const uint32_t accountId = AccountIdOf(slot);
	if (accountId == 0) {
		return false;
	}
	return m_core->IsClientVip(IdentityOf(slot), m_core->Now());
}

int32_t VipPlugin::AccessTime(int32_t slot) {
	if (m_core == nullptr || !IsSlotHuman(slot)) {
		return -1;
	}
	const uint32_t accountId = AccountIdOf(slot);
	if (accountId == 0) {
		return -1;
	}
	return static_cast<int32_t>(m_core->AccessTime(IdentityOf(slot), m_core->Now()));
}

bool VipPlugin::SetAccessTime(int32_t slot, int32_t time, bool in_db) {
	if (!m_ready || !IsSlotHuman(slot) || AccountIdOf(slot) == 0) {
		return false;
	}
	return m_core->SetAccessTime(IdentityOf(slot), time, in_db);
}

std::string VipPlugin::GroupName(int32_t slot) {
	if (m_core == nullptr || !IsSlotHuman(slot) || AccountIdOf(slot) == 0) {
		return {};
	}
	return m_core->GroupOf(IdentityOf(slot));
}

bool VipPlugin::SetGroup(int32_t slot, const std::string& group, bool in_db) {
	if (!m_ready || !IsSlotHuman(slot) || AccountIdOf(slot) == 0) {
		return false;
	}
	return m_core->SetGroup(IdentityOf(slot), group, in_db, m_core->Now());
}

bool VipPlugin::GiveVip(int32_t slot, int32_t time, const std::string& group, bool add_to_db) {
	if (!m_ready || !IsSlotHuman(slot) || AccountIdOf(slot) == 0) {
		return false;
	}
	const VipIdentity identity = IdentityOf(slot);
	const int64_t now = m_core->Now();
	if (!m_core->GiveVip(identity, time, group, add_to_db, now)) {
		return false;
	}
	// PARITY (VIP_GiveClientVIP): the welcome message, VIP_OnClientLoaded(true) and
	// VIP_OnVIPClientAdded.
	Welcome(slot, group, m_core->AccessTime(identity, now));
	FireClientLoaded(slot, true);
	FireVipAdded(slot);
	return true;
}

bool VipPlugin::RemoveVip(int32_t slot, bool notify, bool in_db) {
	if (!m_ready || !IsSlotHuman(slot) || AccountIdOf(slot) == 0) {
		return false;
	}
	if (!m_core->RemoveVip(IdentityOf(slot), in_db)) {
		return false;
	}
	// PARITY (VIP_RemoveClientVIP): the three chat lines, VIP_OnVIPClientRemoved(2) and
	// VIP_OnClientDisconnect(slot, false).
	if (notify) {
		NotifyExpired(slot);
	}
	FireVipRemoved(slot, kRemoveByApi);
	FireClientDisconnect(slot, false);
	return true;
}

bool VipPlugin::IsValidGroup(const std::string& group) const {
	return m_core != nullptr && m_core->IsValidGroup(group);
}

const GroupFeatures* VipPlugin::GroupFeaturesForSlot(int32_t slot) {
	// PARITY (GetGroupKV): only a connected, authenticated, non-bot player with a live VIP record
	// has a feature table.
	if (m_core == nullptr || !IsSlotHuman(slot)) {
		return nullptr;
	}
	const uint32_t accountId = AccountIdOf(slot);
	if (accountId == 0) {
		return nullptr;
	}
	return m_core->ResolveFeatures(slot, accountId, m_core->Now());
}

std::string VipPlugin::DiagnoseSlot(int32_t slot) {
	if (!SlotInRange(slot) || !IsConnectedHuman(slot)) {
		return {};
	}
	const bool authenticated = ClientAuthorized(slot);
	const bool fake = ClientFake(slot);
	const uint32_t accountId = AccountIdOf(slot);
	std::string text = "vip_status slot " + std::to_string(slot) + " '" + PlayerName(slot) +
					   "': authenticated=" + (authenticated ? "1" : "0") + " fake=" + (fake ? "1" : "0") +
					   " account_id=" + std::to_string(accountId);
	if (m_core == nullptr) {
		return text + " core=none";
	}
	const VipPlayer* player = accountId != 0 ? m_core->FindPlayer(accountId) : nullptr;
	if (player == nullptr) {
		return text + " record=none";
	}
	text += " group=" + player->group + " time_end=" + std::to_string(player->time_end) +
			" now=" + std::to_string(m_core->Now()) +
			" group_defined=" + (m_core->Groups().count(player->group) != 0 ? "1" : "0") +
			" features=" + (GroupFeaturesForSlot(slot) != nullptr ? "yes" : "no");
	return text;
}

int32_t VipPlugin::FeatureInt(int32_t slot, const char* feature) {
	if (feature == nullptr || !*feature) {
		return -1;
	}
	const GroupFeatures* group = GroupFeaturesForSlot(slot);
	if (group == nullptr) {
		return -1;
	}
	return static_cast<int32_t>(vip::features::GetInt(*group, feature, GetCookie(slot, feature)));
}

bool VipPlugin::FeatureBool(int32_t slot, const char* feature) {
	if (feature == nullptr || !*feature) {
		return false;
	}
	const GroupFeatures* group = GroupFeaturesForSlot(slot);
	if (group == nullptr) {
		return false;
	}
	return vip::features::GetBool(*group, feature, GetCookie(slot, feature));
}

float VipPlugin::FeatureFloat(int32_t slot, const char* feature) {
	if (feature == nullptr || !*feature) {
		return 1.0f;
	}
	const GroupFeatures* group = GroupFeaturesForSlot(slot);
	if (group == nullptr) {
		return 1.0f;
	}
	return vip::features::GetFloat(*group, feature, GetCookie(slot, feature));
}

std::string VipPlugin::FeatureString(int32_t slot, const char* feature) {
	if (feature == nullptr || !*feature) {
		return {};
	}
	const GroupFeatures* group = GroupFeaturesForSlot(slot);
	if (group == nullptr) {
		return {};
	}
	return vip::features::GetString(*group, feature, GetCookie(slot, feature));
}

void VipPlugin::RegisterFeature(const plg::string& feature, int value_type, int feature_type, ItemSelectableDelegate select,
								ItemTogglableDelegate toggle, ItemDisplayDelegate display) {
	// PARITY (VIP_RegisterFeature): the registration replaces the feature's entry in the core's table.
	Feature& entry = m_features[ToStd(feature)];
	entry.value_type = value_type;
	entry.feature_type = feature_type;
	entry.select = select;
	entry.toggle = toggle;
	entry.display = display;
}

// --- cookies (the ICookiesApi of the original, on the official permissions plugin) -----------------

int64_t VipPlugin::PluginId() const {
	if (plg::plugin::GetId == nullptr || plg::plugin::handle == nullptr) {
		return 0;
	}
	try {
		return static_cast<int64_t>(plg::plugin::GetId(plg::plugin::handle));
	} catch (...) {
		return 0;
	}
}

bool VipPlugin::CookiesAvailable() const {
	// Checked on every use: an optional dependency may be loaded after this plugin.
	return __permissions_GetCookie != nullptr && __permissions_SetCookie != nullptr;
}

std::string VipPlugin::GetCookie(int32_t slot, const std::string& name) {
	// PARITY (VIP_GetClientCookie): the key is '<group>.<name>'.
	if (!CookiesAvailable()) {
		return {};
	}
	const std::string group = GroupName(slot);
	if (group.empty()) {
		return {};
	}
	const uint64_t steamid = SteamIdOf(slot);
	if (steamid == 0) {
		return {};
	}
	try {
		plg::any value;
		const permissions::Status status = permissions::GetCookie(steamid, plg::string(group + "." + name), value);
		if (status != permissions::Status::Success) {
			return {};
		}
		return CookieToString(value);
	} catch (const std::exception& ex) {
		LogError(std::string("vip: reading a cookie failed: ") + ex.what());
	} catch (...) {
		LogError("vip: reading a cookie failed with an unknown exception");
	}
	return {};
}

bool VipPlugin::SetCookie(int32_t slot, const std::string& name, const std::string& data) {
	// PARITY (VIP_SetClientCookie): store '<group>.<name>' = data.
	if (!CookiesAvailable()) {
		return false;
	}
	const std::string group = GroupName(slot);
	if (group.empty()) {
		return false;
	}
	const uint64_t steamid = SteamIdOf(slot);
	if (steamid == 0) {
		return false;
	}
	try {
		const permissions::Status status = permissions::SetCookie(PluginId(), steamid,
																  plg::string(group + "." + name),
																  plg::any(plg::string(data)), /*dontBroadcast=*/false);
		return status == permissions::Status::Success;
	} catch (const std::exception& ex) {
		LogError(std::string("vip: writing a cookie failed: ") + ex.what());
	} catch (...) {
		LogError("vip: writing a cookie failed with an unknown exception");
	}
	return false;
}

void VipPlugin::PrintToCenter(int32_t slot, const std::string& msg) {
	if (!IsSlotHuman(slot) || __s2sdk_PrintCenterText == nullptr) {
		return;
	}
	try {
		s2sdk::PrintCenterText(slot, plg::string(msg));
	} catch (...) {
	}
}

void VipPlugin::OpenMenu(int32_t slot) {
	ShowMenu(slot, true);
}

// --- game state ------------------------------------------------------------------------------------

bool VipPlugin::PistolRound() const {
	// Provisional true until a real round_prestart (a late map may be mid second-half pistol).
	// Outside a live map this stays neutral false and never queries the engine.
	return g_mapReady && m_pistol_round;
}

bool VipPlugin::WarmupPeriod() {
	// m_bWarmupPeriod of the game rules, through the game schema (see GameRulesInt).
	return GameRulesBool("m_bWarmupPeriod", false, m_offset_warmup);
}

int32_t VipPlugin::TotalRounds() {
	// m_totalRoundsPlayed of the game rules; s2sdk has an accessor for this one.
	if (!g_mapReady || __s2sdk_GetGameTotalRoundsPlayed == nullptr || __s2sdk_GetGameRules == nullptr) {
		return 0;
	}
	try {
		if (s2sdk::GetGameRules() == nullptr) {
			return 0;
		}
		return s2sdk::GetGameTotalRoundsPlayed();
	} catch (...) {
		return 0;
	}
}

// --- command support ---------------------------------------------------------------------------

int VipPlugin::FindPlayerSlot(const std::string& token, bool require_pawn) const {
	// PARITY (the search of mm_reload_vip, vip_remove and vip_give): a substring of the player
	// name, the 32 bit account id, the slot index - and, like mm_reload_vip did with the engine's
	// XUID, the steamid64. The player must have a steamid (bots do not). vip_give additionally
	// demanded a pawn.
	if (token.empty()) {
		return -1;
	}
	const bool digits = ContainsOnlyDigits(token);
	long long number = -1;
	if (digits) {
		try {
			number = std::stoll(token);
		} catch (const std::exception&) {
			number = -1; // a digit string that does not fit into a long long matches nothing
		}
	}
	for (int32_t slot = 0; slot < kMaxSlots; ++slot) {
		if (!IsConnectedHuman(slot)) {
			continue;
		}
		const uint64_t steamid = ClientSteamId64(slot);
		if (steamid == 0) {
			continue;
		}
		if (require_pawn && PawnIndex(slot) == 0) {
			continue;
		}
		const std::string name = ClientNameOf(slot);
		if (!name.empty() && name.find(token) != std::string::npos) {
			return slot;
		}
		if (!digits || number < 0) {
			continue;
		}
		if (static_cast<uint64_t>(number) == static_cast<uint64_t>(AccountId(steamid))) {
			return slot;
		}
		if (number == slot) {
			return slot;
		}
		if (static_cast<uint64_t>(number) == steamid) {
			return slot;
		}
	}
	return -1;
}

void VipPlugin::ReloadClientVip(int32_t slot) {
	// mm_reload_vip: the record and the per-slot cache are dropped and the row is read again.
	if (!m_ready || !SlotInRange(slot) || m_core == nullptr) {
		return;
	}
	SlotState& state = m_slots[static_cast<size_t>(slot)];
	const uint64_t steamid = state.steamid != 0 ? state.steamid : ClientSteamId64(slot);
	if (steamid == 0) {
		return;
	}
	state.steamid = steamid;
	state.account_id = AccountId(steamid);
	if (state.name.empty()) {
		state.name = PlayerName(slot);
	}
	state.requested = true;
	state.loading = true;
	state.loaded = false;
	state.client_loaded_fired = false;
	state.user_loaded_fired = false;
	state.user_loaded_seen = false;
	state.requested_at = std::chrono::steady_clock::now();
	m_core->LoadPlayer(slot, IdentityOf(slot), LoadPurpose::Reload);
}

void VipPlugin::GiveOfflineVip(const std::string& account_id_text, int32_t seconds, const std::string& group) {
	// vip_give, offline branch: a plain INSERT of the typed account id (name 'none').
	if (!m_ready || account_id_text.empty() || m_core == nullptr) {
		return;
	}
	m_core->InsertRow(account_id_text, seconds, group, m_core->Now());
}

// --- callback registries --------------------------------------------------------------------------------

void VipPlugin::FireReady() {
	for (const ReadyDelegate& callback : m_on_ready) {
		if (callback == nullptr) {
			continue;
		}
		try {
			callback();
		} catch (const std::exception& ex) {
			LogError(std::string("vip: exception in a VIP_OnVIPLoaded listener: ") + ex.what());
		} catch (...) {
			LogError("vip: unknown exception in a VIP_OnVIPLoaded listener");
		}
	}
}

void VipPlugin::FireClientLoaded(int32_t slot, bool is_vip) {
	for (const ClientLoadedOrDisconnectDelegate& callback : m_on_client_loaded) {
		if (callback == nullptr) {
			continue;
		}
		try {
			callback(slot, is_vip);
		} catch (const std::exception& ex) {
			LogError(std::string("vip: exception in a VIP_OnClientLoaded listener: ") + ex.what());
		} catch (...) {
			LogError("vip: unknown exception in a VIP_OnClientLoaded listener");
		}
	}
}

void VipPlugin::FireClientDisconnect(int32_t slot, bool is_vip) {
	for (const ClientLoadedOrDisconnectDelegate& callback : m_on_client_disconnect) {
		if (callback == nullptr) {
			continue;
		}
		try {
			callback(slot, is_vip);
		} catch (const std::exception& ex) {
			LogError(std::string("vip: exception in a VIP_OnClientDisconnect listener: ") + ex.what());
		} catch (...) {
			LogError("vip: unknown exception in a VIP_OnClientDisconnect listener");
		}
	}
}

void VipPlugin::FirePlayerSpawn(int32_t slot, int32_t team, bool is_vip) {
	for (const SpawnDelegate& callback : m_on_player_spawn) {
		if (callback == nullptr) {
			continue;
		}
		try {
			callback(slot, team, is_vip);
		} catch (const std::exception& ex) {
			LogError(std::string("vip: exception in a VIP_OnPlayerSpawn listener: ") + ex.what());
		} catch (...) {
			LogError("vip: unknown exception in a VIP_OnPlayerSpawn listener");
		}
	}
}

void VipPlugin::FireVipRemoved(int32_t slot, int32_t reason) {
	for (const VipRemoveDelegate& callback : m_on_vip_removed) {
		if (callback == nullptr) {
			continue;
		}
		try {
			callback(slot, reason);
		} catch (const std::exception& ex) {
			LogError(std::string("vip: exception in a VIP_OnVIPClientRemoved listener: ") + ex.what());
		} catch (...) {
			LogError("vip: unknown exception in a VIP_OnVIPClientRemoved listener");
		}
	}
}

void VipPlugin::FireVipAdded(int32_t slot) {
	for (const VipAddDelegate& callback : m_on_vip_added) {
		if (callback == nullptr) {
			continue;
		}
		try {
			callback(slot);
		} catch (const std::exception& ex) {
			LogError(std::string("vip: exception in a VIP_OnVIPClientAdded listener: ") + ex.what());
		} catch (...) {
			LogError("vip: unknown exception in a VIP_OnVIPClientAdded listener");
		}
	}
}

} // namespace vip

// --- the exported methods (IVIPApi of the original, 34 exports) -------------------------------------
// Every one of them catches its exceptions: they are called across the plugify boundary from other
// plugins, where a throw would unwind into the module (and from there into the engine).

using namespace vip;

extern "C" PLUGIN_API bool VIP_IsVIPLoaded() {
	try {
		return g_vipPlugin.IsReady();
	} catch (...) {
		return false;
	}
}

extern "C" PLUGIN_API plg::string VIP_GetTranslate(const plg::string& phrase) {
	try {
		return plg::string(g_vipPlugin.Phrase(ToString(phrase)));
	} catch (...) {
		return plg::string();
	}
}

extern "C" PLUGIN_API bool VIP_IsClientVIP(int32_t slot) {
	try {
		return g_vipPlugin.IsClientVip(slot);
	} catch (...) {
		return false;
	}
}

extern "C" PLUGIN_API int32_t VIP_GetClientAccessTime(int32_t slot) {
	try {
		return g_vipPlugin.AccessTime(slot);
	} catch (...) {
		return -1;
	}
}

extern "C" PLUGIN_API bool VIP_SetClientAccessTime(int32_t slot, int32_t time, bool in_db) {
	try {
		return g_vipPlugin.SetAccessTime(slot, time, in_db);
	} catch (...) {
		return false;
	}
}

extern "C" PLUGIN_API plg::string VIP_GetClientVIPGroup(int32_t slot) {
	try {
		return plg::string(g_vipPlugin.GroupName(slot));
	} catch (...) {
		return plg::string();
	}
}

extern "C" PLUGIN_API bool VIP_SetClientVIPGroup(int32_t slot, const plg::string& group, bool in_db) {
	try {
		return g_vipPlugin.SetGroup(slot, ToString(group), in_db);
	} catch (...) {
		return false;
	}
}

extern "C" PLUGIN_API bool VIP_GiveClientVIP(int32_t slot, int32_t time, const plg::string& group, bool add_to_db) {
	try {
		return g_vipPlugin.GiveVip(slot, time, ToString(group), add_to_db);
	} catch (...) {
		return false;
	}
}

extern "C" PLUGIN_API bool VIP_RemoveClientVIP(int32_t slot, bool notify, bool in_db) {
	try {
		return g_vipPlugin.RemoveVip(slot, notify, in_db);
	} catch (...) {
		return false;
	}
}

extern "C" PLUGIN_API int32_t VIP_GetClientFeatureInt(int32_t slot, const plg::string& feature) {
	try {
		const std::string name = ToString(feature);
		return g_vipPlugin.FeatureInt(slot, name.c_str());
	} catch (...) {
		return -1;
	}
}

extern "C" PLUGIN_API bool VIP_GetClientFeatureBool(int32_t slot, const plg::string& feature) {
	try {
		const std::string name = ToString(feature);
		return g_vipPlugin.FeatureBool(slot, name.c_str());
	} catch (...) {
		return false;
	}
}

extern "C" PLUGIN_API float VIP_GetClientFeatureFloat(int32_t slot, const plg::string& feature) {
	try {
		const std::string name = ToString(feature);
		return g_vipPlugin.FeatureFloat(slot, name.c_str());
	} catch (...) {
		return 1.0f;
	}
}

extern "C" PLUGIN_API plg::string VIP_GetClientFeatureString(int32_t slot, const plg::string& feature) {
	try {
		const std::string name = ToString(feature);
		return plg::string(g_vipPlugin.FeatureString(slot, name.c_str()));
	} catch (...) {
		return plg::string();
	}
}

extern "C" PLUGIN_API void VIP_RegisterFeature(const plg::string& feature, ::vip::ValueType value_type,
											   ::vip::FeatureType feature_type, ItemSelectableDelegate select,
											   ItemTogglableDelegate toggle, ItemDisplayDelegate display) {
	try {
		g_vipPlugin.RegisterFeature(feature, static_cast<int>(value_type), static_cast<int>(feature_type), select, toggle, display);
	} catch (const std::exception& ex) {
		LogError(std::string("vip: VIP_RegisterFeature failed: ") + ex.what());
	} catch (...) {
		LogError("vip: VIP_RegisterFeature failed");
	}
}

extern "C" PLUGIN_API bool VIP_IsValidVIPGroup(const plg::string& group) {
	try {
		return g_vipPlugin.IsValidGroup(ToString(group));
	} catch (...) {
		return false;
	}
}

extern "C" PLUGIN_API bool VIP_PistolRound() {
	try {
		return g_vipPlugin.PistolRound();
	} catch (...) {
		return false;
	}
}

extern "C" PLUGIN_API bool VIP_WarmupPeriod() {
	try {
		return g_vipPlugin.WarmupPeriod();
	} catch (...) {
		return false;
	}
}

extern "C" PLUGIN_API int32_t VIP_GetTotalRounds() {
	try {
		return g_vipPlugin.TotalRounds();
	} catch (...) {
		return 0;
	}
}

extern "C" PLUGIN_API bool VIP_SetClientCookie(int32_t slot, const plg::string& name, const plg::string& data) {
	try {
		return g_vipPlugin.SetCookie(slot, ToString(name), ToString(data));
	} catch (...) {
		return false;
	}
}

extern "C" PLUGIN_API plg::string VIP_GetClientCookie(int32_t slot, const plg::string& name) {
	try {
		return plg::string(g_vipPlugin.GetCookie(slot, ToString(name)));
	} catch (...) {
		return plg::string();
	}
}

extern "C" PLUGIN_API void VIP_PrintToCenter(int32_t slot, const plg::string& msg) {
	try {
		g_vipPlugin.PrintToCenter(slot, ToString(msg));
	} catch (...) {
	}
}

extern "C" PLUGIN_API void VIP_OpenMenu(int32_t slot) {
	try {
		g_vipPlugin.OpenMenu(slot);
	} catch (const std::exception& ex) {
		LogError(std::string("vip: VIP_OpenMenu failed: ") + ex.what());
	} catch (...) {
		LogError("vip: VIP_OpenMenu failed");
	}
}

extern "C" PLUGIN_API void VIP_OnVIPLoaded_Register(ReadyDelegate callback) {
	try {
		AddListener(g_vipPlugin.ReadyListeners(), callback);
	} catch (...) {
	}
}

extern "C" PLUGIN_API void VIP_OnVIPLoaded_Unregister(ReadyDelegate callback) {
	try {
		RemoveListener(g_vipPlugin.ReadyListeners(), callback);
	} catch (...) {
	}
}

extern "C" PLUGIN_API void VIP_OnClientLoaded_Register(ClientLoadedOrDisconnectDelegate callback) {
	try {
		AddListener(g_vipPlugin.ClientLoadedListeners(), callback);
	} catch (...) {
	}
}

extern "C" PLUGIN_API void VIP_OnClientLoaded_Unregister(ClientLoadedOrDisconnectDelegate callback) {
	try {
		RemoveListener(g_vipPlugin.ClientLoadedListeners(), callback);
	} catch (...) {
	}
}

extern "C" PLUGIN_API void VIP_OnClientDisconnect_Register(ClientLoadedOrDisconnectDelegate callback) {
	try {
		AddListener(g_vipPlugin.ClientDisconnectListeners(), callback);
	} catch (...) {
	}
}

extern "C" PLUGIN_API void VIP_OnClientDisconnect_Unregister(ClientLoadedOrDisconnectDelegate callback) {
	try {
		RemoveListener(g_vipPlugin.ClientDisconnectListeners(), callback);
	} catch (...) {
	}
}

extern "C" PLUGIN_API void VIP_OnPlayerSpawn_Register(SpawnDelegate callback) {
	try {
		AddListener(g_vipPlugin.PlayerSpawnListeners(), callback);
	} catch (...) {
	}
}

extern "C" PLUGIN_API void VIP_OnPlayerSpawn_Unregister(SpawnDelegate callback) {
	try {
		RemoveListener(g_vipPlugin.PlayerSpawnListeners(), callback);
	} catch (...) {
	}
}

extern "C" PLUGIN_API void VIP_OnVIPClientRemoved_Register(VipRemoveDelegate callback) {
	try {
		AddListener(g_vipPlugin.VipRemovedListeners(), callback);
	} catch (...) {
	}
}

extern "C" PLUGIN_API void VIP_OnVIPClientRemoved_Unregister(VipRemoveDelegate callback) {
	try {
		RemoveListener(g_vipPlugin.VipRemovedListeners(), callback);
	} catch (...) {
	}
}

extern "C" PLUGIN_API void VIP_OnVIPClientAdded_Register(VipAddDelegate callback) {
	try {
		AddListener(g_vipPlugin.VipAddedListeners(), callback);
	} catch (...) {
	}
}

extern "C" PLUGIN_API void VIP_OnVIPClientAdded_Unregister(VipAddDelegate callback) {
	try {
		RemoveListener(g_vipPlugin.VipAddedListeners(), callback);
	} catch (...) {
	}
}

PLUGIFY_PLUGIN(PLUGIN_API, &vip::g_vipPlugin)
