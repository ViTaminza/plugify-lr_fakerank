#include "fakerank_core.hpp"

#include <charconv>

namespace lr_fakerank {

int Config::RankFor(int rank) const {
	const auto found = ranks.find(rank);
	return found == ranks.end() ? 0 : found->second; // std::map::operator[] of the original
}

void ParseConfig(const pisex::KeyValues& kv, Config& out, std::string* problem) {
	// LoadConfig() of the original: the rank table is cleared first, then the Type switch (the
	// derived type of the four known values; anything else leaves the previous type alone).
	out.ranks.clear();
	switch (kv.GetInt("Type", 0)) {
	case 0:
		out.rank_type = 12;
		break;
	case 1:
		out.rank_type = 7;
		break;
	case 2:
		out.rank_type = 10;
		break;
	case 3:
		out.rank_type = 11;
		break;
	case 4:
		out.rank_type = 13;
		break;
	default:
		break;
	}

	const pisex::KeyValues* fake = kv.FindKey("FakeRank");
	if (fake == nullptr) {
		return; // the original's missing FakeRank key left its FOR_EACH_VALUE loop unentered
	}
	for (const pisex::KeyValues* value = fake->GetFirstValue(); value != nullptr;
		 value = value->GetNextValue()) {
		const char* name = value->GetName();
		if (name == nullptr || *name == '\0') {
			continue;
		}
		const std::string_view text(name);
		int rank = 0;
		const auto result = std::from_chars(text.data(), text.data() + text.size(), rank);
		if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
			// The original's std::stoi threw here (a config typo took the server down with it).
			if (problem != nullptr) {
				*problem = "the FakeRank key \"" + std::string(text) + "\" is not a rank number";
			}
			continue;
		}
		out.ranks[rank] = value->GetInt(nullptr);
	}
}

Desired ComputeDesired(const Config& config, int lr_rank, int lr_exp) {
	Desired desired;
	desired.wins = 777;
	switch (config.rank_type) {
	case 11:
		// Type 3 (Premier): the raw experience is the rating, the rank type 11.
		desired.ranking = lr_exp;
		desired.rank_type = 11;
		break;
	case 13:
		// Type 4 (Premier with a mapped rank): the tab rank of the LR rank, the rank type 11.
		desired.ranking = config.RankFor(lr_rank);
		desired.rank_type = 11;
		break;
	default:
		desired.ranking = config.RankFor(lr_rank);
		desired.rank_type = config.rank_type;
		break;
	}
	return desired;
}

bool RecordOffsetLookup(int& state, int found, bool game_ready) {
	if (state != kOffsetUnresolved) {
		return false; // resolved, or the failure is already latched
	}
	if (found >= 0) {
		state = found;
		return false;
	}
	if (!game_ready) {
		return false; // the schema system may still come up: try again next frame
	}
	state = kOffsetFailed;
	return true; // latched: the caller logs the one-time error
}

bool ShouldReveal(RevealState& state, int slot, uint64_t buttons, int reveal_mode) {
	if (slot < 0 || slot >= kMaxSlots) {
		return false;
	}
	const std::size_t index = static_cast<std::size_t>(slot);
	const bool pressed_tab =
		(buttons & kButtonScoreboard) != 0 && (state.old_buttons[index] & kButtonScoreboard) == 0;
	state.old_buttons[index] = buttons;
	if (!pressed_tab) {
		return false;
	}
	const bool wanted = reveal_mode == kRevealEveryPress ||
						(reveal_mode == kRevealOncePerMap && !state.revealed[index]);
	if (!wanted) {
		return false;
	}
	state.revealed[index] = true;
	return true;
}

void MarkDisconnected(RevealState& state, int slot) {
	if (slot >= 0 && slot < kMaxSlots) {
		state.revealed[static_cast<std::size_t>(slot)] = false;
	}
}

void ResetRevealed(RevealState& state) {
	state.revealed.fill(false);
}

} // namespace lr_fakerank
