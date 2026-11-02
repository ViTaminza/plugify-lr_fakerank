#pragma once
// Engine-free core of the lr_fakerank port (the Pisex LR module "[LR] FakeRank", the original's
// lr_fakerank.cpp): the fakerank.ini reader/model (the "Type" switch and the "FakeRank" table), the
// values the per-frame tick writes into the controller's competitive fields, the Tab/reveal state
// machine (iOldButtons / g_bRevealed) and the map-gated schema-offset cache.
//
// Self-contained on purpose (docs/PLUGIN_STANDARD.md §7: no pisexcompat, no helper library): the
// config is read by the KV1 reader below, which accepts exactly the grammar the original's
// KeyValues parser did. The engine side (every s2sdk call) lives in src/plugin.cpp; everything here
// is testable without a server.

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lr_fakerank {

inline constexpr int kMaxSlots = 64;

// Entity HANDLES are int32 views of the engine's uint32 CEntityHandle (index | serial << 15):
// valid <=> h != INVALID_EHANDLE_INDEX (-1) && h != 0 (index 0 is the world; EntIndexToEntHandle
// answers -1 for "no entity"). The sign means nothing - once the engine's serial counter passes
// 65535 bit 31 is set and the handle is a negative int; a `h > 0` test silently drops those
// controllers (no fake rank written then).
inline bool IsValidHandle(int32_t h) { return h != -1 && h != 0; }

// IN_SCORE ("+showscores", the Tab key) of the button-state word the original read.
inline constexpr uint64_t kButtonScoreboard = 1ull << 33;

// The original's mm_fakerank_reveal values.
inline constexpr int kRevealNever = 0;       // 0: never send the reveal
inline constexpr int kRevealEveryPress = 1;  // 1: on every Tab press (the original behaviour)
inline constexpr int kRevealOncePerMap = 2;  // 2: on the first Tab press per player and map

// --- the KV1 reader (the original loaded the file through KeyValues) -----------------------------

// The production config is the original Metamod file, unchanged (PLUGIN_STANDARD §7: production
// KeyValues files are read as they are, no conversion):
//
//     "LR_FakeRank"
//     {
//         // 0 - обычные ранги ...
//         "Type"      "0"
//         "FakeRank"
//         {
//             "1"     "1"
//             "2"     "2"
//         }
//     }
//
// The grammar is the one the original's KeyValues accepted: quoted ("key" "value") and unquoted
// tokens, a key may carry a value and a block, nested blocks, // and /* */ comments, \" \\ \n \r \t
// escapes inside quotes, and #base / #include / #define lines are skipped instead of followed. A
// UTF-8 BOM is ignored (the production file carries one). A malformed file - an unterminated
// string or comment, a missing '}' or a stray top-level '}' - is rejected as a whole
// (std::nullopt), so half a table is never applied.
//
// Like Valve's loader, a file that is a single "Root" { ... } block is returned as that block, so
// `LoadKv1File(...)->GetInt("Type")` reads the key inside "LR_FakeRank" { ... }. Key lookup is
// case-insensitive.
struct KvNode {
	std::string name;
	std::string value;
	bool hasValue = false;
	std::vector<KvNode> children;

	// First child with that name (case-insensitive, like Valve's FindKey), or nullptr.
	const KvNode* Find(std::string_view key) const;
	// The child's value; `def` when the child is missing or has no value.
	std::string_view GetString(std::string_view key, std::string_view def = {}) const;
	// The child's value as an int, with Valve's atoi fallback ("5x" -> 5, "abc" -> 0); `def` when
	// the child is missing or its value is empty (KeyValues::GetInt(key, def)).
	int GetInt(std::string_view key, int def = 0) const;
	// This node's own value as an int (the original's value->GetInt(nullptr) in the rank loop).
	int IntValue() const;
};

std::optional<KvNode> ParseKv1(std::string_view text);
std::optional<KvNode> LoadKv1File(const std::string& path);

// --- where the file is read from -----------------------------------------------------------------

// The Pisex LR installation keeps every module's config in one directory; the plugin's own folder
// (PLUGIN_STANDARD §3) is the fallback for a clean Plugify install.
inline constexpr std::string_view kLevelsRanksDir = "levels_ranks";
inline constexpr std::string_view kPluginDir = "lr_fakerank";
inline constexpr std::string_view kConfigFile = "fakerank.ini";

std::string JoinPath(std::string_view base, std::string_view name);
bool FileExists(const std::string& path);

// <configsDir>/levels_ranks/fakerank.ini, then <configsDir>/lr_fakerank/fakerank.ini. Empty when
// |configsDir| is empty (the host did not supply plg::GetConfigsDir()).
std::vector<std::string> ConfigCandidates(std::string_view configsDir);

// --- the configuration (g_iType + g_Ranks) -------------------------------------------------------

struct Config {
	// g_iType: the rank type the controller fields are set to. The "Type" switch maps 0..4 to
	// 12/7/10/11/13; an unknown value kept the previous g_iType (0 before the first valid read).
	int rank_type = 0;
	// g_Ranks: "FakeRank" rank -> TAB rank. A rank without an entry reads 0, exactly like the
	// original's std::map operator[].
	std::map<int, int> ranks;

	int RankFor(int rank) const;
};

// The outcome of LoadConfig(): what was read and what was wrong with it.
struct ConfigSource {
	bool loaded = false;    // a candidate file was found and parsed into the Config
	std::string path;       // the file that was read (empty when no candidate existed)
	std::string error;      // why nothing was loaded (empty when |loaded| is true)
	std::string problem;    // non-fatal parse note: a FakeRank key that is not a rank number
};

// Tries ConfigCandidates() in order and parses the first file that exists into |out|. The first
// existing file is authoritative: a malformed one is reported through |error| instead of silently
// falling back (a half-broken primary config must not be masked by an old copy). |out| is only
// modified when a file was parsed (ParseConfig clears the rank table first); the caller keeps its
// previous state otherwise.
ConfigSource LoadConfig(std::string_view configsDir, Config& out);

// LoadConfig() of the original: the "Type" switch and the FOR_EACH_VALUE loop over "FakeRank".
// The rank table is cleared first (g_Ranks.clear()); a key that is not a number is skipped (the
// original's std::stoi would have thrown); |problem| gets a message when that happens.
void ParseConfig(const KvNode& root, Config& out, std::string* problem = nullptr);

// --- the controller fields (the GameFrame body) ---------------------------------------------------

struct Desired {
	int wins = 777;      // m_iCompetitiveWins (the original assigned 777 unconditionally)
	int ranking = 0;     // m_iCompetitiveRanking
	int rank_type = 0;   // m_iCompetitiveRankType
};

// The switch of the original, fed with the current LR statistics:
//   rank_type 11 (Type 3, Premier): the rating is the raw experience, the type 11;
//   rank_type 13 (Type 4, Premier with mapped ranks): the tab rank of the LR rank, the type 11;
//   anything else: the tab rank of the LR rank, the configured type.
Desired ComputeDesired(const Config& config, int lr_rank, int lr_exp);

// --- the schema offset lookups (the plugin's map-gated SchemaOffset) -----------------------------

// The map-gated schema-offset cache (PITFALLS C8). s2sdk's GetSchemaOffset builds its field list
// from the network serializer map, which does not exist before the first map: a lookup from
// OnPluginStart segfaults. Offset() therefore refuses to ask while the plugin's map gate is closed
// and memoises only successful results (offset >= 0): a miss is retried on the next call instead of
// poisoning the field for the session (the game build may simply not have the map up yet, and a
// wrong caching of -1 would hide a field that appears later).
//
// The lookup is injected as a function pointer so the tests can drive the whole policy (gate first,
// successes memoised, failures retried) without an engine.
class SchemaOffsetCache {
public:
	// Both return -1 for "unknown".
	using OffsetLookup = int32_t (*)(const char* className, const char* fieldName);

	// >= 0 offset of className::fieldName, or -1 when no map is up / the field is unknown / an
	// empty name or no lookup was supplied. A cached hit never calls |lookup|.
	int32_t Offset(bool mapReady, const char* className, const char* fieldName, OffsetLookup lookup);

	// Forgets everything; the plugin calls this in OnPluginEnd (a reload re-resolves the fields).
	void Clear() { m_offsets.clear(); }

	// Bookkeeping for the tests.
	std::size_t CachedCount() const { return m_offsets.size(); }

private:
	std::map<std::string, int32_t> m_offsets; // class + '\0' + field -> offset >= 0
};

// --- the Tab/reveal state (iOldButtons / g_bRevealed) ---------------------------------------------

struct RevealState {
	std::array<uint64_t, kMaxSlots> old_buttons{};
	std::array<bool, kMaxSlots> revealed{};
};

// One slot of the original's GameFrame loop: true when the slot pressed Tab on this frame (the
// rising edge of IN_SCORE) and the reveal mode selects it; the slot's revealed flag is set and its
// previous button state updated. A |reveal_mode| of 0 never reveals.
bool ShouldReveal(RevealState& state, int slot, uint64_t buttons, int reveal_mode);

// The "not connected" branch of the original: the revealed flag is cleared (the old button state
// is left alone there as well).
void MarkDisconnected(RevealState& state, int slot);

// StartupServer() of the original (once per map): every revealed flag is cleared.
void ResetRevealed(RevealState& state);

} // namespace lr_fakerank
