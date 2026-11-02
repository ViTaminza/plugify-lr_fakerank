// lr_fakerank — Plugify C++ port of the Pisex LR module "[LR] FakeRank"
// (the original's lr_fakerank.cpp; the Metamod module ships as addons/lr_modules/lr_fakerank
// with lr_fakerank.vdf).
//
// What it does (identical to the original): every frame it writes the fake competitive values
// (the competitive wins, ranking and rank-type fields - the type and the rank mapping from
// <configs>/levels_ranks/fakerank.ini) into every connected player's controller,
// and when a player presses Tab (+showscores) it sends him CCSUsrMsg_ServerRankRevealAll so the
// scoreboard shows them. mm_fakerank_reveal (0/1/2) and mm_reload_fakerank are the module's own
// controls, like in the original.
//
// Port notes (the rest is parity):
//   * Pure s2sdk (docs/PLUGIN_STANDARD.md §7): there is no pisexcompat and no guard library, so
//     every safety rule this module needs lives right here (handoff/weaponskins/PITFALLS.md):
//       map gate (B1/B2)   OnMapStart/OnMapEnd listeners plus the first OnPluginUpdate latch;
//                          no client, entity or schema call before the first map or in the window
//                          between OnMapEnd and the next OnMapStart (s2sdk's client list is torn
//                          down there). OnMapStart also does the original's StartupServer() work
//                          (ResetRevealed), as the old pisex::MapStartHook did.
//       client checks (C2) ControllerHandle() validates the slot, IsClientConnected/InGame,
//                          !IsClientSourceTV, a non-null PlayerSlotToEntPointer and the
//                          PlayerSlotToEntHandle value (IsValidEntHandle + IsValidEntPointer)
//                          before any schema call.
//       schema offsets (C6/C8) SchemaOffset() answers -1 while no map is up and caches only
//                          successful lookups; the cache is cleared in OnPluginEnd. The fields
//                          are written through the offset-based SetEntData/GetEntData because
//                          s2sdk's name-based setters refuse the competitive fields (they are on
//                          its CS2BadList, guarded by FollowCS2ServerGuidelines); the change is
//                          announced through the name-based NetworkStateChanged - the same
//                          values, the same goal. A field whose offset cannot be resolved is
//                          logged once and skipped (never a crash).
//       commands (C11)     mm_reload_fakerank is a server-console command: it is registered with
//                          ConVarFlag::Release only and the callback refuses every caller >= 0
//                          (s2sdk routes chat "!mm_reload_fakerank" / "/mm_reload_fakerank" to
//                          the same callback through ConCommandContext::Chat).
//       unload (A7)        OnPluginEnd detaches the command (RemoveCommand when this plugin created
//                          it, otherwise RemoveCommandListener), unregisters the map listeners,
//                          unhooks the levels_ranks callback and clears the schema cache.
//   * A value is only written when it differs from the current one (read back each frame), so the
//     per-frame loop of the original costs almost nothing; the engine rewriting the field is
//     repaired on the next frame like the original's unconditional assignment did.
//   * IServerGameDLL::GameFrame becomes the per-frame OnPluginUpdate; the Tab button comes from
//     s2sdk's GetClientButtons(slot, 0) (the button-state word the original read).
//   * The rank reveal is a usermessage (UserMessageFindMessageIdByName +
//     UserMessageSetRecipientMask + UserMessageSend) instead of PostEventAbstract + a player
//     filter; the message is the same CCSUsrMsg_ServerRankRevealAll, sent to the same slots.
//   * The config is read by the plugin's own KV1 reader (src/fakerank_core.cpp) from
//     <GetConfigsDir()>/levels_ranks/fakerank.ini, with <GetConfigsDir()>/lr_fakerank/fakerank.ini
//     as the fallback for a clean Plugify install.

#include <plg/plugin.hpp>
#include <plugin_export.h>

#include <s2sdk/s2sdk.hpp>

#include <lr_api.hpp>

#include <chrono>
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>

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
constexpr char kReloadCommand[] = "mm_reload_fakerank";

// s2sdk player slots are 0..63 (the engine's MAXPLAYERS).
constexpr int32_t kMaxPlayers = 64;

using namespace lr_fakerank;

void LogMessage(std::string_view message, plg::Severity severity) {
	if (plg::Log) {
		plg::Log(message, severity, plg::source_location::current());
	}
}

void LogInfo(std::string_view message) {
	LogMessage(message, plg::Severity::Info);
}

void LogWarn(std::string_view message) {
	LogMessage(message, plg::Severity::Warning);
}

void LogError(std::string_view message) {
	LogMessage(message, plg::Severity::Error);
}

std::string ToString(const plg::string& text) {
	return std::string(std::string_view(text));
}

// --- state ----------------------------------------------------------------------------------------

Config g_config;
RevealState g_reveal;
SchemaOffsetCache g_schemaOffsets;

uint64_t g_cvarReveal = 0; // mm_fakerank_reveal handle
bool g_coreReady = false;  // the original's bLoaded (CoreIsReady)
bool g_loggedOffsets[3] = {false, false, false};
bool g_messageResolved = false;
bool g_loggedMessage = false;
int16_t g_messageId = 0;
bool g_commandAttached = false;
bool g_commandOwned = false;
bool g_loggedRemoteCaller = false;

// --- map gate (PITFALLS B1/B2) --------------------------------------------------------------------

bool g_mapReady = false;      // client/entity/schema calls are safe right now
bool g_mapStartSeen = false;  // an OnMapStart arrived since OnPluginStart / the last OnMapEnd
bool g_mapEndSeen = false;    // an OnMapEnd arrived since OnPluginStart
bool g_mapListenersRegistered = false;

// s2sdk listeners are plain function pointers called on the game thread; nothing here may throw,
// and no client query is made - only state changes.
void OnMapStartListener() {
	// The new map and its entity system exist: the PITFALLS B1 work is legal from this point on.
	g_mapReady = true;
	g_mapStartSeen = true;
	g_mapEndSeen = false;
	// StartupServer() of the original: the revealed flags are reset once per map. This is the work
	// the old pisex::MapStartHook did.
	ResetRevealed(g_reveal);
}

void OnMapEndListener() {
	// PITFALLS B2: from here until the next OnMapStart s2sdk's client list and entity system are
	// torn down and any client query crashes - so this handler only drops state.
	g_mapReady = false;
	g_mapStartSeen = false;
	g_mapEndSeen = true;
}

void RegisterMapListeners() {
	if (g_mapListenersRegistered) {
		return;
	}
	if (__s2sdk_OnMapStart_Register == nullptr || __s2sdk_OnMapEnd_Register == nullptr) {
		// Without the map listeners nothing could ever close the gate again, so it stays closed
		// (PITFALLS B2: fail safe, never return Failed).
		LogError(std::string(kLogTag) + " the s2sdk map listeners are unavailable - the fake rank stays off");
		return;
	}
	s2sdk::OnMapStart_Register(&OnMapStartListener);
	s2sdk::OnMapEnd_Register(&OnMapEndListener);
	g_mapListenersRegistered = true;
}

void UnregisterMapListeners() {
	if (!g_mapListenersRegistered) {
		return;
	}
	g_mapListenersRegistered = false;
	if (__s2sdk_OnMapStart_Unregister != nullptr) {
		s2sdk::OnMapStart_Unregister(&OnMapStartListener);
	}
	if (__s2sdk_OnMapEnd_Unregister != nullptr) {
		s2sdk::OnMapEnd_Unregister(&OnMapEndListener);
	}
}

void PollMapGate() {
	if (!g_mapListenersRegistered) {
		return;
	}
	// A plugin loaded into a running map never saw an OnMapStart: the first update after
	// OnPluginStart is the proof of a live map (the host pumps updates from the server frame).
	// After an OnMapEnd only a new OnMapStart re-opens the gate: the host keeps pumping updates
	// through a changelevel, so `!g_mapEndSeen` must not be enough once one was seen.
	if (!g_mapReady && (g_mapStartSeen || !g_mapEndSeen)) {
		g_mapReady = true;
	}
}

// --- client and controller checks (PITFALLS C2) ---------------------------------------------------

bool CanQueryClient(int32_t slot) {
	return g_mapReady && slot >= 0 && slot < kMaxPlayers;
}

bool IsConnected(int32_t slot) {
	if (!CanQueryClient(slot) || __s2sdk_IsClientConnected == nullptr) {
		return false;
	}
	try {
		return s2sdk::IsClientConnected(slot);
	} catch (...) {
		return false;
	}
}

bool HasController(int32_t slot) {
	if (!CanQueryClient(slot)) {
		return false;
	}
	if (__s2sdk_IsClientConnected == nullptr || __s2sdk_IsClientInGame == nullptr ||
		__s2sdk_IsClientSourceTV == nullptr || __s2sdk_PlayerSlotToEntPointer == nullptr) {
		return false;
	}
	try {
		return s2sdk::IsClientConnected(slot) && s2sdk::IsClientInGame(slot) &&
			   !s2sdk::IsClientSourceTV(slot) && s2sdk::PlayerSlotToEntPointer(slot) != nullptr;
	} catch (...) {
		return false;
	}
}

// The controller's entity handle (index | serial << 15); -1 whenever the slot has no usable
// controller, so every caller can treat -1 as "nothing to do".
int32_t ControllerHandle(int32_t slot) {
	if (!HasController(slot)) {
		return -1;
	}
	if (__s2sdk_PlayerSlotToEntHandle == nullptr || __s2sdk_PlayerSlotToEntPointer == nullptr ||
		__s2sdk_IsValidEntHandle == nullptr || __s2sdk_IsValidEntPointer == nullptr) {
		return -1;
	}
	try {
		const int32_t handle = s2sdk::PlayerSlotToEntHandle(slot);
		if (!IsValidHandle(handle) || !s2sdk::IsValidEntHandle(handle)) {
			return -1;
		}
		// The handle and the slot's entity pointer are two views of the same controller and both
		// have to be valid: the schema exports take the handle, while PlayerSlotToEntPointer is
		// what proves the slot still resolves to that entity (between the two reads the engine can
		// already have reclaimed it, and every s2sdk schema call dereferences the handle).
		void* controller = s2sdk::PlayerSlotToEntPointer(slot);
		if (controller == nullptr || !s2sdk::IsValidEntPointer(controller)) {
			return -1;
		}
		return handle;
	} catch (...) {
		return -1;
	}
}

// --- configuration --------------------------------------------------------------------------------

void LoadConfig() {
	const std::string configsDir = plg::GetConfigsDir ? ToString(plg::GetConfigsDir()) : std::string();
	const ConfigSource source = lr_fakerank::LoadConfig(configsDir, g_config);
	if (!source.loaded) {
		// The load-failure warning of the original (the path list of the port).
		LogError(std::string(kLogTag) + " " + source.error);
		g_config.ranks.clear();
		return;
	}
	if (!source.problem.empty()) {
		LogError(std::string(kLogTag) + " fakerank.ini: " + source.problem);
	}
	LogInfo(std::string(kLogTag) + " loaded " + source.path + " (" +
			std::to_string(g_config.ranks.size()) + " ranks, type " + std::to_string(g_config.rank_type) + ")");
}

// ReloadCommand of the original (mm_reload_fakerank).
void Reload() {
	LoadConfig();
}

// --- the controller fields ------------------------------------------------------------------------

// The -1 wrapper the schema exports answer for "unknown"; s2sdk may throw across the boundary.
int32_t LookupSchemaOffset(const char* className, const char* fieldName) {
	try {
		return s2sdk::GetSchemaOffset(plg::string(className), plg::string(fieldName));
	} catch (...) {
		return -1;
	}
}

// PITFALLS C8: -1 without a ready map (GetSchemaOffset dereferences the null serializer map), and
// only a successful lookup (>= 0) is cached by g_schemaOffsets.
int32_t SchemaOffset(const char* className, const char* fieldName) {
	if (__s2sdk_GetSchemaOffset == nullptr) {
		return -1;
	}
	return g_schemaOffsets.Offset(g_mapReady, className, fieldName, &LookupSchemaOffset);
}

int32_t FieldOffset(int which, const char* field) {
	const int32_t offset = SchemaOffset(kControllerClass, field);
	if (offset < 0 && g_mapReady && which >= 0 && which < 3 && !g_loggedOffsets[which]) {
		g_loggedOffsets[which] = true;
		LogError(std::string(kLogTag) + " the schema field " + kControllerClass + "::" + field +
				 " could not be resolved - the fake rank stays off");
	}
	return offset;
}

// The write of one field: skipped when the value is already there, otherwise written at the
// offset and marked changed by name (the name-based path resolves the chain offset itself).
// |handle| is the controller's entity handle (resolved once per player and frame in Tick - the
// conversion is an s2sdk call, not a cast). |size| must be the schema width of the field
// (m_iCompetitiveRankType is int8, the other two int32 - a wrong width destroys the neighbours
// or never compares equal).
void WriteControllerField(int32_t handle, int32_t offset, const char* field, int value, int size) {
	if (offset < 0 || size <= 0 || !IsValidHandle(handle) || __s2sdk_GetEntData == nullptr ||
		__s2sdk_SetEntData == nullptr || __s2sdk_NetworkStateChanged == nullptr) {
		return;
	}
	try {
		if (static_cast<int>(s2sdk::GetEntData(handle, offset, size)) == value) {
			return; // already the value the client knows; the original's assignment was a no-op too
		}
		s2sdk::SetEntData(handle, offset, value, size, /*changeState=*/false, /*chainOffset=*/0);
		// Marking the field changed by name (NetworkStateChanged2 resolves the chain offset itself;
		// the name-based lookup is not on the CS2BadList - only the setters are).
		s2sdk::NetworkStateChanged(handle, plg::string(kControllerClass), plg::string(field));
	} catch (const std::exception& ex) {
		LogError(std::string(kLogTag) + " writing " + field + " failed: " + ex.what());
	} catch (...) {
		LogError(std::string(kLogTag) + " writing " + field + " failed");
	}
}

// --- the reveal -----------------------------------------------------------------------------------

void SendReveal(uint64_t mask) {
	// s2sdk's user-message API: the original posted the same CCSUsrMsg_ServerRankRevealAll
	// through the game-event system with a player filter.
	if (__s2sdk_UserMessageFindMessageIdByName == nullptr || __s2sdk_UserMessageCreateFromId == nullptr ||
		__s2sdk_UserMessageSetRecipientMask == nullptr || __s2sdk_UserMessageSend == nullptr ||
		__s2sdk_UserMessageDestroy == nullptr) {
		return;
	}
	if (!g_messageResolved) {
		g_messageResolved = true;
		g_messageId = s2sdk::UserMessageFindMessageIdByName(plg::string(kRevealMessage));
		if (g_messageId <= 0 && !g_loggedMessage) {
			g_loggedMessage = true;
			LogError(std::string(kLogTag) + " the usermessage " + kRevealMessage +
					 " was not found - the scoreboard reveal is off");
		}
	}
	if (g_messageId <= 0) {
		return;
	}
	void* message = s2sdk::UserMessageCreateFromId(g_messageId);
	if (message == nullptr) {
		return;
	}
	s2sdk::UserMessageSetRecipientMask(message, mask);
	s2sdk::UserMessageSend(message);
	s2sdk::UserMessageDestroy(message);
}

// --- the frame ------------------------------------------------------------------------------------

int RevealMode() {
	if (g_cvarReveal == 0 || __s2sdk_GetConVarInt32 == nullptr) {
		return kRevealEveryPress; // the original's default 1
	}
	try {
		return static_cast<int>(s2sdk::GetConVarInt32(g_cvarReveal));
	} catch (...) {
		return kRevealEveryPress;
	}
}

void Tick() {
	// LR_FakeRank::GameFrame of the original: only with the core ready (bLoaded) and a live map.
	if (!g_coreReady || !g_mapReady) {
		return;
	}
	const int32_t offsetWins = FieldOffset(0, kFieldWins);
	const int32_t offsetRanking = FieldOffset(1, kFieldRanking);
	const int32_t offsetRankType = FieldOffset(2, kFieldRankType);
	if (offsetWins < 0 && offsetRanking < 0 && offsetRankType < 0) {
		return; // nothing to write (already logged); the reveal would show values we could not set
	}
	const int revealMode = RevealMode();
	uint64_t filter = 0;
	int count = 0;
	for (int32_t slot = 0; slot < kMaxPlayers; ++slot) {
		if (!IsConnected(slot)) {
			MarkDisconnected(g_reveal, slot); // the original cleared its revealed flag
			continue;
		}
		// "the slot has a controller" and the controller's entity handle, validated (PITFALLS C2).
		const int32_t handle = ControllerHandle(slot);
		if (!IsValidHandle(handle)) {
			continue;
		}
		// "the slot has a pawn" (the original's "no pawn, skip" test): s2sdk::GetClientPawn answers
		// it with one export, where a controller-based lookup would resolve the controller a second
		// time and convert the pawn to an entity index this path never uses. The controller exists
		// here, so the helper cannot log its "invalid player slot" warning.
		if (__s2sdk_GetClientPawn == nullptr || s2sdk::GetClientPawn(slot) == nullptr) {
			continue;
		}

		WriteControllerField(handle, offsetWins, kFieldWins, 777, /*size=*/sizeof(int32_t));
		const Desired desired = ComputeDesired(g_config, g_pLRCore->GetClientInfo(slot, levels_ranks::ST_RANK),
											   g_pLRCore->GetClientInfo(slot, levels_ranks::ST_EXP));
		WriteControllerField(handle, offsetRanking, kFieldRanking, desired.ranking, /*size=*/sizeof(int32_t));
		// m_iCompetitiveRankType is int8 in the schema (the other two are int32)
		WriteControllerField(handle, offsetRankType, kFieldRankType, desired.rank_type, /*size=*/1);

		// The button-state word of the original.
		if (__s2sdk_GetClientButtons != nullptr) {
			const uint64_t buttons = s2sdk::GetClientButtons(slot, 0);
			if (ShouldReveal(g_reveal, slot, buttons, revealMode)) {
				filter |= 1ull << slot;
				++count;
			}
		}
	}
	if (count > 0) {
		SendReveal(filter);
	}
}

// --- the cvar and the command (PITFALLS C11) ------------------------------------------------------

void RegisterConVar() {
	if (__s2sdk_CreateConVarInt32 == nullptr) {
		LogError(std::string(kLogTag) + " the convar API is missing - mm_fakerank_reveal is not "
										"registered (the reveal defaults to 1)");
		return;
	}
	try {
		// The cvar registration of the original (with the usual release/server flags):
		// mm_fakerank_reveal, default 1.
		g_cvarReveal = s2sdk::CreateConVarInt32(
			plg::string("mm_fakerank_reveal"), 1,
			plg::string("Scoreboard rank reveal: 0 never, 1 on every Tab press (original), "
						"2 on the first Tab press per map"),
			static_cast<s2sdk::ConVarFlag>(static_cast<int64_t>(s2sdk::ConVarFlag::Release) |
										   static_cast<int64_t>(s2sdk::ConVarFlag::ServerCanExecute) |
										   static_cast<int64_t>(s2sdk::ConVarFlag::GameDll)),
			false, 0, false, 0);
	} catch (...) {
		g_cvarReveal = 0;
	}
	if (g_cvarReveal == 0) {
		LogError(std::string(kLogTag) + " mm_fakerank_reveal could not be registered (the reveal defaults to 1)");
	}
}

// ReloadCommand of the original: re-reads the config. A server-console command only: s2sdk also
// dispatches the chat spellings "!mm_reload_fakerank" / "/mm_reload_fakerank" to this callback
// (ConCommandContext::Chat), and a client must never be able to re-read the file (PITFALLS C11).
s2sdk::ResultType OnReloadCommand(int32_t caller, s2sdk::ConCommandContext /*context*/,
								  const plg::vector<plg::string>& /*args*/) {
	try {
		if (caller >= 0) {
			if (!g_loggedRemoteCaller) {
				g_loggedRemoteCaller = true;
				LogWarn(std::string(kLogTag) + " mm_reload_fakerank is a server-console command - a "
											   "client request was refused");
			}
			return s2sdk::ResultType::Handled;
		}
		Reload();
	} catch (const std::exception& ex) {
		LogError(std::string(kLogTag) + " the reload command failed: " + ex.what());
	} catch (...) {
		LogError(std::string(kLogTag) + " the reload command failed with an unknown exception");
	}
	return s2sdk::ResultType::Handled;
}

void RegisterCommand() {
	if (__s2sdk_AddConsoleCommand == nullptr) {
		LogError(std::string(kLogTag) + " s2sdk AddConsoleCommand is unavailable - mm_reload_fakerank "
										"is not registered");
		return;
	}
	// ConVarFlag::Release only: the command exists for the server console, not for clients.
	const bool owned = s2sdk::AddConsoleCommand(plg::string(kReloadCommand), plg::string(), s2sdk::ConVarFlag::Release,
												&OnReloadCommand, s2sdk::HookMode::Pre);
	g_commandAttached = true;
	g_commandOwned = owned;
	if (!owned) {
		// false = the name already existed (another plugin, or our own entry kept by s2sdk across a
		// `plg reload` - s2sdk cannot destroy a console command); the callback is attached anyway,
		// and OnPluginEnd then removes only the listener.
		LogWarn(std::string(kLogTag) + " the command name \"" + kReloadCommand +
				"\" already exists - hooked onto the existing entry");
	}
}

void DetachCommand() {
	if (!g_commandAttached) {
		return;
	}
	g_commandAttached = false;
	const bool owned = g_commandOwned;
	g_commandOwned = false;
	try {
		if (owned) {
			if (__s2sdk_RemoveCommand != nullptr) {
				s2sdk::RemoveCommand(plg::string(kReloadCommand), &OnReloadCommand);
			}
		} else if (__s2sdk_RemoveCommandListener != nullptr) {
			// The name was not ours: detaching the listener is all this plugin may undo
			// (RemoveCommand would take the other owner's callbacks with it).
			s2sdk::RemoveCommandListener(plg::string(kReloadCommand), &OnReloadCommand, s2sdk::HookMode::Pre);
		}
	} catch (const std::exception& ex) {
		LogError(std::string(kLogTag) + " removing the command failed: " + ex.what());
	} catch (...) {
		LogError(std::string(kLogTag) + " removing the command failed");
	}
}

// --- lifecycle ------------------------------------------------------------------------------------

class LrFakerankPlugin final : public plg::Plugin {
public:
	plg::PluginResult OnPluginStart() override {
		try {
			// The map gate first: nothing below may assume a map (PITFALLS B1). The cvar/command
			// registration and the config read only touch the console APIs and the filesystem.
			RegisterMapListeners();
			RegisterConVar();
			RegisterCommand();
			LoadConfig();

			// AllPluginsLoaded() of the original.
			if (g_pLRCore->CoreIsLoaded()) {
				g_coreReady = true;
			} else {
				g_pLRCore->HookOnCoreIsReady([] { g_coreReady = true; });
			}
		} catch (const std::exception& ex) {
			LogError(std::string(kLogTag) + " OnPluginStart failed: " + ex.what());
		} catch (...) {
			LogError(std::string(kLogTag) + " OnPluginStart failed with an unknown exception");
		}
		return {}; // never Failed (PITFALLS A1): that stops every plugify plugin
	}

	plg::PluginResult OnPluginUpdate(std::chrono::milliseconds /*dt*/) override {
		try {
			// The map-ready latch (the host pumps this from the server frame), then the GameFrame
			// body of the original.
			PollMapGate();
			Tick();
		} catch (const std::exception& ex) {
			LogError(std::string(kLogTag) + " OnPluginUpdate failed: " + ex.what());
		} catch (...) {
			LogError(std::string(kLogTag) + " OnPluginUpdate failed with an unknown exception");
		}
		return {};
	}

	plg::PluginResult OnPluginEnd() override {
		// Symmetrical teardown (PITFALLS A7): nothing may point into this library afterwards.
		g_mapReady = false;
		g_coreReady = false;
		try {
			g_pLRCore->UnhookOnCoreIsReady();
		} catch (...) {
			// levels_ranks is gone: its callback died with it
		}
		try {
			DetachCommand();
		} catch (...) {
			// nothing left to do at unload
		}
		try {
			UnregisterMapListeners();
		} catch (...) {
			// nothing left to do at unload
		}
		g_mapStartSeen = false;
		g_mapEndSeen = false;
		g_schemaOffsets.Clear();
		g_messageResolved = false;
		g_messageId = 0;
		return {};
	}
};

LrFakerankPlugin g_plugin;

} // namespace

PLUGIFY_PLUGIN(PLUGIN_API, &g_plugin)
