// vip_core — implementation. Every rule below carries a PARITY note naming the function of
// the original (vip.cpp) it was taken from; anything else is a deliberate, marked deviation.

#include "vip_core.hpp"

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <stdexcept>
#include <utility>
#include <vector>

namespace vip {

namespace {

// statements (the `group` column needs quoting in both MySQL and SQLite; backticks are the
// MySQL quoting SQLite accepts as well).
constexpr std::string_view kSelectPlayer = "SELECT `group`, `expires` FROM vip_users WHERE account_id = ? AND sid = ?";
constexpr std::string_view kInsertPlayer =
	"INSERT INTO vip_users (account_id, name, lastvisit, sid, `group`, expires) VALUES (?, ?, ?, ?, ?, ?)";
constexpr std::string_view kUpdateVisited = "UPDATE vip_users SET name = ?, lastvisit = ? WHERE account_id = ? AND sid = ?";
constexpr std::string_view kUpdateExpires = "UPDATE vip_users SET expires = ? WHERE account_id = ? AND sid = ?";
constexpr std::string_view kUpdateGroup = "UPDATE vip_users SET `group` = ? WHERE account_id = ? AND sid = ?";
constexpr std::string_view kDeletePlayer = "DELETE FROM vip_users WHERE account_id = ? AND sid = ?";
constexpr std::string_view kDeleteExpired = "DELETE FROM vip_users WHERE sid = ? AND expires < ? AND expires <> 0";

// The CREATE TABLE of the original, byte for byte in the MySQL variant (the reserved `group`
// column name included). SQLite gets the same columns, its own way of saying UNSIGNED/utf8 and
// the same composite primary key.
constexpr std::string_view kCreateTableMySQL =
	"CREATE TABLE IF NOT EXISTS `vip_users` ("
	"`account_id` INT NOT NULL, "
	"`name` VARCHAR(64) NOT NULL default 'unknown' COLLATE 'utf8mb4_unicode_ci', "
	"`lastvisit` INT UNSIGNED NOT NULL default 0, "
	"`sid` INT UNSIGNED NOT NULL, "
	"`group` VARCHAR(64) NOT NULL, "
	"`expires` INT UNSIGNED NOT NULL default 0, "
	"CONSTRAINT pk_PlayerID PRIMARY KEY (`account_id`, `sid`)"
	") DEFAULT CHARSET=utf8mb4;";
constexpr std::string_view kCreateTableSQLite =
	"CREATE TABLE IF NOT EXISTS `vip_users` ("
	"`account_id` INTEGER NOT NULL, "
	"`name` TEXT NOT NULL DEFAULT 'unknown', "
	"`lastvisit` INTEGER UNSIGNED NOT NULL DEFAULT 0, "
	"`sid` INTEGER UNSIGNED NOT NULL, "
	"`group` TEXT NOT NULL, "
	"`expires` INTEGER UNSIGNED NOT NULL DEFAULT 0, "
	"CONSTRAINT pk_PlayerID PRIMARY KEY (`account_id`, `sid`)"
	");";

// atoi() of the original, applied to a null-terminated copy of the value: the exact C semantics
// (leading whitespace, optional sign, digits; anything else is 0) matter because the cookie
// "abc" counts as off while "2" counts as on.
long Atoi(std::string_view text) {
	const std::string copy(text);
	return std::atoi(copy.c_str());
}

double Atof(std::string_view text) {
	const std::string copy(text);
	return std::atof(copy.c_str());
}

} // namespace

namespace features {

bool IsEnabled(std::string_view cookie) {
	// PARITY (VIP_GetClientFeature*): an empty cookie, or one whose atoi() is non-zero, is on.
	return cookie.empty() || Atoi(cookie) != 0;
}

int64_t GetInt(const GroupFeatures& group, std::string_view feature, std::string_view cookie) {
	if (!IsEnabled(cookie)) {
		return -1;
	}
	const auto it = group.find(feature);
	// PARITY: a missing feature answers the neutral -1 (the original's atoi of nothing).
	return it != group.end() && !it->second.empty() ? static_cast<int64_t>(Atoi(it->second)) : -1;
}

bool GetBool(const GroupFeatures& group, std::string_view feature, std::string_view cookie) {
	if (!IsEnabled(cookie)) {
		return false;
	}
	const auto it = group.find(feature);
	return it != group.end() && !it->second.empty() ? Atoi(it->second) != 0 : false;
}

float GetFloat(const GroupFeatures& group, std::string_view feature, std::string_view cookie) {
	if (!IsEnabled(cookie)) {
		return 1.0f;
	}
	const auto it = group.find(feature);
	return it != group.end() && !it->second.empty() ? static_cast<float>(Atof(it->second)) : 1.0f;
}

std::string GetString(const GroupFeatures& group, std::string_view feature, std::string_view cookie) {
	if (!IsEnabled(cookie)) {
		return {};
	}
	const auto it = group.find(feature);
	return it != group.end() ? it->second : std::string();
}

} // namespace features

// --- the core -------------------------------------------------------------------------

VipCore::VipCore(plgdb::Database& db) : m_db(db), m_life(std::make_shared<int>(0)) {
	m_clock = [] { return static_cast<int64_t>(std::time(nullptr)); };
}

VipCore::~VipCore() {
	// Callbacks still queued in plgdb find an expired token and do nothing.
	m_life.reset();
}

void VipCore::SetConfig(Config config) {
	m_config = std::move(config);
}

void VipCore::SetClock(std::function<int64_t()> clock) {
	m_clock = clock ? std::move(clock)
					: std::function<int64_t()>([] { return static_cast<int64_t>(std::time(nullptr)); });
}

int64_t VipCore::Now() const {
	return m_clock();
}

void VipCore::SetGroups(GroupTable groups) {
	// PARITY (LoadVips): the group table is replaced and every per-slot cache is dropped.
	m_groups = std::move(groups);
	for (GroupFeatures& cached : m_slot_features) {
		cached.clear();
	}
}

bool VipCore::IsValidGroup(std::string_view group) const {
	const auto it = m_groups.find(group);
	return it != m_groups.end() && !it->second.empty();
}

void VipCore::CreateSchema() {
	const std::string_view ddl = m_db.GetDriver() == plgdb::Driver::MySQL ? kCreateTableMySQL : kCreateTableSQLite;
	const plgdb::Result result = m_db.QuerySync(std::string(ddl), {});
	if (!result.Ok()) {
		throw std::runtime_error(std::string("vip: creating the vip_users table failed: ") + result.Error());
	}
}

bool VipCore::SlotInRange(int slot) const {
	return slot >= 0 && slot < kMaxPlayers;
}

void VipCore::ReportError(std::string_view message) const {
	if (onError) {
		onError(message);
	}
}

void VipCore::Query(std::string sql, plgdb::Params params, plgdb::Callback callback) {
	// Every core callback goes through here, so one lifetime token is enough: a result that lands
	// after the core is gone is dropped instead of touching freed state.
	std::weak_ptr<void> token = m_life;
	m_db.Query(std::move(sql), std::move(params),
			   [token, callback = std::move(callback)](const plgdb::Result& result) {
				   if (token.expired() || !callback) {
					   return;
				   }
				   callback(result);
			   });
}

// --- in-memory player table -------------------------------------------------------------

const VipPlayer* VipCore::FindPlayer(uint32_t account_id) const {
	const auto it = m_players.find(account_id);
	return it == m_players.end() ? nullptr : &it->second;
}

void VipCore::SetPlayer(uint32_t account_id, std::string group, int64_t time_end) {
	VipPlayer& player = m_players[account_id];
	player.group = std::move(group);
	player.time_end = time_end;
}

bool VipCore::RemovePlayer(uint32_t account_id) {
	return m_players.erase(account_id) > 0;
}

// --- VIPApi semantics ----------------------------------------------------------------------

bool VipCore::IsClientVip(const VipIdentity& id, int64_t now) const {
	// PARITY (VIP_IsClientVIP): record exists, not expired (<=, 0 = permanent), group in groups.ini.
	const VipPlayer* player = FindPlayer(id.account_id);
	if (player == nullptr) {
		return false;
	}
	if (player->time_end <= now && player->time_end != 0) {
		return false;
	}
	return IsValidGroup(player->group);
}

int64_t VipCore::AccessTime(const VipIdentity& id, int64_t now) const {
	// PARITY (VIP_GetClientAccessTime): -1 for "not a VIP", else the stored TimeEnd.
	const VipPlayer* player = FindPlayer(id.account_id);
	if (player == nullptr) {
		return -1;
	}
	if (player->time_end <= now && player->time_end != 0) {
		return -1;
	}
	return player->time_end;
}

std::string VipCore::GroupOf(const VipIdentity& id) const {
	const VipPlayer* player = FindPlayer(id.account_id);
	return player == nullptr ? std::string() : player->group;
}

bool VipCore::SetAccessTime(const VipIdentity& id, int64_t time, bool in_db) {
	// PARITY (VIP_SetClientAccessTime): overwrites the expiry of an existing record.
	const auto it = m_players.find(id.account_id);
	if (it == m_players.end()) {
		return false;
	}
	it->second.time_end = time;
	if (in_db) {
		Query(std::string(kUpdateExpires), {time, static_cast<int64_t>(id.account_id), m_config.server_id},
			  [this](const plgdb::Result& result) {
				  if (!result.Ok()) {
					  ReportError("vip: updating `expires` failed: " + result.Error());
				  }
			  });
	}
	return true;
}

bool VipCore::GiveVip(const VipIdentity& id, int64_t seconds, const std::string& group, bool add_to_db, int64_t now) {
	// PARITY (VIP_GiveClientVIP): fails when the record already exists (an expired-but-present
	// record counts as existing, exactly like the original's map lookup).
	if (m_players.find(id.account_id) != m_players.end()) {
		return false;
	}
	const int64_t time_end = seconds != 0 ? now + seconds : 0;
	SetPlayer(id.account_id, group, time_end);
	if (add_to_db) {
		Query(std::string(kInsertPlayer),
			  {static_cast<int64_t>(id.account_id), id.name, now, m_config.server_id, group, time_end},
			  [this](const plgdb::Result& result) {
				  if (!result.Ok()) {
					  ReportError("vip: inserting the vip_users row failed: " + result.Error());
				  }
			  });
	}
	return true;
}

bool VipCore::RemoveVip(const VipIdentity& id, bool in_db) {
	// PARITY (VIP_RemoveClientVIP): false when there was no record.
	if (!RemovePlayer(id.account_id)) {
		return false;
	}
	if (SlotInRange(id.slot)) {
		m_slot_features[static_cast<size_t>(id.slot)].clear();
	}
	if (in_db) {
		DeleteRow(id.account_id);
	}
	return true;
}

bool VipCore::SetGroup(const VipIdentity& id, const std::string& group, bool in_db, int64_t now) {
	// PARITY (VIP_SetClientVIPGroup): record must exist, be unexpired and the group must be in
	// groups.ini.
	const auto it = m_players.find(id.account_id);
	if (it == m_players.end()) {
		return false;
	}
	if (it->second.time_end <= now && it->second.time_end != 0) {
		return false;
	}
	if (!IsValidGroup(group)) {
		return false;
	}
	it->second.group = group;
	if (SlotInRange(id.slot)) {
		m_slot_features[static_cast<size_t>(id.slot)].clear();
	}
	if (in_db) {
		Query(std::string(kUpdateGroup), {group, static_cast<int64_t>(id.account_id), m_config.server_id},
			  [this](const plgdb::Result& result) {
				  if (!result.Ok()) {
					  ReportError("vip: updating `group` failed: " + result.Error());
				  }
			  });
	}
	return true;
}

// --- features ------------------------------------------------------------------------------

const GroupFeatures* VipCore::ResolveFeatures(int slot, uint32_t account_id, int64_t now) {
	if (!SlotInRange(slot)) {
		return nullptr;
	}
	GroupFeatures& cached = m_slot_features[static_cast<size_t>(slot)];
	// PARITY (GetGroupKV): a non-empty cache wins, empty results are recomputed.
	if (!cached.empty()) {
		return &cached;
	}
	const VipPlayer* player = FindPlayer(account_id);
	if (player == nullptr) {
		return nullptr;
	}
	if (player->time_end <= now && player->time_end != 0) {
		return nullptr;
	}
	const auto groupIt = m_groups.find(player->group);
	if (groupIt == m_groups.end() || groupIt->second.empty()) {
		return nullptr;
	}

	GroupFeatures resolved = groupIt->second;
	const auto legacy = resolved.find("legacy");
	if (legacy == resolved.end() || legacy->second.empty()) {
		cached = std::move(resolved);
		return &cached;
	}

	// PARITY (GetGroupKV legacy chain): walk parent groups through the 'legacy' key, copying only
	// keys the child does not already have. The original followed the chain without a visited set
	// (a 'legacy' cycle hung the server); PARITY (safety): a cycle stops the walk here.
	std::string next = legacy->second;
	std::vector<std::string> visited;
	while (!next.empty()) {
		if (std::find(visited.begin(), visited.end(), next) != visited.end()) {
			ReportError("vip: 'legacy' cycle in group '" + player->group + "' (at '" + next + "'), stopping the walk");
			break;
		}
		visited.push_back(next);
		const auto parentIt = m_groups.find(next);
		if (parentIt == m_groups.end() || parentIt->second.empty()) {
			break;
		}
		const GroupFeatures& parent = parentIt->second;
		for (const auto& [key, value] : parent) {
			GroupFeatures::iterator entry = resolved.find(key);
			if (entry == resolved.end() || entry->second.empty()) {
				resolved[key] = value;
			}
		}
		const auto parentLegacy = parent.find("legacy");
		next = parentLegacy == parent.end() ? std::string() : parentLegacy->second;
	}

	cached = std::move(resolved);
	return &cached;
}

void VipCore::ClearSlotFeatures(int slot) {
	// PARITY: the original cleared the slot's cached feature table here.
	if (SlotInRange(slot)) {
		m_slot_features[static_cast<size_t>(slot)].clear();
	}
}

// --- per-second expiry --------------------------------------------------------------------

bool VipCore::ExpirePlayer(uint32_t account_id, int64_t now) {
	// PARITY (GameFrame): a record expires when its TimeEnd is non-zero and strictly before now
	// (the API checks use <=).
	const auto it = m_players.find(account_id);
	if (it == m_players.end()) {
		return false;
	}
	if (it->second.time_end != 0 && it->second.time_end < now) {
		m_players.erase(it);
		return true;
	}
	return false;
}

// --- asynchronous database work --------------------------------------------------------------

void VipCore::LoadPlayer(int slot, VipIdentity id, LoadPurpose purpose) {
	// PARITY (OnClientAuthorized / mm_reload_vip): the old record is dropped before the query and
	// the per-slot feature cache is cleared.
	m_players.erase(id.account_id);
	if (SlotInRange(slot)) {
		m_slot_features[static_cast<size_t>(slot)].clear();
	}

	const int64_t now = Now();
	const uint32_t account_id = id.account_id;
	const std::string name = std::move(id.name);
	Query(std::string(kSelectPlayer), {static_cast<int64_t>(account_id), m_config.server_id},
		  [this, slot, account_id, name, purpose, now](const plgdb::Result& result) {
			  VipLoadOutcome outcome;
			  outcome.account_id = account_id;
			  if (!result.Ok()) {
				  // No onLoaded: a statement that failed is not "not a VIP", and the plugin's
				  // once-per-second reconcile retries the load after its timeout.
				  ReportError("vip: reading the vip_users row failed: " + result.Error());
				  return;
			  }
			  if (result.RowCount() > 0) {
				  outcome.row_found = true;
				  outcome.group = result.GetString(0, 0);
				  outcome.expires = result.GetInt(0, 1);
				  SetPlayer(account_id, outcome.group, outcome.expires);
				  // PARITY: the connect path and mm_reload_vip both delete a row that has expired.
				  if (outcome.expires <= now && outcome.expires != 0) {
					  outcome.expired = true;
					  outcome.row_deleted = true;
					  Query(std::string(kDeletePlayer), {static_cast<int64_t>(account_id), m_config.server_id},
							[this](const plgdb::Result& deleted) {
								if (!deleted.Ok()) {
									ReportError("vip: deleting the expired row failed: " + deleted.Error());
								}
							});
				  } else if (purpose == LoadPurpose::Connect) {
					  // PARITY: UPDATE vip_users SET name = ..., lastvisit = ... (OnClientAuthorized).
					  Query(std::string(kUpdateVisited), {name, now, static_cast<int64_t>(account_id), m_config.server_id},
							[this](const plgdb::Result& updated) {
								if (!updated.Ok()) {
									ReportError("vip: updating name/lastvisit failed: " + updated.Error());
								}
							});
				  }
			  }
			  if (onLoaded) {
				  onLoaded(slot, outcome, purpose);
			  }
		  });
}

void VipCore::DeleteExpiredRows() {
	// PARITY (OnStartupServer): DELETE FROM vip_users WHERE sid = <sid> AND expires < <now> AND expires <> 0.
	Query(std::string(kDeleteExpired), {m_config.server_id, Now()}, [this](const plgdb::Result& result) {
		if (!result.Ok()) {
			ReportError("vip: deleting the expired rows failed: " + result.Error());
		}
	});
}

void VipCore::DeleteRow(uint32_t account_id) {
	Query(std::string(kDeletePlayer), {static_cast<int64_t>(account_id), m_config.server_id},
		  [this](const plgdb::Result& result) {
			  if (!result.Ok()) {
				  ReportError("vip: deleting the row failed: " + result.Error());
			  }
		  });
}

void VipCore::InsertRow(const std::string& account_id_text, int64_t seconds, const std::string& group, int64_t now) {
	// PARITY (vip_give, offline branch): the account id is inserted exactly as it was typed and
	// the name column becomes 'none'. The original stored now + seconds when seconds != 0.
	const int64_t time_end = seconds != 0 ? now + seconds : 0;
	Query(std::string(kInsertPlayer), {account_id_text, std::string("none"), now, m_config.server_id, group, time_end},
		  [this](const plgdb::Result& result) {
			  if (!result.Ok()) {
				  ReportError("vip: inserting the offline row failed: " + result.Error());
			  }
		  });
}

} // namespace vip
