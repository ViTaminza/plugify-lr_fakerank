// Tests for plugins/lr_fakerank/src/fakerank_core.hpp (engine-free core of the ported
// "[LR] FakeRank" module).
//
//   * the shipped fakerank.ini through the same pisex::KeyValues parser the plugin uses, including
//     the Type mapping and the reload semantics (the type survives a file without a known Type),
//   * the desired controller values of the three switch branches,
//   * the schema offset lookup policy (retry while no map, latch a miss),
//   * the Tab/reveal state machine: the rising edge, the three mm_fakerank_reveal modes, the
//     per-map reset and the disconnect branch.

#include <plg/plugin.hpp>

#include <pisex/keyvalues.hpp>
#include <pisex/paths.hpp>

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>

#include "fakerank_core.hpp"

// A real plugin gets these from PLUGIFY_PLUGIN; the test leaves them null. The path resolver is
// driven through pisex::SetPluginName/SetConfigDir instead.
namespace plg {
GetBaseDirFn GetBaseDir{nullptr};
GetExtensionsDirFn GetExtensionsDir{nullptr};
GetConfigsDirFn GetConfigsDir{nullptr};
GetDataDirFn GetDataDir{nullptr};
GetLogsDirFn GetLogsDir{nullptr};
GetCacheDirFn GetCacheDir{nullptr};
IsLoadedFn IsLoaded{nullptr};
LogFn Log{nullptr};
BeginZoneFn BeginZone{nullptr};
EndZoneFn EndZone{nullptr};
} // namespace plg

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

using namespace lr_fakerank;

// --- the configuration ----------------------------------------------------------------------------

void TestRealConfig() {
	Section("real fakerank.ini");
	pisex::SetPluginName("levels_ranks");
	pisex::SetConfigDir(LR_FAKERANK_TEST_DATA_DIR);

	pisex::KeyValues kv("LR_FakeRank");
	CHECK(kv.LoadFromFile("addons/configs/levels_ranks/fakerank.ini"));

	Config config;
	std::string problem;
	ParseConfig(kv, config, &problem);
	CHECK(problem.empty());
	// The shipped file is "Type" "0" -> g_iType 12 and ranks 1..18 mapped to themselves.
	CHECK_EQ_INT(config.rank_type, 12);
	CHECK_EQ_INT(config.ranks.size(), 18);
	for (int rank = 1; rank <= 18; ++rank) {
		CHECK_EQ_INT(config.RankFor(rank), rank);
	}
	// A rank without an entry reads 0 (the original's std::map operator[]).
	CHECK_EQ_INT(config.RankFor(19), 0);
}

void TestConfigVariants() {
	Section("config variants");
	// The Type switch of the original: 0->12, 1->7, 2->10, 3->11, 4->13.
	const std::pair<int, int> types[] = {{0, 12}, {1, 7}, {2, 10}, {3, 11}, {4, 13}};
	for (const auto& [file_type, expected] : types) {
		pisex::KeyValues kv("LR_FakeRank");
		CHECK(kv.LoadFromBuffer("\"LR_FakeRank\"\n{\n    \"Type\" \"" + std::to_string(file_type) +
								"\"\n    \"FakeRank\" { \"1\" \"5\" }\n}\n"));
		Config config;
		ParseConfig(kv, config);
		CHECK_EQ_INT(config.rank_type, expected);
		CHECK_EQ_INT(config.RankFor(1), 5);
	}
	// An unknown Type keeps the previous g_iType (the original's global was not reset).
	{
		Config config;
		config.rank_type = 12;
		config.ranks[1] = 5;
		pisex::KeyValues kv("LR_FakeRank");
		CHECK(kv.LoadFromBuffer("\"LR_FakeRank\"\n{\n    \"Type\" \"9\"\n    \"FakeRank\" { \"2\" \"7\" }\n}\n"));
		ParseConfig(kv, config);
		CHECK_EQ_INT(config.rank_type, 12);
		// g_Ranks.clear() ran first: the old entry is gone, the new one is in.
		CHECK_EQ_INT(config.ranks.size(), 1);
		CHECK_EQ_INT(config.RankFor(1), 0);
		CHECK_EQ_INT(config.RankFor(2), 7);
	}
	// Without a FakeRank block the table stays empty.
	{
		Config config;
		pisex::KeyValues kv("LR_FakeRank");
		CHECK(kv.LoadFromBuffer("\"LR_FakeRank\"\n{\n    \"Type\" \"1\"\n}\n"));
		ParseConfig(kv, config);
		CHECK(config.ranks.empty());
		CHECK_EQ_INT(config.rank_type, 7);
	}
	// A non-numeric key is skipped with a problem message (the original's std::stoi threw).
	{
		Config config;
		pisex::KeyValues kv("LR_FakeRank");
		CHECK(kv.LoadFromBuffer(
			"\"LR_FakeRank\"\n{\n    \"FakeRank\" { \"oops\" \"3\" \"4\" \"9\" }\n}\n"));
		std::string problem;
		ParseConfig(kv, config, &problem);
		CHECK(!problem.empty());
		CHECK_EQ_INT(config.ranks.size(), 1);
		CHECK_EQ_INT(config.RankFor(4), 9);
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

void TestOffsetLookup() {
	Section("schema offset lookups");
	// A miss before the schema system is up does not latch: the next frame retries, and the offset
	// resolves as soon as one is found. Offset 0 is a valid answer, not a failure.
	{
		int state = kOffsetUnresolved;
		CHECK(!RecordOffsetLookup(state, -1, /*game_ready=*/false));
		CHECK_EQ_INT(state, kOffsetUnresolved);
		CHECK(!RecordOffsetLookup(state, 0, true));
		CHECK_EQ_INT(state, 0);
	}
	// A miss with the game up latches the failure (the state stops being kOffsetUnresolved, so the
	// plugin never asks s2sdk again) and reports the one-time log; a resolved offset is kept.
	{
		int state = kOffsetUnresolved;
		CHECK(RecordOffsetLookup(state, -1, /*game_ready=*/true));
		CHECK_EQ_INT(state, kOffsetFailed);
		CHECK(!RecordOffsetLookup(state, 1234, true));
		CHECK_EQ_INT(state, kOffsetFailed);
	}
	{
		int state = 1234;
		CHECK(!RecordOffsetLookup(state, -1, true));
		CHECK_EQ_INT(state, 1234);
	}
	CHECK_EQ_INT(kOffsetUnresolved, -1);
	CHECK_EQ_INT(kOffsetFailed, -2);
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

	TestRealConfig();
	TestConfigVariants();
	TestComputeDesired();
	TestOffsetLookup();
	TestReveal();
	TestHandleValidity();

	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
