// Tests for plugins/vip/src/vip_core.hpp (engine-independent core of the vip plugin: group
// table, the vip_users table, the VIPApi state rules, the feature-value/cookie rules and the
// expiry semantics).
//
// Runs against SQLite ":memory:" always, and against MySQL when MYSQL_HOST is set (test.sh
// provides MYSQL_HOST/MYSQL_PORT/MYSQL_USER/MYSQL_PASSWORD/MYSQL_DATABASE).
//
// Every run uses fresh account ids, so repeated runs against the shared MySQL database do not
// interfere. The vip_users table is created with IF NOT EXISTS (the same statement the plugin
// runs) and left in place.
//
// Fixtures call CreateSchema()/LoadPlayer()/... and then Wait() -> Poll() until the hooks of the
// core have run, which is exactly how the plugin drives them from OnPluginUpdate.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <unistd.h>

#include <plgdb/db.hpp>

#include "vip_core.hpp"

namespace {

int g_checks = 0;
int g_failures = 0;
std::string_view g_section = "?";

void Section(std::string_view name) {
	g_section = name;
	std::printf("-- %.*s\n", static_cast<int>(name.size()), name.data());
}

#define CHECK(expr)                                                                                \
	do {                                                                                           \
		++g_checks;                                                                                \
		if (!(expr)) {                                                                             \
			++g_failures;                                                                          \
			std::printf("FAIL [%.*s] %s:%d: %s\n", static_cast<int>(g_section.size()),              \
						g_section.data(), __FILE__, __LINE__, #expr);                              \
		}                                                                                          \
	} while (false)

#define CHECK_EQ_STR(actual, expected)                                                             \
	do {                                                                                           \
		++g_checks;                                                                                \
		const std::string a_{actual};                                                              \
		const std::string e_{expected};                                                            \
		if (a_ != e_) {                                                                            \
			++g_failures;                                                                          \
			std::printf("FAIL [%.*s] %s:%d: got \"%s\", expected \"%s\"\n",                        \
						static_cast<int>(g_section.size()), g_section.data(), __FILE__,           \
						__LINE__, a_.c_str(), e_.c_str());                                         \
		}                                                                                          \
	} while (false)

#define CHECK_EQ_INT(actual, expected)                                                             \
	do {                                                                                           \
		++g_checks;                                                                                \
		const long long a_ = static_cast<long long>(actual);                                       \
		const long long e_ = static_cast<long long>(expected);                                     \
		if (a_ != e_) {                                                                            \
			++g_failures;                                                                          \
			std::printf("FAIL [%.*s] %s:%d: got %lld, expected %lld\n",                            \
						static_cast<int>(g_section.size()), g_section.data(), __FILE__,           \
						__LINE__, a_, e_);                                                         \
		}                                                                                          \
	} while (false)

#define CHECK_QUERY_OK(result)                                                                     \
	do {                                                                                           \
		++g_checks;                                                                                \
		const plgdb::Result& r_ = (result);                                                        \
		if (!r_.Ok()) {                                                                            \
			++g_failures;                                                                          \
			std::printf("FAIL [%.*s] %s:%d: query failed: %s\n",                                   \
						static_cast<int>(g_section.size()), g_section.data(), __FILE__,           \
						__LINE__, r_.Error().c_str());                                             \
		}                                                                                          \
	} while (false)

using vip::Config;
using vip::GroupFeatures;
using vip::GroupTable;
using vip::LoadPurpose;
using vip::VipIdentity;
using vip::VipLoadOutcome;

constexpr int64_t kNow = 2000000000; // a fixed clock so the expiry checks are deterministic

// --- helpers ---------------------------------------------------------------------------

bool Wait(plgdb::Database& db, const std::function<bool()>& done, int timeoutMs = 10000) {
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
	for (;;) {
		db.Poll();
		if (done()) {
			return true;
		}
		if (std::chrono::steady_clock::now() >= deadline) {
			db.Poll();
			return done();
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
}

// Lets every queued statement run (writes are only visible to the hooks once Poll() ran).
void Drain(plgdb::Database& db) {
	Wait(db, [] { return false; }, 80);
}

long long ScalarInt(plgdb::Database& db, std::string_view sql, plgdb::Params params = {}) {
	const plgdb::Result result = db.QuerySync(std::string(sql), std::move(params));
	if (!result.Ok() || result.RowCount() != 1) {
		return -1;
	}
	return result.GetInt(0, 0, -1);
}

std::string ScalarString(plgdb::Database& db, std::string_view sql, plgdb::Params params = {}) {
	const plgdb::Result result = db.QuerySync(std::string(sql), std::move(params));
	if (!result.Ok() || result.RowCount() != 1) {
		return "<no row>";
	}
	return result.GetString(0, 0);
}

void Exec(plgdb::Database& db, std::string sql, plgdb::Params params = {}) {
	CHECK_QUERY_OK(db.QuerySync(std::move(sql), std::move(params)));
}

uint32_t MakeRunBase() {
	const auto now = static_cast<uint64_t>(std::chrono::system_clock::now().time_since_epoch().count());
	std::mt19937_64 rng(now ^ (static_cast<uint64_t>(::getpid()) << 32));
	// The account ids of the test run; the column is a 32 bit integer.
	return static_cast<uint32_t>(100000000ULL + (rng() % 100000000ULL));
}

// The groups of the shipped groups.ini, trimmed to what the tests need.
GroupTable MakeGroups() {
	GroupTable groups;
	groups["SKILL"]["money"] = "++4000";
	groups["SKILL"]["speed"] = "1.03";
	groups["PROVIP"]["armor"] = "100";
	groups["PROVIP"]["Vampirism"] = "15";
	groups["PROVIPRED"]["legacy"] = "PROVIP";
	groups["PROVIPRED"]["smoke_color"] = "242 9 9";
	groups["CHILD"]["legacy"] = "PROVIPRED";
	groups["CHILD"]["Vampirism"] = "99"; // a child value wins over the parent
	return groups;
}

struct Fixture {
	explicit Fixture(uint32_t base) : run_base(base) {}

	std::unique_ptr<plgdb::Database> db;
	std::unique_ptr<vip::VipCore> core;
	std::vector<std::string> errors;
	std::vector<VipLoadOutcome> loads;
	std::vector<LoadPurpose> load_purposes;
	uint32_t run_base = 0;
	int64_t now = kNow;

	uint32_t Account(int index) const { return run_base + static_cast<uint32_t>(index); }

	VipIdentity Identity(int index, uint32_t account = 0, int slot = 0) {
		VipIdentity identity;
		identity.slot = slot;
		identity.account_id = account != 0 ? account : Account(index);
		identity.steamid64 = 76561197960265728ULL + identity.account_id;
		identity.name = "Player" + std::to_string(index);
		return identity;
	}

	void Open(plgdb::Config config) {
		db = plgdb::Database::Create(config);
		if (db == nullptr) {
			throw std::runtime_error("cannot create the database");
		}
		core = std::make_unique<vip::VipCore>(*db);
		Config core_config;
		core_config.server_id = 1;
		core->SetConfig(core_config);
		core->SetGroups(MakeGroups());
		core->SetClock([this] { return now; });
		core->onError = [this](std::string_view message) { errors.emplace_back(message); };
		core->onLoaded = [this](int slot, const VipLoadOutcome& outcome, LoadPurpose purpose) {
			(void)slot;
			loads.push_back(outcome);
			load_purposes.push_back(purpose);
		};
		core->CreateSchema();
	}

	// Deletes every row of the run (SQLite ":memory:" is thrown away, MySQL is shared).
	void Clean() {
		if (db == nullptr) {
			return;
		}
		for (int index = 0; index < 32; ++index) {
			Exec(*db, "DELETE FROM vip_users WHERE account_id = ?", {static_cast<int64_t>(Account(index))});
		}
	}
};

// --- suite -----------------------------------------------------------------------------

void TestSchema(Fixture& f) {
	Section("schema");
	// The table exists and the (account_id, sid) pair is the primary key.
	Exec(*f.db, "INSERT INTO vip_users (account_id, name, lastvisit, sid, `group`, expires) VALUES (?, ?, ?, ?, ?, ?)",
		 {static_cast<int64_t>(f.Account(0)), std::string("dup"), static_cast<int64_t>(f.now), static_cast<int64_t>(1),
		  std::string("SKILL"), static_cast<int64_t>(0)});
	const plgdb::Result duplicate =
		f.db->QuerySync("INSERT INTO vip_users (account_id, name, lastvisit, sid, `group`, expires) VALUES (?, ?, ?, ?, ?, ?)",
						{static_cast<int64_t>(f.Account(0)), std::string("dup2"), static_cast<int64_t>(f.now),
						 static_cast<int64_t>(1), std::string("SKILL"), static_cast<int64_t>(0)});
	CHECK(!duplicate.Ok()); // the composite primary key rejects the second row
	Exec(*f.db, "DELETE FROM vip_users WHERE account_id = ?", {static_cast<int64_t>(f.Account(0))});
}

void TestGiveAndRemove(Fixture& f) {
	Section("give / remove");
	const VipIdentity id = f.Identity(1);
	CHECK(f.core->GiveVip(id, 3600, "PROVIP", true, f.now));
	CHECK(f.core->FindPlayer(id.account_id) != nullptr);
	CHECK(f.core->IsClientVip(id, f.now));
	CHECK_EQ_INT(f.core->AccessTime(id, f.now), f.now + 3600);
	CHECK_EQ_STR(f.core->GroupOf(id), "PROVIP");
	CHECK_EQ_INT(f.core->IsValidGroup("PROVIP"), 1);
	CHECK_EQ_INT(f.core->IsValidGroup("nope"), 0);

	// A second give fails like the original (the record exists), DB or not.
	CHECK(!f.core->GiveVip(id, 60, "SKILL", false, f.now));
	CHECK_EQ_STR(f.core->GroupOf(id), "PROVIP");

	Drain(*f.db);
	CHECK_EQ_INT(ScalarInt(*f.db, "SELECT expires FROM vip_users WHERE account_id = ?",
						   {static_cast<int64_t>(id.account_id)}),
				 f.now + 3600);
	CHECK_EQ_STR(ScalarString(*f.db, "SELECT `group` FROM vip_users WHERE account_id = ?",
							  {static_cast<int64_t>(id.account_id)}),
				 "PROVIP");
	CHECK_EQ_STR(ScalarString(*f.db, "SELECT name FROM vip_users WHERE account_id = ?",
							  {static_cast<int64_t>(id.account_id)}),
				 "Player1");
	CHECK_EQ_INT(ScalarInt(*f.db, "SELECT sid FROM vip_users WHERE account_id = ?",
						   {static_cast<int64_t>(id.account_id)}),
				 1);

	CHECK(f.core->RemoveVip(id, true));
	CHECK(f.core->FindPlayer(id.account_id) == nullptr);
	CHECK(!f.core->RemoveVip(id, true)); // nothing left
	CHECK(!f.core->IsClientVip(id, f.now));
	CHECK_EQ_INT(f.core->AccessTime(id, f.now), -1);
	Drain(*f.db);
	CHECK_EQ_INT(ScalarInt(*f.db, "SELECT COUNT(*) FROM vip_users WHERE account_id = ?",
						   {static_cast<int64_t>(id.account_id)}),
				 0);
}

void TestSetAccessTimeAndGroup(Fixture& f) {
	Section("set access time / group");
	const VipIdentity id = f.Identity(2);
	CHECK(!f.core->SetAccessTime(id, 12345, true)); // no record
	CHECK(!f.core->SetGroup(id, "SKILL", true, f.now));

	CHECK(f.core->GiveVip(id, 0, "SKILL", true, f.now)); // permanent
	CHECK_EQ_INT(f.core->AccessTime(id, f.now), 0);
	CHECK(f.core->SetAccessTime(id, f.now + 100, true));
	CHECK_EQ_INT(f.core->AccessTime(id, f.now), f.now + 100);
	Drain(*f.db);
	CHECK_EQ_INT(ScalarInt(*f.db, "SELECT expires FROM vip_users WHERE account_id = ?",
						   {static_cast<int64_t>(id.account_id)}),
				 f.now + 100);

	// SetGroup validates the group and the expiry.
	CHECK(!f.core->SetGroup(id, "nope", true, f.now));
	CHECK(f.core->SetGroup(id, "PROVIPRED", true, f.now));
	CHECK_EQ_STR(f.core->GroupOf(id), "PROVIPRED");
	Drain(*f.db);
	CHECK_EQ_STR(ScalarString(*f.db, "SELECT `group` FROM vip_users WHERE account_id = ?",
							  {static_cast<int64_t>(id.account_id)}),
				 "PROVIPRED");

	// An expired record cannot change its group, but it still exists (so SetAccessTime works).
	f.core->SetAccessTime(id, f.now - 10, false);
	CHECK(!f.core->SetGroup(id, "SKILL", false, f.now));
	CHECK(!f.core->IsClientVip(id, f.now)); // expired for the API
	CHECK(f.core->SetAccessTime(id, f.now + 50, true));
	CHECK_EQ_INT(f.core->AccessTime(id, f.now), f.now + 50);
	CHECK(f.core->RemoveVip(id, true));
}

void TestLoadPlayer(Fixture& f) {
	Section("load player");
	// A row that does not exist: row_found false, and the callback still fires.
	f.loads.clear();
	f.load_purposes.clear();
	f.core->LoadPlayer(5, f.Identity(3, 0, 5), LoadPurpose::Connect);
	CHECK(Wait(*f.db, [&f] { return !f.loads.empty(); }));
	CHECK_EQ_INT(f.loads.size(), 1);
	CHECK(!f.loads[0].row_found);
	CHECK(f.loads[0].group.empty());
	CHECK_EQ_INT(f.load_purposes[0] == LoadPurpose::Connect, 1);
	CHECK(f.core->FindPlayer(f.Account(3)) == nullptr);

	// A valid row: the record is set, name/lastvisit are refreshed, no deletion.
	Exec(*f.db, "INSERT INTO vip_users (account_id, name, lastvisit, sid, `group`, expires) VALUES (?, ?, ?, ?, ?, ?)",
		 {static_cast<int64_t>(f.Account(4)), std::string("old"), static_cast<int64_t>(0), static_cast<int64_t>(1),
		  std::string("PROVIP"), static_cast<int64_t>(0)});
	f.loads.clear();
	f.core->LoadPlayer(6, f.Identity(4, 0, 6), LoadPurpose::Connect);
	CHECK(Wait(*f.db, [&f] { return !f.loads.empty(); }));
	CHECK(f.loads[0].row_found);
	CHECK(!f.loads[0].expired);
	CHECK_EQ_STR(f.loads[0].group, "PROVIP");
	CHECK_EQ_INT(f.loads[0].expires, 0);
	CHECK(f.core->IsClientVip(f.Identity(4, 0, 6), f.now));
	Drain(*f.db);
	CHECK_EQ_STR(ScalarString(*f.db, "SELECT name FROM vip_users WHERE account_id = ?",
							  {static_cast<int64_t>(f.Account(4))}),
				 "Player4");

	// An expired row is deleted (the DELETE of the original).
	Exec(*f.db, "INSERT INTO vip_users (account_id, name, lastvisit, sid, `group`, expires) VALUES (?, ?, ?, ?, ?, ?)",
		 {static_cast<int64_t>(f.Account(5)), std::string("dead"), static_cast<int64_t>(0), static_cast<int64_t>(1),
		  std::string("SKILL"), static_cast<int64_t>(f.now - 1)});
	f.loads.clear();
	f.core->LoadPlayer(7, f.Identity(5, 0, 7), LoadPurpose::Connect);
	CHECK(Wait(*f.db, [&f] { return !f.loads.empty(); }));
	CHECK(f.loads[0].row_found);
	CHECK(f.loads[0].expired);
	CHECK(f.loads[0].row_deleted);
	CHECK(!f.core->IsClientVip(f.Identity(5, 0, 7), f.now));
	CHECK(f.core->FindPlayer(f.Account(5)) != nullptr); // the record is kept, like the original
	Drain(*f.db);
	CHECK_EQ_INT(ScalarInt(*f.db, "SELECT COUNT(*) FROM vip_users WHERE account_id = ?",
						   {static_cast<int64_t>(f.Account(5))}),
				 0);

	// The reload purpose reads the row without the name/lastvisit UPDATE.
	Exec(*f.db, "INSERT INTO vip_users (account_id, name, lastvisit, sid, `group`, expires) VALUES (?, ?, ?, ?, ?, ?)",
		 {static_cast<int64_t>(f.Account(6)), std::string("keep"), static_cast<int64_t>(5), static_cast<int64_t>(1),
		  std::string("SKILL"), static_cast<int64_t>(0)});
	f.loads.clear();
	f.load_purposes.clear();
	f.core->LoadPlayer(8, f.Identity(6, 0, 8), LoadPurpose::Reload);
	CHECK(Wait(*f.db, [&f] { return !f.loads.empty(); }));
	CHECK_EQ_INT(f.loads.size(), 1);
	CHECK_EQ_INT(f.load_purposes[0] == LoadPurpose::Reload, 1);
	CHECK(f.core->IsClientVip(f.Identity(6, 0, 8), f.now));
	Drain(*f.db);
	CHECK_EQ_INT(ScalarInt(*f.db, "SELECT lastvisit FROM vip_users WHERE account_id = ?",
						   {static_cast<int64_t>(f.Account(6))}),
				 5); // untouched

	// Another server's row (sid) is invisible.
	Exec(*f.db, "INSERT INTO vip_users (account_id, name, lastvisit, sid, `group`, expires) VALUES (?, ?, ?, ?, ?, ?)",
		 {static_cast<int64_t>(f.Account(7)), std::string("other"), static_cast<int64_t>(0), static_cast<int64_t>(2),
		  std::string("SKILL"), static_cast<int64_t>(0)});
	f.loads.clear();
	f.core->LoadPlayer(9, f.Identity(7, 0, 9), LoadPurpose::Connect);
	CHECK(Wait(*f.db, [&f] { return !f.loads.empty(); }));
	CHECK(!f.loads[0].row_found);
	CHECK(!f.core->IsClientVip(f.Identity(7, 0, 9), f.now));
}

void TestExpiry(Fixture& f) {
	Section("expiry");
	f.now = kNow;
	const VipIdentity id = f.Identity(8, 0, 3);
	CHECK(f.core->GiveVip(id, 100, "SKILL", false, f.now));

	// The API treats TimeEnd == now as expired ("<="), the tick only at "<" - both verbatim.
	f.now = kNow + 100;
	CHECK(!f.core->IsClientVip(id, f.now));
	CHECK_EQ_INT(f.core->AccessTime(id, f.now), -1);
	CHECK(!f.core->ExpirePlayer(id.account_id, f.now));
	// ... and one second later the tick drops the record.
	f.now = kNow + 101;
	CHECK(f.core->ExpirePlayer(id.account_id, f.now));
	CHECK(f.core->FindPlayer(id.account_id) == nullptr);
	CHECK(!f.core->ExpirePlayer(id.account_id, f.now)); // nothing left

	// A permanent record (0) never expires.
	const VipIdentity permanent = f.Identity(9, 0, 4);
	CHECK(f.core->GiveVip(permanent, 0, "SKILL", false, f.now));
	f.now = kNow + 1000000;
	CHECK(!f.core->ExpirePlayer(permanent.account_id, f.now));
	CHECK(f.core->IsClientVip(permanent, f.now));
}

void TestDeleteExpiredRowsAndInsert(Fixture& f) {
	Section("startup delete / offline insert");
	f.now = kNow;
	Exec(*f.db, "INSERT INTO vip_users (account_id, name, lastvisit, sid, `group`, expires) VALUES (?, ?, ?, ?, ?, ?)",
		 {static_cast<int64_t>(f.Account(10)), std::string("expired"), static_cast<int64_t>(0), static_cast<int64_t>(1),
		  std::string("SKILL"), static_cast<int64_t>(f.now - 1)});
	Exec(*f.db, "INSERT INTO vip_users (account_id, name, lastvisit, sid, `group`, expires) VALUES (?, ?, ?, ?, ?, ?)",
		 {static_cast<int64_t>(f.Account(11)), std::string("permanent"), static_cast<int64_t>(0), static_cast<int64_t>(1),
		  std::string("SKILL"), static_cast<int64_t>(0)});
	Exec(*f.db, "INSERT INTO vip_users (account_id, name, lastvisit, sid, `group`, expires) VALUES (?, ?, ?, ?, ?, ?)",
		 {static_cast<int64_t>(f.Account(12)), std::string("othersid"), static_cast<int64_t>(0), static_cast<int64_t>(2),
		  std::string("SKILL"), static_cast<int64_t>(f.now - 1)});
	f.core->DeleteExpiredRows();
	Drain(*f.db);
	CHECK_EQ_INT(ScalarInt(*f.db, "SELECT COUNT(*) FROM vip_users WHERE account_id = ?",
						   {static_cast<int64_t>(f.Account(10))}),
				 0);
	CHECK_EQ_INT(ScalarInt(*f.db, "SELECT COUNT(*) FROM vip_users WHERE account_id = ?",
						   {static_cast<int64_t>(f.Account(11))}),
				 1);
	CHECK_EQ_INT(ScalarInt(*f.db, "SELECT COUNT(*) FROM vip_users WHERE account_id = ?",
						   {static_cast<int64_t>(f.Account(12))}),
				 1); // another server's rows are untouched

	// The vip_give offline branch: the account id is a string, the name becomes 'none'.
	f.core->InsertRow(std::to_string(f.Account(13)), 60, "PROVIP", f.now);
	Drain(*f.db);
	CHECK_EQ_STR(ScalarString(*f.db, "SELECT name FROM vip_users WHERE account_id = ?",
							  {static_cast<int64_t>(f.Account(13))}),
				 "none");
	CHECK_EQ_INT(ScalarInt(*f.db, "SELECT expires FROM vip_users WHERE account_id = ?",
						   {static_cast<int64_t>(f.Account(13))}),
				 f.now + 60);
	CHECK_EQ_STR(ScalarString(*f.db, "SELECT `group` FROM vip_users WHERE account_id = ?",
							  {static_cast<int64_t>(f.Account(13))}),
				 "PROVIP");
}

void TestFeatureValues(Fixture& f) {
	Section("feature values");
	(void)f;
	GroupFeatures group;
	group["jumps"] = "2";
	group["speed"] = "1.5";
	group["bhop"] = "1";
	group["FOV"] = "100,90,80,70,60";
	group["empty"] = "";

	// An empty cookie (no toggle written yet) means enabled; "0" means switched off and answers
	// the neutral value; any other number means enabled (atoi != 0).
	CHECK_EQ_INT(vip::features::GetInt(group, "jumps", ""), 2);
	CHECK_EQ_INT(vip::features::GetInt(group, "jumps", "1"), 2);
	CHECK_EQ_INT(vip::features::GetInt(group, "jumps", "2"), 2);
	CHECK_EQ_INT(vip::features::GetInt(group, "jumps", "0"), -1);
	CHECK_EQ_INT(vip::features::GetInt(group, "jumps", "abc"), -1); // atoi("abc") == 0
	CHECK_EQ_INT(vip::features::GetInt(group, "missing", ""), -1);
	CHECK_EQ_INT(vip::features::GetInt(group, "empty", ""), -1);

	CHECK_EQ_INT(vip::features::GetBool(group, "bhop", ""), 1);
	CHECK_EQ_INT(vip::features::GetBool(group, "bhop", "0"), 0);
	CHECK_EQ_INT(vip::features::GetBool(group, "jumps", ""), 1); // atoi("2") != 0
	CHECK_EQ_INT(vip::features::GetBool(group, "missing", ""), 0);

	CHECK(vip::features::GetFloat(group, "speed", "") == 1.5f);
	CHECK(vip::features::GetFloat(group, "speed", "0") == 1.0f);
	CHECK(vip::features::GetFloat(group, "missing", "") == 1.0f);

	CHECK_EQ_STR(vip::features::GetString(group, "FOV", ""), "100,90,80,70,60");
	CHECK_EQ_STR(vip::features::GetString(group, "FOV", "0"), "");
	CHECK_EQ_STR(vip::features::GetString(group, "missing", ""), "");
}

void TestResolveFeatures(Fixture& f) {
	Section("group resolution (legacy chain)");
	const VipIdentity id = f.Identity(20, 0, 10);
	CHECK(f.core->GiveVip(id, 0, "PROVIPRED", false, f.now));

	const GroupFeatures* resolved = f.core->ResolveFeatures(10, id.account_id, f.now);
	CHECK(resolved != nullptr);
	if (resolved != nullptr) {
		CHECK_EQ_STR(resolved->at("armor"), "100");         // inherited from PROVIP
		CHECK_EQ_STR(resolved->at("smoke_color"), "242 9 9"); // the child's own value wins
		CHECK_EQ_STR(resolved->at("Vampirism"), "15");
		CHECK(resolved->find("legacy") != resolved->end()); // 'legacy' stays in the table (the menu skips it)
	}

	// The cache is per slot: a second call returns the same object.
	CHECK(f.core->ResolveFeatures(10, id.account_id, f.now) == resolved);
	f.core->ClearSlotFeatures(10);
	CHECK(f.core->ResolveFeatures(10, id.account_id, f.now) != nullptr);

	// A two-level chain: CHILD -> PROVIPRED -> PROVIP, with the child's own value kept.
	const VipIdentity child = f.Identity(21, 0, 11);
	CHECK(f.core->GiveVip(child, 0, "CHILD", false, f.now));
	const GroupFeatures* deep = f.core->ResolveFeatures(11, child.account_id, f.now);
	CHECK(deep != nullptr);
	if (deep != nullptr) {
		CHECK_EQ_STR(deep->at("Vampirism"), "99");
		CHECK_EQ_STR(deep->at("armor"), "100");
		CHECK_EQ_STR(deep->at("smoke_color"), "242 9 9");
	}

	// A group that is unknown to groups.ini has no feature table (the original returned an empty
	// map and answered the neutral values).
	const VipIdentity unknown = f.Identity(22, 0, 12);
	CHECK(f.core->GiveVip(unknown, 0, "GHOST", false, f.now));
	CHECK(!f.core->IsClientVip(unknown, f.now));
	CHECK(f.core->ResolveFeatures(12, unknown.account_id, f.now) == nullptr);

	// An expired record has no feature table.
	f.now = kNow;
	const VipIdentity timed = f.Identity(23, 0, 13);
	CHECK(f.core->GiveVip(timed, 10, "SKILL", false, f.now));
	f.now = kNow + 11;
	CHECK(f.core->ResolveFeatures(13, timed.account_id, f.now) == nullptr);

	// Replacing the group table drops the per-slot caches (LoadVips did the same): with an empty
	// table the resolution is recomputed from scratch and finds nothing.
	f.now = kNow;
	CHECK(f.core->ResolveFeatures(10, id.account_id, f.now) != nullptr);
	f.core->SetGroups(GroupTable{});
	CHECK(f.core->ResolveFeatures(10, id.account_id, f.now) == nullptr);
	f.core->SetGroups(MakeGroups()); // restore for the next test
	CHECK(f.core->ResolveFeatures(10, id.account_id, f.now) != nullptr);
}

void TestLegacyCycle(Fixture& f) {
	Section("legacy cycle");
	GroupTable groups = MakeGroups();
	groups["A"]["legacy"] = "B";
	groups["A"]["value"] = "1";
	groups["B"]["legacy"] = "A";
	groups["B"]["other"] = "2";
	f.core->SetGroups(std::move(groups));
	f.now = kNow;

	const VipIdentity id = f.Identity(24, 0, 14);
	CHECK(f.core->GiveVip(id, 0, "A", false, f.now));
	const GroupFeatures* resolved = f.core->ResolveFeatures(14, id.account_id, f.now);
	CHECK(resolved != nullptr); // the walk stopped instead of hanging
	if (resolved != nullptr) {
		CHECK_EQ_STR(resolved->at("value"), "1");
		CHECK_EQ_STR(resolved->at("other"), "2");
	}
	// The guard reported the cycle once (expected); forget it so the suite's final "no errors"
	// check stays meaningful for the other tests.
	bool saw_cycle = false;
	for (const std::string& error : f.errors) {
		if (error.find("legacy") != std::string::npos) {
			saw_cycle = true;
		}
	}
	CHECK(saw_cycle);
	f.errors.clear();
	f.core->SetGroups(MakeGroups()); // restore
}

// --- main -----------------------------------------------------------------------------

void RunSuite(const plgdb::Config& config, const char* name) {
	std::printf("== %s\n", name);
	try {
		Fixture f(MakeRunBase());
		f.Open(config);
		TestSchema(f);
		TestGiveAndRemove(f);
		TestSetAccessTimeAndGroup(f);
		TestLoadPlayer(f);
		TestExpiry(f);
		TestDeleteExpiredRowsAndInsert(f);
		TestFeatureValues(f);
		TestResolveFeatures(f);
		TestLegacyCycle(f);
		f.Clean();
		if (!f.errors.empty()) {
			std::printf("FAIL [%.*s] core reported %zu errors, first: %s\n", static_cast<int>(g_section.size()),
						g_section.data(), f.errors.size(), f.errors.front().c_str());
			++g_failures;
		}
	} catch (const std::exception& ex) {
		std::printf("FATAL [%s]: %s\n", name, ex.what());
		++g_failures;
	}
}

plgdb::Config SqliteConfig() {
	plgdb::Config config;
	config.driver = plgdb::Driver::SQLite;
	config.sqlitePath = ":memory:";
	return config;
}

bool MySqlConfig(plgdb::Config& config, std::string& error) {
	const char* host = std::getenv("MYSQL_HOST");
	if (!host || !*host) {
		return false;
	}
	config.driver = plgdb::Driver::MySQL;
	config.host = host;
	config.user = std::getenv("MYSQL_USER") ? std::getenv("MYSQL_USER") : "";
	config.password = std::getenv("MYSQL_PASSWORD") ? std::getenv("MYSQL_PASSWORD") : "";
	config.database = std::getenv("MYSQL_DATABASE") ? std::getenv("MYSQL_DATABASE") : "";
	const char* port = std::getenv("MYSQL_PORT");
	if (port && *port) {
		try {
			const int value = std::stoi(port);
			if (value < 1 || value > 65535) {
				error = "MYSQL_PORT out of range";
				return false;
			}
			config.port = static_cast<uint16_t>(value);
		} catch (const std::exception&) {
			error = "MYSQL_PORT is not a number";
			return false;
		}
	}
	return true;
}

} // namespace

int main() {
	std::setvbuf(stdout, nullptr, _IONBF, 0); // a crash must not swallow the section it happened in
	std::printf("vip_core_tests\n");

	RunSuite(SqliteConfig(), "sqlite (:memory:)");

	plgdb::Config mysql;
	std::string error;
	if (MySqlConfig(mysql, error)) {
		std::printf("== mysql: %s:%u/%s\n", mysql.host.c_str(), mysql.port, mysql.database.c_str());
		RunSuite(mysql, "mysql");
	} else if (!error.empty()) {
		std::printf("FATAL: %s\n", error.c_str());
		++g_failures;
	} else {
		std::printf("== mysql: skipped (MYSQL_HOST not set)\n");
	}

	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
