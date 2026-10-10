// vip — engine side: lifecycle, configuration, the s2sdk listeners, the once-per-second
// reconcile+expiry tick, the vip_core hook bodies, the messages of the original and the 34
// exported methods (the IVIPApi of the Pisex cs2-vip plugin).
//
// Ported from the original cs2-vip (vip.cpp) onto libs/pisexcompat + vip_core + the permissions
// plugin (the cookie store; the ICookiesApi of the original). Commands live in commands.cpp,
// the menu in menus.cpp.
//
// Rules this file follows:
//   * nothing here blocks on the database except the startup path (CreateSchema in EnsureCore);
//   * every s2sdk/plugify callback catches its exceptions (a throw into the module would kill the
//     server, not the plugin);
//   * no client query before a map exists - pisexcompat's player wrappers return neutral values
//     until then, and the reconcile only runs from OnPluginUpdate (pumped with a running map);
//   * the plugin never returns Failed from OnPluginStart (that stops every plugify plugin); a
//     missing binding or a broken config leaves it running as a no-op core.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <exception>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <plg/plugin.hpp>
#include <plg/string.hpp>
#include <plugin_export.h>
#include <s2sdk/s2sdk.hpp>

#include <permissions/permissions.hpp>

#include <pisex/pisex.hpp>

#include <vip/vip.hpp>

#include "vip_plugin.hpp"

namespace vip {

VipPlugin g_vipPlugin;

// --- logging / small helpers -----------------------------------------------------------

void LogMessage(std::string_view message, plg::Severity severity) {
	if (plg::Log) {
		plg::Log(message, severity, plg::source_location::current());
	}
}

void LogInfo(std::string_view message) {
	LogMessage(message, plg::Severity::Info);
}

void LogError(std::string_view message) {
	LogMessage(message, plg::Severity::Error);
}

std::string ToString(std::string_view view) {
	return std::string(view);
}

bool ContainsOnlyDigits(std::string_view text) {
	if (text.empty()) {
		return false;
	}
	return text.find_first_not_of("0123456789") == std::string_view::npos;
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
		return ToString(plg::get<plg::string>(value));
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

namespace {

constexpr auto kLoadRetryAfter = std::chrono::seconds(10);

std::string ToString(const plg::string& text) {
	return std::string(std::string_view(text));
}

// A registry is keyed by the callback pointer (the original keyed listeners by plugin id; every
// consumer plugin registers exactly one function per event, and registering the same pointer
// again is a no-op, so a vector with duplicate rejection is the same thing).
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

void OnPlayerSpawnEvent(const char* name, pisex::IGameEvent* event, bool dontBroadcast) {
	(void)name;
	(void)dontBroadcast;
	try {
		if (event != nullptr) {
			g_vipPlugin.HandlePlayerSpawn(*event);
		}
	} catch (const std::exception& ex) {
		LogError(std::string("vip: exception in the player_spawn handler: ") + ex.what());
	} catch (...) {
		LogError("vip: unknown exception in the player_spawn handler");
	}
}

void OnRoundPreStartEvent(const char* name, pisex::IGameEvent* event, bool dontBroadcast) {
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
}

} // namespace

// --- configuration ----------------------------------------------------------------------

bool VipPlugin::LoadDatabaseConfig() {
	// The "vip" section of databases.cfg, the same file and section name the original read
	// (mirroring plugins/admin_system; a missing file or section falls back to a local SQLite
	// file so the plugin still works without a database server).
	plgdb::Config fallback;
	fallback.driver = plgdb::Driver::SQLite;
	fallback.sqlitePath = pisex::ResolveDataPath("vip.sqlite3");

	pisex::KeyValues kv("Databases");
	if (!kv.LoadFromFile("addons/configs/databases.cfg")) {
		LogError("vip: cannot read databases.cfg - using a local SQLite database");
		m_db_config = std::move(fallback);
		return false;
	}
	const pisex::KeyValues* section = kv.FindKey("vip");
	if (section == nullptr) {
		LogError("vip: databases.cfg has no 'vip' section - using a local SQLite database");
		m_db_config = std::move(fallback);
		return false;
	}

	plgdb::Config config;
	const std::string_view driver = section->GetStringValue("driver", "mysql");
	config.host = std::string(section->GetStringValue("host", "127.0.0.1"));
	config.port = static_cast<uint16_t>(section->GetInt("port", 3306));
	config.user = std::string(section->GetStringValue("user", ""));
	config.password = std::string(section->GetStringValue("pass", ""));
	config.database = std::string(section->GetStringValue("database", ""));
	const std::string sqlite_path = std::string(section->GetStringValue("sqlite_path", "vip.sqlite3"));

	if (driver == "sqlite") {
		config.driver = plgdb::Driver::SQLite;
		config.sqlitePath = sqlite_path;
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
	pisex::KeyValues kv("VIP");
	if (!kv.LoadFromFile("addons/configs/vip/groups.ini")) {
		LogError("vip: failed to load the vip config 'addons/configs/vip/groups.ini'");
		return false;
	}

	Config config = m_config;
	config.server_id = kv.GetInt("server_id", 0);

	GroupTable groups;
	// The original's key walk advanced from the current key. Advancing from the root instead
	// stopped after the first child - the "server_id" value - so no group was ever loaded and
	// every VIP got "NotAccess".
	std::size_t group_count = 0;
	for (pisex::KeyValues* key = kv.GetFirstSubKey(); key != nullptr; key = key->GetNextKey()) {
		GroupFeatures group;
		FOR_EACH_VALUE(key, value) {
			group[value->GetName()] = std::string(value->GetStringValue());
		}
		if (!group.empty()) {
			++group_count; // "server_id" is a plain value, not a group
		}
		groups[std::string(key->GetName())] = std::move(group);
	}
	LogInfo("vip: " + std::to_string(group_count) + " group(s) loaded from groups.ini (server_id " +
			std::to_string(config.server_id) + ")");

	m_config = config;
	if (m_core != nullptr) {
		m_core->SetConfig(m_config);
		m_core->SetGroups(std::move(groups));
	}
	return true;
}

bool VipPlugin::LoadPhrases() {
	// The g_vecPhrases map of the original, from vip.phrases.txt (the same file the package
	// ships). The original read the phrases for the server's current language; the shipped file
	// carries that language in its own "language" key ("ru"), which is what the other ports use and
	// makes the server language detection (which cannot run before a map exists) irrelevant.
	pisex::KeyValues kv("Phrases");
	if (kv.LoadFromFile("addons/translations/vip.phrases.txt")) {
		if (const char* language = kv.GetString("language", ""); language != nullptr && language[0] != '\0') {
			pisex::SetPhrasesLanguage(language);
		}
	}
	if (!pisex::LoadPhrases("vip.phrases.txt")) {
		LogError("vip: failed to load 'vip.phrases.txt' - phrases print as empty strings");
		return false;
	}
	return true;
}

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
		const uint64_t steamid = pisex::g_pPlayers->GetSteamID64(slot);
		if (steamid == 0) {
			continue;
		}
		if (require_pawn && pisex::GetPawnEntity(slot) <= 0) {
			continue;
		}
		const char* name = pisex::g_pPlayers->GetPlayerName(slot);
		if (name != nullptr && std::strstr(name, token.c_str()) != nullptr) {
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
	if (m_core == nullptr || slot < 0 || slot >= kMaxSlots) {
		return;
	}
	SlotState& state = m_slots[static_cast<size_t>(slot)];
	const uint64_t steamid = state.steamid != 0 ? state.steamid : pisex::g_pPlayers->GetSteamID64(slot);
	if (steamid == 0) {
		return;
	}
	state.steamid = steamid;
	state.account_id = AccountId(steamid);
	if (state.name.empty()) {
		state.name = PlayerName(slot);
	}
	state.requested = true;
	state.loaded = false;
	state.client_loaded_fired = false;
	state.user_loaded_fired = false;
	state.user_loaded_seen = false;
	state.requested_at = std::chrono::steady_clock::now();
	m_core->LoadPlayer(slot, IdentityOf(slot), LoadPurpose::Reload);
}

void VipPlugin::GiveOfflineVip(const std::string& account_id_text, int32_t seconds, const std::string& group) {
	// vip_give, offline branch: a plain INSERT of the typed account id (name 'none').
	if (m_core == nullptr || account_id_text.empty()) {
		return;
	}
	m_core->InsertRow(account_id_text, seconds, group, m_core->Now());
}

// --- startup ---------------------------------------------------------------------------------

bool VipPlugin::EnsureCore() {
	// A missing or unusable databases.cfg means a local SQLite file (a fail-soft fallback, like
	// the modules' fallback of the admin core); only a config that cannot produce a connection
	// stops the core.
	LoadDatabaseConfig();
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

	try {
		m_core->CreateSchema();
	} catch (const std::exception& ex) {
		LogError(std::string("vip: ") + ex.what());
		m_core.reset();
		m_db.reset();
		return false;
	}

	m_ready = true;
	// PARITY: the OnVIPLoaded callbacks fire and the core goes ready once the CREATE TABLE ran.
	FireReady();
	LogInfo("vip: core ready (server_id " + std::to_string(m_config.server_id) + ")");
	return true;
}

void VipPlugin::EnsurePermissionsListener() {
	// The permissions plugin is an optional dependency, so this may run before (or without) it:
	// it is retried from the update pump until the binding slots are there.
	if (m_permissions_listener_registered) {
		return;
	}
	if (__permissions_OnUserLoaded_Register == nullptr) {
		if (!m_permissions_warned) {
			m_permissions_warned = true;
			LogError("vip: the permissions plugin is not available - cookies are disabled until it loads");
		}
		return;
	}
	m_permissions_listener_registered = true;
	try {
		permissions::OnUserLoaded_Register(&OnUserLoadedTrampoline);
		LogInfo("vip: the permissions cookie store is available");
	} catch (const std::exception& ex) {
		LogError(std::string("vip: registering the permissions OnUserLoaded listener failed: ") + ex.what());
	} catch (...) {
		LogError("vip: registering the permissions OnUserLoaded listener failed");
	}
}

void VipPlugin::RegisterListeners() {
	s2sdk::OnClientAuthenticated_Register(&OnClientAuthenticatedTrampoline);
	s2sdk::OnClientPutInServer_Register(&OnClientPutInServerTrampoline);
	s2sdk::OnClientDisconnect_Post_Register(&OnClientDisconnectTrampoline);
	m_listeners_registered = true;
}

void VipPlugin::RegisterCommandsAndEvents() {
	RegisterCommands();
	pisex::HookEvent("player_spawn", &OnPlayerSpawnEvent);
	pisex::HookEvent("round_prestart", &OnRoundPreStartEvent);
	m_events_registered = true;
}

plg::PluginResult VipPlugin::OnPluginStart() {
	try {
		pisex::Init("vip");
		LoadPhrases();
		EnsurePermissionsListener();

		if (!EnsureCore()) {
			// Fail soft: the plugin stays loaded, the API answers with neutral values.
			LogError("vip: the core is not ready - the plugin runs as a no-op");
			return plg::PluginResult{};
		}

		RegisterListeners();
		RegisterCommandsAndEvents();

		// OnStartupServer of the original: delete the expired rows of this server.
		pisex::g_pUtils->StartupServer([] {
			try {
				g_vipPlugin.OnStartupServer();
			} catch (const std::exception& ex) {
				LogError(std::string("vip: exception in the startup callback: ") + ex.what());
			} catch (...) {
				LogError("vip: unknown exception in the startup callback");
			}
		});

		// The one-second loop of the original (expiry) plus the slot reconcile.
		m_second_timer = pisex::g_pUtils->CreateTimer(kSecondTimerInterval, [] {
			try {
				g_vipPlugin.OnSecondTick();
			} catch (const std::exception& ex) {
				LogError(std::string("vip: exception in the second timer: ") + ex.what());
			} catch (...) {
				LogError("vip: unknown exception in the second timer");
			}
			return kSecondTimerInterval;
		});

		LogInfo("vip: plugin started");
	} catch (const std::exception& ex) {
		LogError(std::string("vip: exception in OnPluginStart: ") + ex.what());
	} catch (...) {
		LogError("vip: unknown exception in OnPluginStart");
	}
	return plg::PluginResult{};
}

plg::PluginResult VipPlugin::OnPluginUpdate(std::chrono::milliseconds dt) {
	try {
		// pisexcompat first: it applies the deferred startup callbacks, the timer reschedules and
		// the menu requests that came out of engine callbacks (which run under s2sdk's locks).
		pisex::Poll();
		if (m_db != nullptr) {
			m_db->Poll();
		}
		// The optional permissions dependency may appear after this plugin started.
		EnsurePermissionsListener();
		// Fallback for a server whose s2sdk timer binding is missing: keep the one-second work
		// (reconcile + expiry) running from the update pump.
		if (m_second_timer == nullptr) {
			m_tick_accumulator += dt;
			if (m_tick_accumulator >= std::chrono::seconds(1)) {
				m_tick_accumulator = std::chrono::milliseconds(0);
				OnSecondTick();
			}
		}
	} catch (const std::exception& ex) {
		LogError(std::string("vip: exception in OnPluginUpdate: ") + ex.what());
	} catch (...) {
		LogError("vip: unknown exception in OnPluginUpdate");
	}
	return plg::PluginResult{};
}

plg::PluginResult VipPlugin::OnPluginEnd() {
	try {
		if (m_permissions_listener_registered && __permissions_OnUserLoaded_Unregister != nullptr) {
			try {
				permissions::OnUserLoaded_Unregister(&OnUserLoadedTrampoline);
			} catch (...) {
			}
			m_permissions_listener_registered = false;
		}
		if (m_listeners_registered) {
			s2sdk::OnClientAuthenticated_Unregister(&OnClientAuthenticatedTrampoline);
			s2sdk::OnClientPutInServer_Unregister(&OnClientPutInServerTrampoline);
			s2sdk::OnClientDisconnect_Post_Unregister(&OnClientDisconnectTrampoline);
			m_listeners_registered = false;
		}
		m_events_registered = false; // pisex::Shutdown() drops the event hooks
		pisex::Shutdown();
		// The core has to go before the database (its callbacks hold a lifetime token).
		m_core.reset();
		m_db.reset();
		m_ready = false;
	} catch (const std::exception& ex) {
		LogError(std::string("vip: exception in OnPluginEnd: ") + ex.what());
	} catch (...) {
		LogError("vip: unknown exception in OnPluginEnd");
	}
	return plg::PluginResult{};
}

// --- engine side ------------------------------------------------------------------------------

void VipPlugin::HandleClientAuthenticated(int32_t slot, uint64_t steamid) {
	if (slot < 0 || slot >= kMaxSlots || steamid == 0 || m_core == nullptr) {
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
	state.requested_at = std::chrono::steady_clock::now();
	m_core->LoadPlayer(slot, IdentityOf(slot), LoadPurpose::Connect);
}

void VipPlugin::HandleClientPutInServer(int32_t slot) {
	// OnClientPutInServer of the original: when the record of the player is already known (a
	// reconnect in the same session) the Welcome message is printed right away.
	if (slot < 0 || slot >= kMaxSlots || m_core == nullptr) {
		return;
	}
	const SlotState& state = m_slots[static_cast<size_t>(slot)];
	const uint64_t steamid = state.steamid != 0 ? state.steamid : pisex::g_pPlayers->GetSteamID64(slot);
	if (steamid == 0) {
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
	if (slot < 0 || slot >= kMaxSlots) {
		return;
	}
	if (m_core != nullptr) {
		// OnClientDisconnect_Post runs after the engine processed the disconnect, so the live
		// engine queries behind IsClientVip may already read false (IsSlotHuman requires
		// IsConnected/IsAuthenticated): the check uses the tracked slot state instead.
		const uint32_t account_id = AccountIdOf(slot);
		const bool wasVip = account_id != 0 && m_core->IsClientVip(IdentityOf(slot), m_core->Now());
		FireClientDisconnect(slot, wasVip);
	}
	ClearSlot(slot);
}

void VipPlugin::HandlePlayerSpawn(const pisex::IGameEvent& event) {
	// PARITY (OnPlayerSpawn): the controller is looked up from the event, bots are skipped, and
	// the work happens one frame later (the pawn is not always alive yet inside the event).
	const int32_t slot = event.GetPlayerSlot("userid");
	if (slot < 0 || slot >= kMaxSlots) {
		return;
	}
	if (pisex::g_pPlayers->GetSteamID64(slot) == 0) {
		return; // ignore bots
	}
	pisex::g_pUtils->NextFrame([slot] {
		try {
			if (!pisex::g_pPlayers->IsAlive(slot)) {
				return;
			}
			const bool isVip = g_vipPlugin.IsClientVip(slot);
			g_vipPlugin.FirePlayerSpawn(slot, pisex::GetPlayerTeam(slot), isVip);
		} catch (const std::exception& ex) {
			LogError(std::string("vip: exception in the deferred spawn handler: ") + ex.what());
		} catch (...) {
			LogError("vip: unknown exception in the deferred spawn handler");
		}
	});
}

void VipPlugin::HandleRoundPreStart() {
	if (s2sdk::GetGameRules() == nullptr) {
		return;
	}
	// PARITY (OnRoundPreStart): a pistol round is one with no rounds played yet, a team switch at
	//     round reset with no overtime playing, or a game restart.
	const int total = GameRulesInt("m_totalRoundsPlayed", TotalRounds());
	const bool switching = GameRulesBool("m_bSwitchingTeamsAtRoundReset", false);
	const int overtime = GameRulesInt("m_nOvertimePlaying", 0);
	const bool restart = GameRulesBool("m_bGameRestart", false);
	m_pistol_round = total == 0 || (switching && overtime == 0) || restart;
}

void VipPlugin::OnStartupServer() {
	if (m_core != nullptr) {
		m_core->DeleteExpiredRows();
	}
}

void VipPlugin::OnSecondTick() {
	if (m_core == nullptr) {
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
		if (!IsSlotHuman(slot)) {
			if (state.requested && !pisex::g_pPlayers->IsConnected(slot)) {
				ClearSlot(slot); // the disconnect event was missed
			}
			continue;
		}
		const uint64_t steamid = pisex::g_pPlayers->GetSteamID64(slot);
		if (steamid == 0) {
			continue;
		}
		if (state.requested && state.steamid == steamid) {
			// A load that never answered (a lost statement) is retried after a timeout.
			if (!state.loaded && now - state.requested_at >= kLoadRetryAfter) {
				state.requested_at = now;
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
		state.requested_at = now;
		m_core->LoadPlayer(slot, IdentityOf(slot), LoadPurpose::Connect);
	}
}

void VipPlugin::ExpireLoop() {
	// PARITY (GameFrame): every second, for every connected/authenticated non-bot player, a VIP
	// whose TimeEnd passed is forgotten: callback, the three chat lines and the DELETE.
	const int64_t now = m_core->Now();
	for (int32_t slot = 0; slot < kMaxSlots; ++slot) {
		if (!IsSlotHuman(slot)) {
			continue;
		}
		const uint32_t account_id = AccountIdOf(slot);
		if (account_id == 0) {
			continue;
		}
		if (m_core->ExpirePlayer(account_id, now)) {
			m_core->ClearSlotFeatures(slot);
			FireVipRemoved(slot, kRemoveExpired);
			NotifyExpired(slot);
			m_core->DeleteRow(account_id);
		}
	}
}

// --- vip_core hooks -----------------------------------------------------------------------------

void VipPlugin::HandleLoaded(int32_t slot, const VipLoadOutcome& outcome, LoadPurpose purpose) {
	if (slot < 0 || slot >= kMaxSlots) {
		return;
	}
	SlotState& state = m_slots[static_cast<size_t>(slot)];
	if (state.account_id != outcome.account_id) {
		// The slot was reused (or cleared) while the statement was in flight: the result belongs
		// to a previous occupant and must not mark the new player's slot as loaded.
		return;
	}
	state.loaded = true;

	const bool isVip = outcome.row_found && !outcome.expired && m_core != nullptr && m_core->IsClientVip(IdentityOf(slot), m_core->Now());

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

// --- slot data / helpers ----------------------------------------------------------------------------

bool VipPlugin::IsSlotHuman(int32_t slot) const {
	if (slot < 0 || slot >= kMaxSlots || !pisex::IsGameReady()) {
		return false;
	}
	return pisex::g_pPlayers->IsConnected(slot) && pisex::g_pPlayers->IsAuthenticated(slot) &&
		   !pisex::g_pPlayers->IsFakeClient(slot);
}

uint64_t VipPlugin::SteamIdOf(int32_t slot) const {
	if (slot < 0 || slot >= kMaxSlots) {
		return 0;
	}
	const SlotState& state = m_slots[static_cast<size_t>(slot)];
	return state.steamid;
}

uint32_t VipPlugin::AccountIdOf(int32_t slot) const {
	if (slot < 0 || slot >= kMaxSlots) {
		return 0;
	}
	return m_slots[static_cast<size_t>(slot)].account_id;
}

std::string VipPlugin::PlayerName(int32_t slot) const {
	if (slot < 0 || slot >= kMaxSlots) {
		return {};
	}
	const SlotState& state = m_slots[static_cast<size_t>(slot)];
	if (!state.name.empty()) {
		return state.name;
	}
	return pisex::g_pPlayers->GetPlayerName(slot);
}

VipIdentity VipPlugin::IdentityOf(int32_t slot) const {
	VipIdentity identity;
	if (slot < 0 || slot >= kMaxSlots) {
		return identity;
	}
	const SlotState& state = m_slots[static_cast<size_t>(slot)];
	identity.slot = slot;
	identity.account_id = state.account_id;
	identity.steamid64 = state.steamid;
	identity.name = PlayerName(slot);
	return identity;
}

void VipPlugin::ClearSlot(int32_t slot) {
	if (slot < 0 || slot >= kMaxSlots) {
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
		pisex::PrintToChat(slot, "%s", CStr(pisex::FormatString(CStr(Phrase("WelcomePerm")), CStr(name))));
	} else {
		const std::string when = FormatTimestamp(expires);
		pisex::PrintToChat(slot, "%s", CStr(pisex::FormatString(CStr(Phrase("Welcome")), CStr(name), CStr(when))));
	}
	(void)group;
}

void VipPlugin::NotifyExpired(int32_t slot) const {
	// PARITY: VIPExpired1..3.
	pisex::PrintToChat(slot, "%s", CStr(Phrase("VIPExpired1")));
	pisex::PrintToChat(slot, "%s", CStr(Phrase("VIPExpired2")));
	pisex::PrintToChat(slot, "%s", CStr(Phrase("VIPExpired3")));
}

int VipPlugin::GameRulesInt(const char* field, int def) const {
	// The gamerules object of s2sdk has no accessor for every field the original used; the offset
	// comes from the game schema (pisex::SchemaOffset returns -1 for an unknown field, so an
	// engine update degrades to the default instead of reading a wrong address).
	void* rules = s2sdk::GetGameRules();
	if (rules == nullptr || field == nullptr) {
		return def;
	}
	const int offset = pisex::SchemaOffset("CCSGameRules", field);
	if (offset < 0) {
		return def;
	}
	int value = 0;
	std::memcpy(&value, static_cast<const char*>(rules) + offset, sizeof(value));
	return value;
}

bool VipPlugin::GameRulesBool(const char* field, bool def) const {
	void* rules = s2sdk::GetGameRules();
	if (rules == nullptr || field == nullptr) {
		return def;
	}
	const int offset = pisex::SchemaOffset("CCSGameRules", field);
	if (offset < 0) {
		return def;
	}
	unsigned char value = 0;
	std::memcpy(&value, static_cast<const char*>(rules) + offset, sizeof(value));
	return value != 0;
}

// --- VIPApi surface -------------------------------------------------------------------------------

bool VipPlugin::IsClientVip(int32_t slot) {
	// PARITY (VIP_IsClientVIP): authenticated client with a record, unexpired and a valid group.
	if (m_core == nullptr || !IsSlotHuman(slot)) {
		return false;
	}
	const uint32_t account_id = AccountIdOf(slot);
	if (account_id == 0) {
		return false;
	}
	return m_core->IsClientVip(IdentityOf(slot), m_core->Now());
}

int32_t VipPlugin::AccessTime(int32_t slot) {
	if (m_core == nullptr || !IsSlotHuman(slot)) {
		return -1;
	}
	const uint32_t account_id = AccountIdOf(slot);
	if (account_id == 0) {
		return -1;
	}
	return static_cast<int32_t>(m_core->AccessTime(IdentityOf(slot), m_core->Now()));
}

bool VipPlugin::SetAccessTime(int32_t slot, int32_t time, bool in_db) {
	if (m_core == nullptr || !IsSlotHuman(slot) || AccountIdOf(slot) == 0) {
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
	if (m_core == nullptr || !IsSlotHuman(slot) || AccountIdOf(slot) == 0) {
		return false;
	}
	return m_core->SetGroup(IdentityOf(slot), group, in_db, m_core->Now());
}

bool VipPlugin::GiveVip(int32_t slot, int32_t time, const std::string& group, bool add_to_db) {
	if (m_core == nullptr || !IsSlotHuman(slot) || AccountIdOf(slot) == 0) {
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
	if (m_core == nullptr || !IsSlotHuman(slot) || AccountIdOf(slot) == 0) {
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
	const uint32_t account_id = AccountIdOf(slot);
	if (account_id == 0) {
		return nullptr;
	}
	return m_core->ResolveFeatures(slot, account_id, m_core->Now());
}

std::string VipPlugin::DiagnoseSlot(int32_t slot) {
	if (slot < 0 || slot >= kMaxSlots || !pisex::IsGameReady() || !pisex::g_pPlayers->IsConnected(slot)) {
		return {};
	}
	const bool authenticated = pisex::g_pPlayers->IsAuthenticated(slot);
	const bool fake = pisex::g_pPlayers->IsFakeClient(slot);
	const uint32_t account_id = AccountIdOf(slot);
	std::string text = "vip_status slot " + std::to_string(slot) + " '" + PlayerName(slot) +
					   "': authenticated=" + (authenticated ? "1" : "0") + " fake=" + (fake ? "1" : "0") +
					   " account_id=" + std::to_string(account_id);
	if (m_core == nullptr) {
		return text + " core=none";
	}
	const VipPlayer* player = account_id != 0 ? m_core->FindPlayer(account_id) : nullptr;
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
	Feature& entry = m_features[ToString(feature)];
	entry.value_type = value_type;
	entry.feature_type = feature_type;
	entry.select = select;
	entry.toggle = toggle;
	entry.display = display;
}

// --- cookies (the ICookiesApi of the original, on the official permissions plugin) -----------------

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
		const permissions::Status status = permissions::SetCookie(static_cast<int64_t>(GetId()), steamid,
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
	pisex::PrintToCenter(slot, "%s", CStr(msg));
}

void VipPlugin::OpenMenu(int32_t slot) {
	ShowMenu(slot, true);
}

// --- game state ------------------------------------------------------------------------------------

bool VipPlugin::WarmupPeriod() {
	// m_bWarmupPeriod of the game rules, through the game schema (see GameRulesInt).
	return GameRulesBool("m_bWarmupPeriod", false);
}

int32_t VipPlugin::TotalRounds() {
	// m_totalRoundsPlayed of the game rules; s2sdk has an accessor for this one.
	if (__s2sdk_GetGameTotalRoundsPlayed == nullptr || s2sdk::GetGameRules() == nullptr) {
		return 0;
	}
	try {
		return s2sdk::GetGameTotalRoundsPlayed();
	} catch (...) {
		return 0;
	}
}

// --- callback registries ------------------------------------------------------------------------------

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
		return plg::string(g_vipPlugin.Phrase(ToString(phrase).c_str()));
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
