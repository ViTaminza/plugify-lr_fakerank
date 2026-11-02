// Tests for plugins/lr_fakerank/src/fakerank_core.{hpp,cpp} (engine-free core of the ported
// "[LR] FakeRank" module).
//
//   * the shipped fakerank.ini through the plugin's own KV1 reader, including the Type mapping, the
//     search order (<configs>/levels_ranks first, <configs>/lr_fakerank fallback) and the reload
//     semantics (the type survives a file without a known Type),
//   * the reader itself: comments, quotes, escapes, #base lines, a UTF-8 BOM, Valve's atoi integer
//     parsing and the rejection of malformed input,
//   * the desired controller values of the three switch branches,
//   * the map-gated schema offset cache (gate first, successes memoised, misses retried),
//   * the Tab/reveal state machine: the rising edge, the three mm_fakerank_reveal modes, the
//     per-map reset and the disconnect branch.
//
// No engine, no Plugify host, no pisexcompat: only the plugin's own core is compiled in.

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "fakerank_core.hpp"

namespace {

int g_checks = 0;
int g_failures = 0;
std::string_view g_section = "?";

void Section(std::string_view name) {
	g_section = name;
	std::printf("-- %.*s\n", static_cast<int>(name.size()), name.data());
}

#define CHECK(expr)                                                                            \
	do {                                                                                       \
		++g_checks;                                                                            \
		if (!(expr)) {                                                                         \
			++g_failures;                                                                      \
			std::printf("FAIL [%.*s] %s:%d: %s\n", static_cast<int>(g_section.size()),         \
						g_section.data(), __FILE__, __LINE__, #expr);                          \
		}                                                                                      \
	} while (false)

#define CHECK_EQ_INT(actual, expected)                                                         \
	do {                                                                                       \
		++g_checks;                                                                            \
		const long long a_ = static_cast<long long>(actual);                                   \
		const long long e_ = static_cast<long long>(expected);                                 \
		if (a_ != e_) {                                                                        \
			++g_failures;                                                                      \
			std::printf("FAIL [%.*s] %s:%d: got %lld, expected %lld\n",                        \
						static_cast<int>(g_section.size()), g_section.data(), __FILE__,       \
						__LINE__, a_, e_);                                                     \
		}                                                                                      \
	} while (false)

#define CHECK_EQ_STR(actual, expected)                                                         \
	do {                                                                                       \
		++g_checks;                                                                            \
		const std::string a_((actual));                                                        \
		const std::string e_((expected));                                                      \
		if (a_ != e_) {                                                                        \
			++g_failures;                                                                      \
			std::printf("FAIL [%.*s] %s:%d: got \"%s\", expected \"%s\"\n",                    \
						static_cast<int>(g_section.size()), g_section.data(), __FILE__,       \
						__LINE__, a_.c_str(), e_.c_str());                                     \
		}                                                                                      \
	} while (false)

using namespace lr_fakerank;

bool EndsWith(std::string_view text, std::string_view suffix) {
	return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}

// --- the KV1 reader -------------------------------------------------------------------------------

void TestKvReader() {
	Section("KV1 reader");
	// Quoted and unquoted tokens, nested blocks, a key carrying both a value and a block, and
	// case-insensitive lookups (Valve's FindKey).
	{
		const std::optional<KvNode> root = ParseKv1("Root\n{\n"
													"  \"quoted\" \"a value\"\n"
													"  unquoted 42\n"
													"  \"both\" \"v\" { \"inner\" \"x\" }\n"
													"}\n");
		CHECK(root.has_value());
		if (root) {
			CHECK_EQ_STR(root->GetString("QUOTED"), "a value");
			CHECK_EQ_INT(root->GetInt("unquoted"), 42);
			const KvNode* both = root->Find("both");
			CHECK(both != nullptr);
			if (both != nullptr) {
				CHECK(both->hasValue);
				CHECK_EQ_STR(both->GetString("inner"), "x");
			}
		}
	}
	// // and /* */ comments, #base / #include lines skipped instead of followed.
	{
		const std::optional<KvNode> root = ParseKv1("// header\n"
													"#base \"other.ini\"\n"
													"Root { /* inline */ \"a\" /* between */ \"1\" // after\n"
													"       \"b\" \"2\" }\n");
		CHECK(root.has_value());
		if (root) {
			CHECK_EQ_INT(root->GetInt("a"), 1);
			CHECK_EQ_INT(root->GetInt("b"), 2);
		}
	}
	// Escapes inside quotes (\" \\ \n \r \t); an unknown escape stays verbatim.
	{
		const std::optional<KvNode> root = ParseKv1("Root { \"quote\" \"a\\\"b\" \"slash\" \"a\\\\b\" "
													"\"nl\" \"a\\nb\" \"unknown\" \"a\\qb\" }");
		CHECK(root.has_value());
		if (root) {
			CHECK_EQ_STR(root->GetString("quote"), "a\"b");
			CHECK_EQ_STR(root->GetString("slash"), "a\\b");
			CHECK_EQ_STR(root->GetString("nl"), "a\nb");
			CHECK_EQ_STR(root->GetString("unknown"), "a\\qb");
		}
	}
	// A UTF-8 BOM is ignored (a BOM would otherwise become part of the first key).
	{
		const std::string text = std::string("\xEF\xBB\xBF") + "\"LR_FakeRank\" { \"Type\" \"4\" }";
		const std::optional<KvNode> root = ParseKv1(text);
		CHECK(root.has_value());
		if (root) {
			CHECK_EQ_STR(root->name, "LR_FakeRank");
			CHECK_EQ_INT(root->GetInt("Type"), 4);
		}
	}
	// A single top-level block becomes the root; a flat file keeps its top-level keys.
	{
		const std::optional<KvNode> unwrapped = ParseKv1("\"LR_FakeRank\" { \"Type\" \"1\" }");
		CHECK(unwrapped.has_value());
		if (unwrapped) {
			CHECK_EQ_INT(unwrapped->GetInt("Type"), 1);
		}
		const std::optional<KvNode> flat = ParseKv1("\"Type\" \"3\"");
		CHECK(flat.has_value());
		if (flat) {
			CHECK_EQ_INT(flat->GetInt("Type"), 3);
		}
	}
	// Valve's integer parsing: a value is trimmed, partial numbers are atoi-like ("5 // x" -> 5),
	// garbage is 0, a missing or empty value keeps the default (KeyValues::GetInt).
	{
		const std::optional<KvNode> root = ParseKv1("Root { \"partial\" \"5 // x\" \"bad\" \"abc\" "
													"\"empty\" \"\" }");
		CHECK(root.has_value());
		if (root) {
			CHECK_EQ_INT(root->GetInt("partial", -1), 5);
			CHECK_EQ_INT(root->GetInt("bad", 7), 0);
			CHECK_EQ_INT(root->GetInt("empty", 7), 7);
			CHECK_EQ_INT(root->GetInt("missing", 9), 9);
			const KvNode* bad = root->Find("bad");
			CHECK(bad != nullptr);
			if (bad != nullptr) {
				CHECK_EQ_INT(bad->IntValue(), 0);
			}
		}
	}
	// Malformed input is rejected as a whole (std::nullopt), never half a tree.
	CHECK(!ParseKv1("\"a\" { \"b\" \"unterminated }").has_value()); // unterminated string
	CHECK(!ParseKv1("\"a\" { \"b\" \"c\"").has_value());            // missing '}'
	CHECK(!ParseKv1("\"a\" \"b\" }").has_value());                  // stray top-level '}'
	CHECK(!ParseKv1("\"a\" { \"b\" \"c\" } /* open").has_value());  // unterminated block comment
}

// --- the configuration ----------------------------------------------------------------------------

void TestRealConfig() {
	Section("real fakerank.ini");
	Config config;
	const ConfigSource source = LoadConfig(LR_FAKERANK_TEST_DATA_DIR, config);
	CHECK(source.loaded);
	CHECK(source.error.empty());
	CHECK(source.problem.empty());
	// tests/data also holds a lr_fakerank/fakerank.ini copy: the levels_ranks path must win.
	CHECK(EndsWith(source.path, "levels_ranks/fakerank.ini"));
	// The shipped file is "Type" "0" -> g_iType 12 and ranks 1..18 mapped to themselves.
	CHECK_EQ_INT(config.rank_type, 12);
	CHECK_EQ_INT(config.ranks.size(), 18);
	for (int rank = 1; rank <= 18; ++rank) {
		CHECK_EQ_INT(config.RankFor(rank), rank);
	}
	// A rank without an entry reads 0 (the original's std::map operator[]).
	CHECK_EQ_INT(config.RankFor(19), 0);
}

void TestConfigCandidates() {
	Section("config candidates");
	const std::vector<std::string> candidates = ConfigCandidates("/srv/plugify/configs");
	CHECK_EQ_INT(candidates.size(), 2);
	if (candidates.size() == 2) {
		CHECK_EQ_STR(candidates[0], "/srv/plugify/configs/levels_ranks/fakerank.ini");
		CHECK_EQ_STR(candidates[1], "/srv/plugify/configs/lr_fakerank/fakerank.ini");
	}
	// A trailing slash is not doubled.
	const std::vector<std::string> withSlash = ConfigCandidates("/cfg/");
	CHECK_EQ_INT(withSlash.size(), 2);
	if (withSlash.size() == 2) {
		CHECK_EQ_STR(withSlash[0], "/cfg/levels_ranks/fakerank.ini");
		CHECK_EQ_STR(withSlash[1], "/cfg/lr_fakerank/fakerank.ini");
	}
	// No configs directory (the host did not supply plg::GetConfigsDir()): nothing to try.
	CHECK(ConfigCandidates("").empty());
	CHECK_EQ_STR(JoinPath("", "a.ini"), "a.ini");
	CHECK_EQ_STR(JoinPath("/cfg", ""), "/cfg");
}

void TestConfigFallback() {
	Section("config fallback");
	// tests/data/fallback only holds lr_fakerank/fakerank.ini, and that file starts with a UTF-8
	// BOM: the second candidate and the BOM skip are exercised together.
	Config config;
	const std::string dir = std::string(LR_FAKERANK_TEST_DATA_DIR) + "/fallback";
	const ConfigSource source = LoadConfig(dir, config);
	CHECK(source.loaded);
	CHECK(source.error.empty());
	CHECK(source.problem.empty());
	CHECK(EndsWith(source.path, "lr_fakerank/fakerank.ini"));
	// "Type" "2" -> g_iType 10.
	CHECK_EQ_INT(config.rank_type, 10);
	CHECK_EQ_INT(config.ranks.size(), 2);
	CHECK_EQ_INT(config.RankFor(1), 5);
	CHECK_EQ_INT(config.RankFor(2), 9);
}

void TestConfigMissing() {
	Section("missing config");
	{
		Config config;
		config.ranks[7] = 3; // the caller's previous table
		const ConfigSource source = LoadConfig(std::string(LR_FAKERANK_TEST_DATA_DIR) + "/does-not-exist", config);
		CHECK(!source.loaded);
		CHECK(!source.error.empty());
		CHECK(source.path.empty());
		// Nothing was parsed, so the caller keeps its state (the plugin clears the table itself).
		CHECK_EQ_INT(config.ranks.size(), 1);
		CHECK_EQ_INT(config.RankFor(7), 3);
	}
	{
		Config config;
		const ConfigSource source = LoadConfig({}, config);
		CHECK(!source.loaded);
		CHECK(source.error.find("configs directory") != std::string::npos);
	}
}

void TestConfigVariants() {
	Section("config variants");
	// The Type switch of the original: 0->12, 1->7, 2->10, 3->11, 4->13.
	const std::pair<int, int> types[] = {{0, 12}, {1, 7}, {2, 10}, {3, 11}, {4, 13}};
	for (const auto& [file_type, expected] : types) {
		const std::optional<KvNode> root = ParseKv1("\"LR_FakeRank\"\n{\n    \"Type\" \"" +
												   std::to_string(file_type) +
												   "\"\n    \"FakeRank\" { \"1\" \"5\" }\n}\n");
		CHECK(root.has_value());
		if (!root) {
			continue;
		}
		Config config;
		ParseConfig(*root, config);
		CHECK_EQ_INT(config.rank_type, expected);
		CHECK_EQ_INT(config.RankFor(1), 5);
	}
	// An unknown Type keeps the previous g_iType (the original's global was not reset).
	{
		Config config;
		config.rank_type = 12;
		config.ranks[1] = 5;
		const std::optional<KvNode> root =
			ParseKv1("\"LR_FakeRank\"\n{\n    \"Type\" \"9\"\n    \"FakeRank\" { \"2\" \"7\" }\n}\n");
		CHECK(root.has_value());
		if (root) {
			ParseConfig(*root, config);
			CHECK_EQ_INT(config.rank_type, 12);
			// g_Ranks.clear() ran first: the old entry is gone, the new one is in.
			CHECK_EQ_INT(config.ranks.size(), 1);
			CHECK_EQ_INT(config.RankFor(1), 0);
			CHECK_EQ_INT(config.RankFor(2), 7);
		}
	}
	// Without a FakeRank block the table stays empty.
	{
		Config config;
		const std::optional<KvNode> root = ParseKv1("\"LR_FakeRank\"\n{\n    \"Type\" \"1\"\n}\n");
		CHECK(root.has_value());
		if (root) {
			ParseConfig(*root, config);
			CHECK(config.ranks.empty());
			CHECK_EQ_INT(config.rank_type, 7);
		}
	}
	// A non-numeric key is skipped with a problem message (the original's std::stoi threw).
	{
		Config config;
		const std::optional<KvNode> root =
			ParseKv1("\"LR_FakeRank\"\n{\n    \"FakeRank\" { \"oops\" \"3\" \"4\" \"9\" }\n}\n");
		CHECK(root.has_value());
		if (root) {
			std::string problem;
			ParseConfig(*root, config, &problem);
			CHECK(!problem.empty());
			CHECK_EQ_INT(config.ranks.size(), 1);
			CHECK_EQ_INT(config.RankFor(4), 9);
		}
	}
	// A block inside FakeRank is not a value node: FOR_EACH_VALUE skipped it in the original too.
	{
		Config config;
		const std::optional<KvNode> root = ParseKv1(
			"\"LR_FakeRank\" { \"Type\" \"1\" \"FakeRank\" { \"1\" \"5\" \"nested\" { \"x\" \"y\" } } }");
		CHECK(root.has_value());
		if (root) {
			ParseConfig(*root, config);
			CHECK_EQ_INT(config.ranks.size(), 1);
			CHECK_EQ_INT(config.RankFor(1), 5);
		}
	}
}

// --- the desired values ---------------------------------------------------------------------------

void TestComputeDesired() {
	Section("ComputeDesired");
	Config config;
	config.rank_type = 12;
	config.ranks[7] = 14;

	// The default branch: the tab rank of the LR rank and the configured type, wins always 777.
	{
		const Desired desired = ComputeDesired(config, 7, 12345);
		CHECK_EQ_INT(desired.wins, 777);
		CHECK_EQ_INT(desired.ranking, 14);
		CHECK_EQ_INT(desired.rank_type, 12);
	}
	// An unknown LR rank reads 0 (the map's operator[]).
	CHECK_EQ_INT(ComputeDesired(config, 9, 0).ranking, 0);

	// Type 3 (rank_type 11): the raw experience, type 11.
	config.rank_type = 11;
	{
		const Desired desired = ComputeDesired(config, 7, 12345);
		CHECK_EQ_INT(desired.ranking, 12345);
		CHECK_EQ_INT(desired.rank_type, 11);
	}
	// Type 4 (rank_type 13): the tab rank of the LR rank, type 11.
	config.rank_type = 13;
	{
		const Desired desired = ComputeDesired(config, 7, 12345);
		CHECK_EQ_INT(desired.ranking, 14);
		CHECK_EQ_INT(desired.rank_type, 11);
	}
}

// --- the schema offset lookup policy --------------------------------------------------------------

int g_lookupCalls = 0;
int32_t g_nextOffset = -1;

int32_t FakeLookup(const char* /*className*/, const char* /*fieldName*/) {
	++g_lookupCalls;
	return g_nextOffset;
}

void TestSchemaOffsetCache() {
	Section("map-gated schema offset cache");
	SchemaOffsetCache cache;
	g_lookupCalls = 0;
	g_nextOffset = 42;

	// No map: -1 and no s2sdk call at all (GetSchemaOffset would dereference a null map).
	CHECK_EQ_INT(cache.Offset(/*mapReady=*/false, "CCSPlayerController", "m_iCompetitiveWins", &FakeLookup), -1);
	CHECK_EQ_INT(g_lookupCalls, 0);
	CHECK_EQ_INT(cache.CachedCount(), 0);

	// Map up: the lookup runs and the success is memoised (a cached hit never asks again).
	CHECK_EQ_INT(cache.Offset(true, "CCSPlayerController", "m_iCompetitiveWins", &FakeLookup), 42);
	CHECK_EQ_INT(g_lookupCalls, 1);
	CHECK_EQ_INT(cache.Offset(true, "CCSPlayerController", "m_iCompetitiveWins", &FakeLookup), 42);
	CHECK_EQ_INT(g_lookupCalls, 1);
	CHECK_EQ_INT(cache.CachedCount(), 1);

	// A miss is not cached: the next call asks again (a field missing from this game build must
	// not poison the field forever).
	g_nextOffset = -1;
	CHECK_EQ_INT(cache.Offset(true, "CCSPlayerController", "m_iCompetitiveRanking", &FakeLookup), -1);
	CHECK_EQ_INT(g_lookupCalls, 2);
	CHECK_EQ_INT(cache.Offset(true, "CCSPlayerController", "m_iCompetitiveRanking", &FakeLookup), -1);
	CHECK_EQ_INT(g_lookupCalls, 3);
	CHECK_EQ_INT(cache.CachedCount(), 1);

	// Offset 0 is a valid answer (not a failure) and is cached.
	g_nextOffset = 0;
	CHECK_EQ_INT(cache.Offset(true, "CCSPlayerController", "m_iCompetitiveRankType", &FakeLookup), 0);
	CHECK_EQ_INT(cache.Offset(true, "CCSPlayerController", "m_iCompetitiveRankType", &FakeLookup), 0);
	CHECK_EQ_INT(g_lookupCalls, 4);
	CHECK_EQ_INT(cache.CachedCount(), 2);

	// Empty names / no lookup: -1 without an engine call.
	CHECK_EQ_INT(cache.Offset(true, nullptr, "x", &FakeLookup), -1);
	CHECK_EQ_INT(cache.Offset(true, "x", "", &FakeLookup), -1);
	CHECK_EQ_INT(cache.Offset(true, "x", "y", nullptr), -1);
	CHECK_EQ_INT(g_lookupCalls, 4);

	// Clear() (OnPluginEnd) forgets everything: a reload re-resolves the fields.
	cache.Clear();
	CHECK_EQ_INT(cache.CachedCount(), 0);
	g_nextOffset = 7;
	CHECK_EQ_INT(cache.Offset(true, "CCSPlayerController", "m_iCompetitiveWins", &FakeLookup), 7);
	CHECK_EQ_INT(g_lookupCalls, 5);
}

// --- the reveal state machine ---------------------------------------------------------------------

void TestReveal() {
	Section("reveal state machine");
	constexpr uint64_t tab = kButtonScoreboard;
	constexpr uint64_t other = 1ull << 3; // a movement key, for the "no Tab" frames

	// The rising edge: only the frame that turns Tab on reveals (mode 1 = every press).
	{
		RevealState state;
		CHECK(!ShouldReveal(state, 3, other, kRevealEveryPress));
		CHECK(ShouldReveal(state, 3, tab, kRevealEveryPress));       // press
		CHECK(!ShouldReveal(state, 3, tab, kRevealEveryPress));      // held
		CHECK(!ShouldReveal(state, 3, tab | other, kRevealEveryPress)); // still held
		CHECK(!ShouldReveal(state, 3, other, kRevealEveryPress));    // released
		CHECK(ShouldReveal(state, 3, tab, kRevealEveryPress));       // pressed again
	}
	// Mode 0: never.
	{
		RevealState state;
		CHECK(!ShouldReveal(state, 1, tab, kRevealNever));
		CHECK(!ShouldReveal(state, 1, other, kRevealNever));
		CHECK(!ShouldReveal(state, 1, tab, kRevealNever));
	}
	// Mode 2: once per player and map; ResetRevealed (the map start) arms it again (a held Tab is
	// still no new edge - only the original's revealed flag is reset).
	{
		RevealState state;
		CHECK(ShouldReveal(state, 1, tab, kRevealOncePerMap));
		CHECK(!ShouldReveal(state, 1, other, kRevealOncePerMap));
		CHECK(!ShouldReveal(state, 1, tab, kRevealOncePerMap)); // second press of the map
		ResetRevealed(state);
		CHECK(!ShouldReveal(state, 1, tab, kRevealOncePerMap)); // still held across the map change
		CHECK(!ShouldReveal(state, 1, other, kRevealOncePerMap)); // released
		CHECK(ShouldReveal(state, 1, tab, kRevealOncePerMap)); // a new press on the new map
	}
	// The disconnect branch clears the revealed flag (mode 2 fires again after a reconnect).
	{
		RevealState state;
		CHECK(ShouldReveal(state, 5, tab, kRevealOncePerMap));
		MarkDisconnected(state, 5);
		CHECK(!ShouldReveal(state, 5, tab, kRevealOncePerMap));    // the old Tab state is kept
		CHECK(!ShouldReveal(state, 5, other, kRevealOncePerMap)); // released after the reconnect
		CHECK(ShouldReveal(state, 5, tab, kRevealOncePerMap));    // pressed again
	}
	// Slots are independent, and an out-of-range slot is a no-op.
	{
		RevealState state;
		CHECK(ShouldReveal(state, 2, tab, kRevealEveryPress));
		CHECK(!ShouldReveal(state, 2, tab, kRevealEveryPress));
		CHECK(ShouldReveal(state, 4, tab, kRevealEveryPress));
		CHECK(!ShouldReveal(state, -1, tab, kRevealEveryPress));
		CHECK(!ShouldReveal(state, kMaxSlots, tab, kRevealEveryPress));
	}
	// The button bit is the original's bit 33.
	CHECK_EQ_INT(kButtonScoreboard, 1ull << 33);
}

void TestHandleValidity() {
	Section("entity handle validity");
	// The case seen in practice: index 93, serial 102949 -> 0xC912805D = -921534371, a valid
	// handle the game hands out once its serial counter has passed 65535 (bit 31 set).
	CHECK(IsValidHandle(-921534371));
	CHECK(IsValidHandle(0x7FFFFFFF));
	CHECK(IsValidHandle(1));
	CHECK(!IsValidHandle(0));  // "none" / index 0 (the world)
	CHECK(!IsValidHandle(-1)); // INVALID_EHANDLE_INDEX (0xFFFFFFFF)
}

} // namespace

int main() {
	std::printf("lr_fakerank_tests\n");

	TestKvReader();
	TestRealConfig();
	TestConfigCandidates();
	TestConfigFallback();
	TestConfigMissing();
	TestConfigVariants();
	TestComputeDesired();
	TestSchemaOffsetCache();
	TestReveal();
	TestHandleValidity();

	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
