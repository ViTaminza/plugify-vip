#pragma once
// Engine-independent core of the "vip" plugin: the VIP group table (groups.ini), the per-player
// state (g_VipPlayer of the original, vip_users in the database), the feature-value rules with
// the '<group>.<feature>' toggle cookies, and the expiry checks.
//
// No Plugify, s2sdk or engine headers here - only plgdb - so the whole module is unit-testable
// against SQLite and MySQL (tests/vip_core_tests.cpp). The plugin (src/plugin.cpp) owns the
// engine half: player lookups (slot -> account id), the permission cookies, the menu, the chat
// messages and the once-per-second slot loop; it feeds this core a VipIdentity and applies what
// the hooks report back.
//
// Compatibility contract with the original Metamod plugin (ref/src/cs2-vip/vip.cpp):
//   * the vip_users table, its columns and the queries are those of the original (so a Metamod
//     server and this port can share one database); account ids are the 32 bit `account_id`
//     column of the original, not steamid64;
//   * the in-memory rules are the ones of VIPApi: a record whose TimeEnd is 0 is permanent, a
//     record with 0 < TimeEnd <= now counts as expired for the API checks (`<=`) but is only
//     dropped by the per-second loop at `<` - both verbatim from the original;
//   * the feature values follow VIP_GetClientFeature*: a missing, off (cookie "0") or unknown
//     feature answers the documented neutral value, cookies with any other non-zero number mean
//     "on";
//   * GetGroupKV's 'legacy' inheritance is kept, including "a child value wins over its parent
//     and is never overwritten". One deliberate deviation: a 'legacy' cycle stops the walk
//     instead of hanging the server (marked PARITY (safety)).
//   * Every statement is asynchronous (plgdb) and its effects are applied from the callback,
//     which plgdb only runs inside Poll() on the game thread; the only blocking statement is
//     CreateSchema(), which the plugin calls once at startup.

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <plgdb/db.hpp>

namespace vip {

// CS2 has 64 player slots.
inline constexpr int kMaxPlayers = 64;

// The two reasons VIP_OnVIPClientRemoved fires with (the iReason of the original).
inline constexpr int kRemoveExpired = 1;
inline constexpr int kRemoveByApi = 2;

// group -> feature -> value, the g_VipGroups of the original.
using GroupFeatures = std::map<std::string, std::string, std::less<>>;
using GroupTable = std::map<std::string, GroupFeatures, std::less<>>;

// g_VipPlayer[account_id] of the original.
struct VipPlayer {
	std::string group;
	int64_t time_end = 0; // 0 = permanent, otherwise an absolute unix timestamp
};

// --- config ---------------------------------------------------------------------
struct Config {
	int64_t server_id = 0; // "server_id" of groups.ini, the `sid` column of vip_users
};

// --- player identity -------------------------------------------------------------
// The engine half of a player, filled by the plugin: the core never asks the engine.
struct VipIdentity {
	int slot = -1;
	uint32_t account_id = 0;  // the 32 bit account id of the steamid (m_steamID of the original)
	uint64_t steamid64 = 0;   // only used by the plugin-side cookies
	std::string name;         // the name stored in vip_users
};

// --- async load result (CheckVIP() / mm_reload_vip of the original) ---------------
struct VipLoadOutcome {
	bool row_found = false;    // a vip_users row was read
	bool expired = false;      // ... but its `expires` has passed (0 = permanent is never expired)
	bool row_deleted = false;  // the expired row was DELETEed (what the original did)
	std::string group;
	int64_t expires = 0; // 0 = permanent
};

// Why the row was read; both variant print the Welcome message on success, the connect path also
// refreshes name/lastvisit like OnClientAuthorized did.
enum class LoadPurpose {
	Connect, // HookOnClientAuthorized / the once-per-second reconcile
	Reload,  // mm_reload_vip
};

// --- feature values ----------------------------------------------------------------
// VIP_ValueType / VIP_ToggleState of include/vip.h, kept as plain integers at the core boundary
// (the generated enum classes of the API map onto them one to one).
inline constexpr int kValueNull = 0;
inline constexpr int kValueInt = 1;
inline constexpr int kValueFloat = 2;
inline constexpr int kValueBool = 3;
inline constexpr int kValueString = 4;

inline constexpr int kToggleDisabled = 0;
inline constexpr int kToggleEnabled = 1;

// VIP_FeatureType of include/vip.h.
inline constexpr int kFeatureTogglable = 0;
inline constexpr int kFeatureSelectable = 1;
inline constexpr int kFeatureHide = 2;

namespace features {

// The '<group>.<feature>' cookie semantics of VIP_GetClientFeature*: an empty cookie or one whose
// atoi() is non-zero means "enabled" (use the group value), the cookie "0" means "switched off".
bool IsEnabled(std::string_view cookie);

// The four VIP_GetClientFeature* computations. |group| is the resolved feature table of the
// player, |feature| the key; a missing entry answers the same neutral value as a switched-off
// feature, exactly like the original's `Group[szFeature] != "" ? ... : neutral`.
int64_t GetInt(const GroupFeatures& group, std::string_view feature, std::string_view cookie);
bool GetBool(const GroupFeatures& group, std::string_view feature, std::string_view cookie);
float GetFloat(const GroupFeatures& group, std::string_view feature, std::string_view cookie);
std::string GetString(const GroupFeatures& group, std::string_view feature, std::string_view cookie);

} // namespace features

// --- the core -----------------------------------------------------------------------
class VipCore {
public:
	explicit VipCore(plgdb::Database& db);
	~VipCore();

	VipCore(const VipCore&) = delete;
	VipCore& operator=(const VipCore&) = delete;

	// --- configuration -------------------------------------------------------------
	void SetConfig(Config config);
	const Config& GetConfig() const { return m_config; }

	// LoadVips() of the original: replaces the group table and drops every per-slot feature
	// cache. The plugin parses groups.ini with the pisexcompat KeyValues parser and hands the
	// table over; server_id is part of Config.
	void SetGroups(GroupTable groups);
	const GroupTable& Groups() const { return m_groups; }
	// VIP_IsValidVIPGroup() of the original.
	bool IsValidGroup(std::string_view group) const;

	// --- schema ----------------------------------------------------------------------
	// Creates vip_users (identical to the original, including the table name and the composite
	// primary key) when it does not exist. Blocking: startup only. Throws std::runtime_error on
	// failure so the plugin can fail soft.
	void CreateSchema();

	// --- clock -------------------------------------------------------------------------
	// Overridable so tests can force expiry without sleeping.
	void SetClock(std::function<int64_t()> clock);
	int64_t Now() const;

	// --- in-memory player table (g_VipPlayer) -------------------------------------------
	const VipPlayer* FindPlayer(uint32_t account_id) const;
	// g_VipPlayer[m_steamID] = {group, timeEnd} of VIP_GiveClientVIP.
	void SetPlayer(uint32_t account_id, std::string group, int64_t time_end);
	// Returns true when a record existed (the erase of VIP_RemoveClientVIP).
	bool RemovePlayer(uint32_t account_id);

	// --- VIPApi semantics, with the engine checks done by the caller ----------------------
	// VIP_IsClientVIP: record exists, not expired and its group exists in groups.ini.
	bool IsClientVip(const VipIdentity& id, int64_t now) const;
	// VIP_GetClientAccessTime: the stored TimeEnd, or -1 when the player is not a VIP.
	int64_t AccessTime(const VipIdentity& id, int64_t now) const;
	// VIP_GetClientVIPGroup: the stored group ("" when the player has no record; like the
	// original it does not check the expiry).
	std::string GroupOf(const VipIdentity& id) const;
	// VIP_SetClientAccessTime: overwrites the expiry of an existing record.
	bool SetAccessTime(const VipIdentity& id, int64_t time, bool in_db);
	// VIP_GiveClientVIP: creates the record (now + seconds, 0 = permanent). False when the player
	// already is a VIP, like the original.
	bool GiveVip(const VipIdentity& id, int64_t seconds, const std::string& group, bool add_to_db, int64_t now);
	// VIP_RemoveClientVIP: drops the record; false when there was none.
	bool RemoveVip(const VipIdentity& id, bool in_db);
	// VIP_SetClientVIPGroup: false for an expired record or an unknown group.
	bool SetGroup(const VipIdentity& id, const std::string& group, bool in_db, int64_t now);

	// --- features --------------------------------------------------------------------------
	// GetGroupKV() of the original without its engine checks (the caller only passes a slot that
	// is a connected, fully authenticated, non-bot player): resolves the group of the record -
	// through the 'legacy' chain - caches it per slot (g_pKVUser) and returns nullptr when there
	// is nothing to return (no record, expired, unknown group, empty feature table).
	const GroupFeatures* ResolveFeatures(int slot, uint32_t account_id, int64_t now);
	// g_pKVUser[iSlot].clear().
	void ClearSlotFeatures(int slot);

	// --- per-second expiry (the GameFrame loop) ----------------------------------------------
	// True when the record existed and its TimeEnd is in the past (0 = permanent never expires);
	// the record is removed. The caller prints VIPExpired1..3, fires VIP_OnVIPClientRemoved(1)
	// and deletes the row.
	bool ExpirePlayer(uint32_t account_id, int64_t now);

	// --- asynchronous database work -------------------------------------------------------------
	// CheckVIP() of the original: reads the vip_users row of the account. A row that has expired
	// is DELETEed; a valid Connect load refreshes name/lastvisit; the Reload variant only reads.
	// The record is always replaced (the original erased it before the query) and the slot
	// feature cache is dropped; onLoaded fires in Poll() once the row was read (or not).
	void LoadPlayer(int slot, VipIdentity id, LoadPurpose purpose);
	// The startup cleanup of OnStartupServer: DELETE the expired rows of this server.
	void DeleteExpiredRows();
	// The DELETE of the expiry loop and of VIP_RemoveClientVIP with inDB (already covered by
	// RemoveVip; exposed for the loop, which has no identity).
	void DeleteRow(uint32_t account_id);
	// The INSERT of the vip_give offline path (name 'none', account_id as it was typed).
	void InsertRow(const std::string& account_id_text, int64_t seconds, const std::string& group, int64_t now);

	// --- hooks (game thread only) -----------------------------------------------------------------
	// A load finished; |outcome| reports what was found. Fires for every LoadPlayer.
	std::function<void(int slot, const VipLoadOutcome& outcome, LoadPurpose purpose)> onLoaded;
	// SQL failures and rejected values; never empty in the plugin.
	std::function<void(std::string_view)> onError;

	// The table name, exposed for the plugin and the tests.
	static constexpr std::string_view TableName() { return "vip_users"; }

private:
	bool SlotInRange(int slot) const;
	void ReportError(std::string_view message) const;
	// The shared tail of the INSERT/UPDATE/DELETE helpers.
	void Query(std::string sql, plgdb::Params params, plgdb::Callback callback = {});

	plgdb::Database& m_db;
	Config m_config;
	std::function<int64_t()> m_clock;

	GroupTable m_groups;
	std::map<uint32_t, VipPlayer> m_players;                     // g_VipPlayer
	std::array<GroupFeatures, kMaxPlayers> m_slot_features{};    // g_pKVUser
	// Kept alive by the core; callbacks that plgdb runs from Poll() hold a weak reference so a
	// result landing after the core was destroyed is dropped instead of touching freed state.
	std::shared_ptr<void> m_life;
};

} // namespace vip
