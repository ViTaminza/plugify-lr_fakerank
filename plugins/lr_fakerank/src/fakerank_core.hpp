#pragma once
// Engine-free core of the lr_fakerank port (the Pisex LR module "[LR] FakeRank",
// ref/src/cs2-lvl_ranks_modules/LR_FakeRanks/lr_fakerank.cpp, 270 lines): the fakerank.ini model
// (the "Type" switch and the "FakeRank" table), the values the GameFrame hook writes into the
// controller's competitive fields, and the Tab/reveal state machine (iOldButtons / g_bRevealed).
//
// The engine side (the per-frame tick, the schema field writes, the usermessage, the cvar and the
// command) lives in src/plugin.cpp; everything here is testable.

#include <array>
#include <cstdint>
#include <map>
#include <string>

#include <pisex/keyvalues.hpp>

namespace lr_fakerank {

inline constexpr int kMaxSlots = 64;

// Entity HANDLES are int32 views of the engine's uint32 CEntityHandle (index | serial << 15):
// valid <=> h != INVALID_EHANDLE_INDEX (-1) && h != 0 (index 0 is the world; EntIndexToEntHandle
// answers -1 for "no entity"). The sign means nothing - once the engine's serial counter passes
// 65535 bit 31 is set and the handle is a negative int; a `h > 0` test silently drops those
// controllers (no fake rank written then).
inline bool IsValidHandle(int32_t h) { return h != -1 && h != 0; }

// IN_SCORE ("+showscores", the Tab key) of the button-state word the original read:
// pMovementServices->m_nButtons().m_pButtonStates()[0] & (1ULL << 33).
inline constexpr uint64_t kButtonScoreboard = 1ull << 33;

// The original's mm_fakerank_reveal values.
inline constexpr int kRevealNever = 0;       // 0: never send the reveal
inline constexpr int kRevealEveryPress = 1;  // 1: on every Tab press (the original behaviour)
inline constexpr int kRevealOncePerMap = 2;  // 2: on the first Tab press per player and map

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

// LoadConfig() of the original: the "Type" switch and the FOR_EACH_VALUE loop over "FakeRank".
// The rank table is cleared first (g_Ranks.clear()); a key that is not a number is skipped (the
// original's std::stoi would have thrown); |problem| gets a message when that happens.
void ParseConfig(const pisex::KeyValues& kv, Config& out, std::string* problem = nullptr);

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

// --- the Tab/reveal state (iOldButtons / g_bRevealed) ---------------------------------------------

struct RevealState {
	std::array<uint64_t, kMaxSlots> old_buttons{};
	std::array<bool, kMaxSlots> revealed{};
};

// One slot of the original's GameFrame loop: true when the slot pressed Tab on this frame (the
// rising edge of IN_SCORE) and the reveal mode selects it; the slot's revealed flag is set and its
// previous button state updated. A |reveal_mode| of 0 never reveals.
bool ShouldReveal(RevealState& state, int slot, uint64_t buttons, int reveal_mode);

// The "not connected" branch of the original: g_bRevealed[slot] = false (the old button state is
// left alone there as well).
void MarkDisconnected(RevealState& state, int slot);

// StartupServer() of the original (once per map): every revealed flag is cleared.
void ResetRevealed(RevealState& state);

} // namespace lr_fakerank
