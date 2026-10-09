// lr_fakerank — Plugify C++ port of the Pisex LR module "[LR] FakeRank"
// (ref/src/cs2-lvl_ranks_modules/LR_FakeRanks/lr_fakerank.cpp, 270 lines; the production plugin
// addons/lr_modules/lr_fakerank, vdf lr_fakerank.vdf).
//
// What it does (identical to the original): every frame it writes the fake competitive values
// (m_iCompetitiveWins = 777, m_iCompetitiveRanking, m_iCompetitiveRankType - the type and the
// rank mapping from <configs>/levels_ranks/fakerank.ini) into every connected player's controller,
// and when a player presses Tab (+showscores) it sends him CCSUsrMsg_ServerRankRevealAll so the
// scoreboard shows them. mm_fakerank_reveal (0/1/2) and mm_reload_fakerank are the module's own
// controls, like in the original.
//
// Port notes (the rest is parity):
//   * s2sdk's name-based schema setters refuse the competitive fields (they are on its
//     CS2BadList, guarded by FollowCS2ServerGuidelines), so the port writes them through the
//     offset-based SetEntData/GetEntData and marks the change with the name-based
//     NetworkStateChanged - the same values, the same goal; a field whose offset cannot be
//     resolved is logged once and skipped (never a crash).
//   * A value is only written when it differs from the current one (read back each frame), so the
//     per-frame loop of the original costs almost nothing; the engine rewriting the field is
//     repaired on the next frame like the original's unconditional assignment did.
//   * IServerGameDLL::GameFrame becomes the per-frame OnPluginUpdate; the Tab button comes from
//     s2sdk's GetClientButtons(slot, 0) (the button-state word the original read through
//     m_pMovementServices->m_nButtons().m_pButtonStates()[0]).
//   * The rank reveal is a usermessage (UserMessageFindMessageIdByName +
//     UserMessageSetRecipientMask + UserMessageSend) instead of PostEventAbstract + a player
//     filter; the message is the same CCSUsrMsg_ServerRankRevealAll, sent to the same slots.
//   * StartupServer() (the per-map revealed reset) becomes pisex::MapStartHook.

#include <pisex/pisex.hpp>

#include <plg/plugin.hpp>

#include <s2sdk/s2sdk.hpp>

#include <lr_api.hpp>

#include <chrono>
#include <cstdint>
#include <exception>
#include <string>

#include "fakerank_core.hpp"

namespace {

// GetLogTag() of the original.
constexpr char kLogTag[] = "[LR-FR]";

// The original's g_pLRCore.
levels_ranks::LrApi* g_pLRCore = &levels_ranks::LrApi::Get();

// The class the four fields live on (the original's CCSPlayerController).
constexpr char kControllerClass[] = "CCSPlayerController";
constexpr char kFieldWins[] = "m_iCompetitiveWins";
constexpr char kFieldRanking[] = "m_iCompetitiveRanking";
constexpr char kFieldRankType[] = "m_iCompetitiveRankType";
constexpr char kRevealMessage[] = "CCSUsrMsg_ServerRankRevealAll";

using namespace lr_fakerank;

class LrFakerankPlugin final : public plg::Plugin {
public:
	plg::PluginResult OnPluginStart() override;
	plg::PluginResult OnPluginUpdate(std::chrono::milliseconds dt) override;
	plg::PluginResult OnPluginEnd() override;

	// ReloadCommand of the original (mm_reload_fakerank).
	void Reload();

private:
	// LoadConfig() of the original.
	void LoadConfig();
	// The GameFrame body of the original, once per frame.
	void Tick();
	// The offset of a controller field (cached; -1 until the schema system can answer, logged once
	// per field when it cannot).
	int FieldOffset(int which, const char* field);
	// The write of one field: skipped when the value is already there, otherwise written at the
	// offset and marked changed by name (the name-based path resolves the chain offset itself).
	// |handle| is the controller's entity handle (resolved once per player and frame in Tick - the
	// conversion is an s2sdk call, not a cast). |size| must be the schema width of the field
	// (m_iCompetitiveRankType is int8, the other two int32 - a wrong width destroys the neighbours
	// or never compares equal).
	void WriteControllerField(int handle, int offset, const char* field, int value, int size);
	// The reveal usermessage to |mask| recipients.
	void SendReveal(uint64_t mask);
	// mm_fakerank_reveal (default 1 when the cvar API is missing).
	int RevealMode() const;

	Config m_config;
	RevealState m_reveal;
	uint64_t m_cvar_reveal = 0; // mm_fakerank_reveal handle
	int m_offsets[3] = {-1, -1, -1};
	bool m_logged_offset[3] = {false, false, false};
	bool m_loaded = false; // the original's bLoaded (CoreIsReady)
	bool m_message_resolved = false;
	bool m_logged_message = false;
	int16_t m_message_id = 0;
};

LrFakerankPlugin g_plugin;

// --- configuration --------------------------------------------------------------------------------

void LrFakerankPlugin::LoadConfig() {
	// LoadConfig() of the original: KeyValues("LR_FakeRank") ->
	// addons/configs/levels_ranks/fakerank.ini, g_Ranks cleared first.
	pisex::KeyValues kv("LR_FakeRank");
	if (!kv.LoadFromFile("addons/configs/levels_ranks/fakerank.ini")) {
		// Warning("Failed to load %s") of the original.
		pisex::ErrorLog("%s Failed to load addons/configs/levels_ranks/fakerank.ini", kLogTag);
		m_config.ranks.clear();
		return;
	}
	std::string problem;
	ParseConfig(kv, m_config, &problem);
	if (!problem.empty()) {
		pisex::ErrorLog("%s fakerank.ini: %s", kLogTag, problem.c_str());
	}
}

void LrFakerankPlugin::Reload() {
	LoadConfig();
}

// --- the controller fields ------------------------------------------------------------------------

int LrFakerankPlugin::FieldOffset(int which, const char* field) {
	if (m_offsets[which] < 0) {
		// pisex::SchemaOffset caches successful lookups only, so a query before the schema system
		// is up simply retries on the next frame.
		const int offset = pisex::SchemaOffset(kControllerClass, field);
		if (offset < 0) {
			if (!m_logged_offset[which] && pisex::IsGameReady()) {
				m_logged_offset[which] = true;
				pisex::ErrorLog("%s the schema field %s::%s could not be resolved - the fake rank stays off",
								kLogTag, kControllerClass, field);
			}
			return -1;
		}
		m_offsets[which] = offset;
	}
	return m_offsets[which];
}

void LrFakerankPlugin::WriteControllerField(int handle, int offset, const char* field, int value, int size) {
	if (offset < 0 || size <= 0 || !lr_fakerank::IsValidHandle(handle) ||
		__s2sdk_GetEntData == nullptr || __s2sdk_SetEntData == nullptr ||
		__s2sdk_NetworkStateChanged == nullptr) {
		return;
	}
	if (static_cast<int>(s2sdk::GetEntData(handle, offset, size)) == value) {
		return; // already the value the client knows; the original's assignment was a no-op too
	}
	s2sdk::SetEntData(handle, offset, value, size, /*changeState=*/false, /*chainOffset=*/0);
	// Marking the field changed by name (NetworkStateChanged2 resolves the chain offset itself;
	// the name-based lookup is not on the CS2BadList - only the setters are).
	s2sdk::NetworkStateChanged(handle, plg::string(kControllerClass), plg::string(field));
}

// --- the reveal -----------------------------------------------------------------------------------

void LrFakerankPlugin::SendReveal(uint64_t mask) {
	// s2sdk's user-message API: the original used
	// g_pGameEventSystem->PostEventAbstract(-1, false, ABSOLUTE_PLAYER_LIMIT, filter.Base(), netmsg,
	// msg, 0, BUF_RELIABLE) with the same CCSUsrMsg_ServerRankRevealAll.
	if (__s2sdk_UserMessageFindMessageIdByName == nullptr || __s2sdk_UserMessageCreateFromId == nullptr ||
		__s2sdk_UserMessageSetRecipientMask == nullptr || __s2sdk_UserMessageSend == nullptr ||
		__s2sdk_UserMessageDestroy == nullptr) {
		return;
	}
	if (!m_message_resolved) {
		m_message_resolved = true;
		m_message_id = s2sdk::UserMessageFindMessageIdByName(plg::string(kRevealMessage));
		if (m_message_id <= 0 && !m_logged_message) {
			m_logged_message = true;
			pisex::ErrorLog("%s the usermessage %s was not found - the scoreboard reveal is off",
							kLogTag, kRevealMessage);
		}
	}
	if (m_message_id <= 0) {
		return;
	}
	void* message = s2sdk::UserMessageCreateFromId(m_message_id);
	if (message == nullptr) {
		return;
	}
	s2sdk::UserMessageSetRecipientMask(message, mask);
	s2sdk::UserMessageSend(message);
	s2sdk::UserMessageDestroy(message);
}

// --- the frame ------------------------------------------------------------------------------------

int LrFakerankPlugin::RevealMode() const {
	if (m_cvar_reveal == 0 || __s2sdk_GetConVarInt32 == nullptr) {
		return kRevealEveryPress; // the original's default 1
	}
	return static_cast<int>(s2sdk::GetConVarInt32(m_cvar_reveal));
}

void LrFakerankPlugin::Tick() {
	// LR_FakeRank::GameFrame of the original: only with the core ready (bLoaded) and a live map.
	if (!m_loaded || !pisex::IsGameReady()) {
		return;
	}
	const int offset_wins = FieldOffset(0, kFieldWins);
	const int offset_ranking = FieldOffset(1, kFieldRanking);
	const int offset_rank_type = FieldOffset(2, kFieldRankType);
	if (offset_wins < 0 && offset_ranking < 0 && offset_rank_type < 0) {
		return; // nothing to write (already logged); the reveal would show values we could not set
	}
	const int reveal_mode = RevealMode();
	uint64_t filter = 0;
	int count = 0;
	for (int slot = 0; slot < kMaxSlots; ++slot) {
		if (!pisex::g_pPlayers->IsConnected(slot)) {
			MarkDisconnected(m_reveal, slot); // g_bRevealed[i] = false
			continue;
		}
		const int controller = pisex::GetControllerEntity(slot);
		if (controller == 0) {
			continue;
		}
		// "the slot has a pawn" (the original's `if(!pPawn) continue;`): s2sdk::GetClientPawn answers
		// it with one export, where pisex::GetPawnEntity resolves the controller a second time (its
		// own ClientExists/GetControllerEntity guards) and converts the pawn to an entity index this
		// path never uses. The controller exists here, so the helper cannot log its "invalid player
		// slot" warning.
		if (__s2sdk_GetClientPawn == nullptr || s2sdk::GetClientPawn(slot) == nullptr) {
			continue;
		}
		// Resolved once per player and frame: the three writes below share the same controller, and
		// pisex::EntIndexToEntHandle is an s2sdk call (GetEntityInstance + GetRefEHandle), not a cast.
		const int handle = pisex::EntIndexToEntHandle(controller);
		if (!lr_fakerank::IsValidHandle(handle)) {
			continue;
		}

		// pPlayerController->m_iCompetitiveWins() = 777;
		WriteControllerField(handle, offset_wins, kFieldWins, 777, /*size=*/sizeof(int32_t));
		const Desired desired = ComputeDesired(m_config, g_pLRCore->GetClientInfo(slot, levels_ranks::ST_RANK),
											   g_pLRCore->GetClientInfo(slot, levels_ranks::ST_EXP));
		WriteControllerField(handle, offset_ranking, kFieldRanking, desired.ranking, /*size=*/sizeof(int32_t));
		// m_iCompetitiveRankType is int8 in the schema (the other two are int32)
		WriteControllerField(handle, offset_rank_type, kFieldRankType, desired.rank_type, /*size=*/1);

		// pMovementServices->m_nButtons().m_pButtonStates()[0] of the original.
		if (__s2sdk_GetClientButtons != nullptr) {
			const uint64_t buttons = s2sdk::GetClientButtons(slot, 0);
			if (ShouldReveal(m_reveal, slot, buttons, reveal_mode)) {
				filter |= 1ull << slot;
				++count;
			}
		}
	}
	if (count > 0) {
		SendReveal(filter);
	}
}

// --- lifecycle ------------------------------------------------------------------------------------

plg::PluginResult LrFakerankPlugin::OnPluginStart() {
	try {
		pisex::Init("levels_ranks");

		// ConVar_Register(FCVAR_RELEASE | FCVAR_SERVER_CAN_EXECUTE | FCVAR_GAMEDLL) of the
		// original: mm_fakerank_reveal, default 1.
		if (__s2sdk_CreateConVarInt32 != nullptr) {
			m_cvar_reveal = s2sdk::CreateConVarInt32(
				plg::string("mm_fakerank_reveal"), 1,
				plg::string("Scoreboard rank reveal: 0 never, 1 on every Tab press (original), "
							"2 on the first Tab press per map"),
				static_cast<s2sdk::ConVarFlag>(static_cast<int64_t>(s2sdk::ConVarFlag::Release) |
											   static_cast<int64_t>(s2sdk::ConVarFlag::ServerCanExecute) |
											   static_cast<int64_t>(s2sdk::ConVarFlag::GameDll)),
				false, 0, false, 0);
		}
		if (m_cvar_reveal == 0) {
			pisex::ErrorLog("%s the convar API is missing - mm_fakerank_reveal is not registered "
							"(the reveal defaults to 1)",
							kLogTag);
		}

		// CON_COMMAND_EXTERN(mm_reload_fakerank, ReloadCommand, ...) of the original.
		pisex::RegCommand("mm_reload_fakerank", [](int, const char*) {
			g_plugin.Reload();
			return true;
		});

		LoadConfig();

		// StartupServer() of the original: the revealed flags reset every map.
		pisex::g_pUtils->MapStartHook([this](const char*) { ResetRevealed(m_reveal); });

		// AllPluginsLoaded() of the original.
		if (g_pLRCore->CoreIsLoaded()) {
			m_loaded = true;
		} else {
			g_pLRCore->HookOnCoreIsReady([this] { m_loaded = true; });
		}
		return plg::PluginResult{};
	} catch (const std::exception& ex) {
		pisex::ErrorLog("%s OnPluginStart failed: %s", kLogTag, ex.what());
	} catch (...) {
		pisex::ErrorLog("%s OnPluginStart failed with an unknown exception", kLogTag);
	}
	return plg::PluginResult{}; // never Failed: that stops every plugify plugin
}

plg::PluginResult LrFakerankPlugin::OnPluginUpdate(std::chrono::milliseconds /*dt*/) {
	try {
		pisex::Poll();
		Tick();
	} catch (const std::exception& ex) {
		pisex::ErrorLog("%s OnPluginUpdate failed: %s", kLogTag, ex.what());
	} catch (...) {
		pisex::ErrorLog("%s OnPluginUpdate failed with an unknown exception", kLogTag);
	}
	return plg::PluginResult{};
}

plg::PluginResult LrFakerankPlugin::OnPluginEnd() {
	try {
		g_pLRCore->UnhookOnCoreIsReady();
		pisex::Shutdown();
	} catch (...) {
		// nothing left to do at unload
	}
	return plg::PluginResult{};
}

} // namespace

PLUGIFY_PLUGIN(PLUGIN_API, &g_plugin)
