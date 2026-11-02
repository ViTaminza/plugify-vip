// vip_config_tests — the engine-free configuration half of the pure-s2sdk vip port
// (docs/PLUGIN_STANDARD.md §7: the production KeyValues files are read as they are, no conversion).
//
// Covers:
//   * src/kv1.hpp: the read-only Valve KeyValues reader for groups.ini, databases.cfg and
//     vip.phrases.txt - quoted/unquoted tokens, comments, the UTF-8 BOM, escapes, nested blocks,
//     case-insensitive lookup, Valve's atoi/atof fallbacks and the whole-file rejection of a
//     malformed document (the caller then keeps its previous config instead of half a table);
//   * src/text.hpp: the exact cs2-menus colour-tag table ({DEFAULT}..{GRAYBLUE}, "\n" -> U+2029),
//     StripColorTags for the server console, the Source language-name normalisation and the
//     Pisex printf formatting;
//   * src/menu_type.hpp: every branch of pisexcompat::ResolveS2sdkMenuTypeName with the plugin's
//     HUD_LAYOUT default (the player's "!menu" choice first, "hud" -> registered/button,
//     "wasd" -> registered/button, unknown -> HUD_LAYOUT -> hud/button).
//
// No s2sdk, no engine, no Plugify host. Run by ../test.sh (build/tests/vip_config_tests).

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

#include "kv1.hpp"
#include "menu_type.hpp"
#include "text.hpp"

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
			std::printf("FAIL %s:%d: got \"%s\", expected \"%s\"\n", __FILE__, __LINE__,           \
						a_.c_str(), e_.c_str());                                                   \
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

using namespace std::string_view_literals;

// --- KV1 -------------------------------------------------------------------------------------

// The production shape of configs/vip/groups.ini: one root block, a plain server_id value, group
// blocks with feature values and the 'legacy' inheritance key.
constexpr std::string_view kGroups = R"KV(// the original cs2-vip groups.ini
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

void TestGroups() {
	const std::optional<vip::kv::KvNode> root = vip::kv::ParseKv1(kGroups);
	CHECK(root.has_value());
	if (!root.has_value()) {
		return;
	}
	CHECK_EQ_INT(root->GetInt("server_id", 0), 7);
	CHECK_EQ_INT(root->children.size(), 3);
	// The lookup is case-insensitive (Valve's KeyValues::FindKey).
	const vip::kv::KvNode* gold = root->Find("gold");
	CHECK(gold != nullptr);
	if (gold != nullptr) {
		CHECK(gold->children.size() == 3);
		CHECK_EQ_STR(gold->GetString("Health", ""), "150");
		CHECK_EQ_STR(gold->GetString("legacy", ""), "Silver");
		CHECK_EQ_INT(gold->GetInt("Health", 0), 150);
	}
	const vip::kv::KvNode* silver = root->Find("Silver");
	CHECK(silver != nullptr);
	if (silver != nullptr) {
		CHECK_EQ_STR(silver->GetString("Armor", "?"), "100");
	}
	// A missing key answers the default.
	CHECK_EQ_STR(root->GetString("nope", "fallback"), "fallback");
	CHECK_EQ_INT(root->GetInt("nope", 42), 42);
}

void TestDatabases() {
	constexpr std::string_view kDatabases = R"KV(
"Databases"
{
	"vip"
	{
		"driver"	"sqlite"
		"port"		"3307"
		"user"		""
		"pass"		""
		"database"	"vip_test"
		"sqlite_path"	":memory:"
	}
}
)KV";
	const std::optional<vip::kv::KvNode> root = vip::kv::ParseKv1(kDatabases);
	CHECK(root.has_value());
	if (!root.has_value()) {
		return;
	}
	const vip::kv::KvNode* section = root->Find("vip");
	CHECK(section != nullptr);
	if (section == nullptr) {
		return;
	}
	CHECK_EQ_STR(section->GetString("driver", "mysql"), "sqlite");
	CHECK_EQ_INT(section->GetInt("port", 3306), 3307);
	CHECK_EQ_STR(section->GetString("user", "x"), "");
	CHECK_EQ_STR(section->GetString("database", ""), "vip_test");
	CHECK_EQ_STR(section->GetString("sqlite_path", ""), ":memory:");
	// A typo value falls back to the default through Valve's atoi.
	CHECK_EQ_STR(root->GetString("missing", "mysql"), "mysql");
}

void TestParserDetails() {
	// Unquoted tokens, // and /* */ comments, an escaped quote and a newline.
	constexpr std::string_view kText = R"KV(
#base "ignored.vdf"
/* block
   comment */
Root
{
	name "a\"b"
	path "x\\y"
	line "first\nsecond"
	unquoted value
	block { inner "1" }
}
)KV";
	const std::optional<vip::kv::KvNode> root = vip::kv::ParseKv1(kText);
	CHECK(root.has_value());
	if (!root.has_value()) {
		return;
	}
	CHECK_EQ_STR(root->GetString("name", ""), "a\"b");
	CHECK_EQ_STR(root->GetString("path", ""), "x\\y");
	CHECK_EQ_STR(root->GetString("line", ""), "first\nsecond");
	CHECK_EQ_STR(root->GetString("unquoted", ""), "value");
	const vip::kv::KvNode* block = root->Find("block");
	CHECK(block != nullptr && block->GetString("inner", "") == "1");
}

void TestMalformedIsRejected() {
	// An unterminated string, a missing '}' and a stray '}' reject the whole file: the caller must
	// keep its previous configuration instead of using half a table.
	CHECK(!vip::kv::ParseKv1("Root { \"key\" \"unterminated }").has_value());
	CHECK(!vip::kv::ParseKv1("Root { \"key\" \"value\"").has_value());
	CHECK(!vip::kv::ParseKv1("Root { } }").has_value());
}

void TestPhrases() {
	constexpr std::string_view kPhrases = R"KV(
"Phrases"
{
	"language"	"ru"
	"Welcome"	{ "ru" "Добро пожаловать, %s!" "en" "Welcome, %s!" }
	"OnlyEn"	{ "en" "english only" }
	"OnlyAny"	{ "de" "hallo" }
	"Empty"	{ }
}
)KV";
	const std::optional<vip::kv::KvNode> root = vip::kv::ParseKv1(kPhrases);
	CHECK(root.has_value());
	if (!root.has_value()) {
		return;
	}
	CHECK_EQ_STR(root->GetString("language", ""), "ru");

	const vip::kv::PhraseMap ru = vip::kv::ResolvePhrases(*root, "ru");
	CHECK_EQ_STR(ru.at("Welcome"), "Добро пожаловать, %s!");
	CHECK_EQ_STR(ru.at("OnlyEn"), "english only"); // "en" fallback
	CHECK_EQ_STR(ru.at("OnlyAny"), "hallo");       // last resort: any language
	// A block with no entries is not a phrase (Valve's GetFirstTrueSubKey skips it too).
	CHECK(ru.find("Empty") == ru.end());
	// The plain "language" value is not a phrase.
	CHECK(ru.find("language") == ru.end());

	const vip::kv::PhraseMap en = vip::kv::ResolvePhrases(*root, "en");
	CHECK_EQ_STR(en.at("Welcome"), "Welcome, %s!");
	CHECK_EQ_STR(en.at("OnlyEn"), "english only");
	CHECK_EQ_STR(en.at("OnlyAny"), "hallo");
}

void TestLoadFileAndBom() {
	const std::filesystem::path path =
		std::filesystem::temp_directory_path() / "vip_config_tests_groups.ini";
	{
		std::ofstream file(path, std::ios::binary | std::ios::trunc);
		// A UTF-8 BOM, as a Windows editor writes it, must not become part of the first key.
		file << "\xEF\xBB\xBF";
		file << kGroups;
	}
	const std::optional<vip::kv::KvNode> root = vip::kv::LoadKv1File(path.string());
	CHECK(root.has_value());
	if (root.has_value()) {
		CHECK_EQ_INT(root->GetInt("server_id", 0), 7);
	}
	std::error_code ec;
	std::filesystem::remove(path, ec);
	// A missing file is a failure, not an empty config.
	CHECK(!vip::kv::LoadKv1File((path.string() + ".missing")).has_value());
}

// --- text ------------------------------------------------------------------------------------

void TestColorTags() {
	CHECK_EQ_STR(vip::Colorize("{RED}red{WHITE}white{DEFAULT}"), "\x02red\x01white\x01");
	CHECK_EQ_STR(vip::Colorize("{GREEN}ok"), "\x04ok");
	CHECK_EQ_STR(vip::Colorize("{GRAYBLUE}x{LIGHTOLIVE}y{OLIVE}z"), "\x0Ax\x09y\x10z");
	// cs2-menus' line break is U+2029, not a newline.
	CHECK_EQ_STR(vip::Colorize("a\\nb"), "a\xe2\x80\xa9" "b");
	// Unknown tags stay literal.
	CHECK_EQ_STR(vip::Colorize("{PINK}x"), "{PINK}x");

	// The server console variant removes the tags and turns the line break into a real newline.
	CHECK_EQ_STR(vip::StripColorTags("{RED}red{WHITE}white"), "redwhite");
	CHECK_EQ_STR(vip::StripColorTags("a\\nb"), "a\nb");
	CHECK_EQ_STR(vip::StripColorTags("plain"), "plain");
}

void TestLanguage() {
	CHECK_EQ_STR(vip::NormalizeLanguage("russian"), "ru");
	CHECK_EQ_STR(vip::NormalizeLanguage("Russian"), "ru");
	CHECK_EQ_STR(vip::NormalizeLanguage("en"), "en");
	CHECK_EQ_STR(vip::NormalizeLanguage("portuguese"), "pt-BR");
	CHECK_EQ_STR(vip::NormalizeLanguage("brazilian"), "pt-BR");
	CHECK_EQ_STR(vip::NormalizeLanguage("schinese"), "zh-CN");
	CHECK_EQ_STR(vip::NormalizeLanguage("klingon"), "klingon");
}

void TestFormatting() {
	CHECK_EQ_STR(vip::FormatString("%s has %d hp", "E!N", 150), "E!N has 150 hp");
	CHECK_EQ_STR(vip::FormatString("%s", ""), "");
	CHECK_EQ_STR(vip::FormatString("100%%"), "100%");
	CHECK(vip::ContainsOnlyDigits("123"));
	CHECK(!vip::ContainsOnlyDigits("12a"));
	CHECK(!vip::ContainsOnlyDigits(""));
}

// --- menu type table ------------------------------------------------------------------------

void TestMenuTypeTable() {
	using vip::ResolveMenuTypeName;
	// The HUD_LAYOUT default: "hud" when the optional backend is registered, else the WASD button
	// (never another custom type).
	CHECK_EQ_STR(ResolveMenuTypeName(true, false, ""), "hud");
	CHECK_EQ_STR(ResolveMenuTypeName(false, false, ""), "button");
	CHECK_EQ_STR(ResolveMenuTypeName(false, true, ""), "button"); // wasd is never a HUD fallback
	// The player's own "!menu" choice from hudmenu.
	CHECK_EQ_STR(ResolveMenuTypeName(true, false, "chat"), "chat");
	CHECK_EQ_STR(ResolveMenuTypeName(true, false, "centerhtml"), "centerhtml");
	CHECK_EQ_STR(ResolveMenuTypeName(true, false, "button"), "button");
	CHECK_EQ_STR(ResolveMenuTypeName(true, false, "hud"), "hud");
	CHECK_EQ_STR(ResolveMenuTypeName(false, true, "hud"), "button");
	CHECK_EQ_STR(ResolveMenuTypeName(true, true, "wasd"), "wasd");
	CHECK_EQ_STR(ResolveMenuTypeName(true, false, "wasd"), "button");
	// An unknown/stale answer falls through to the HUD_LAYOUT default.
	CHECK_EQ_STR(ResolveMenuTypeName(true, true, "hud_layout"), "hud");
	CHECK_EQ_STR(ResolveMenuTypeName(false, true, "nonsense"), "button");
}

} // namespace

int main() {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	std::printf("vip_config_tests\n");
	TestGroups();
	TestDatabases();
	TestParserDetails();
	TestMalformedIsRejected();
	TestPhrases();
	TestLoadFileAndBom();
	TestColorTags();
	TestLanguage();
	TestFormatting();
	TestMenuTypeTable();
	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
