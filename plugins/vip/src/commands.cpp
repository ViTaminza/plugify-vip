// vip — the commands of the original cs2-vip plugin, on pure s2sdk (no pisexcompat):
//
//   mm_vip / sm_vip, chat !vip          opens the VIP menu (the only player command)
//   vip_status                          server console: every !vip check per connected slot
//   vip_reload                          reloads the VIP group list (LoadVips + the green message)
//   mm_reload_vip <userid|nickname|accountid>   re-reads the vip_users row of a player
//   vip_remove   <userid|nickname|accountid>    drops the VIP status (VIP_RemoveClientVIP)
//   vip_give <userid|nickname|accountid> <time_second> <group>
//                                              gives VIP to an online player, or INSERTs the row
//                                              of an 9..11 digit account id that is offline
//
// Everything runs on the game thread; the database work is asynchronous (plgdb). The four admin
// commands were console commands in the original (CON_COMMAND_F); they stay server-console-only.
//
// PITFALLS C10/C11:
//   * s2sdk dispatches a chat "!vip" / "/vip" to the ConCommand named "vip" with
//     ConCommandContext::Chat, and a client console "mm_vip" with ::Console. Every callback below
//     answers exactly the sources the original registered it for: the menu command opens the menu
//     from the console spellings mm_vip/sm_vip and from the chat spelling vip, and does nothing for
//     "!mm_vip" (the original had no chat spelling for those names).
//   * the server-console commands accept strictly caller == -1. A -2 (or any other negative)
//     caller is not a bypass: it is rejected with ResultType::Handled without running the handler.
//   * AddConsoleCommand() returns false when the name already exists and attaches the callback as
//     a listener in that case, so the ownership is remembered and OnPluginEnd detaches a created
//     command with RemoveCommand and a pre-existing one with RemoveCommandListener - never
//     RemoveCommand on somebody else's command.
//   * the complete AddConsoleCommand + RemoveCommand + RemoveCommandListener pair is required
//     before the first registration (A7: a callback that cannot be removed would point into this
//     library after unload).

#include <cstdint>
#include <cstdlib>
#include <exception>
#include <string>
#include <string_view>
#include <vector>

#include <plg/string.hpp>
#include <plg/vector.hpp>
#include <s2sdk/s2sdk.hpp>

#include "vip_plugin.hpp"

namespace vip {

namespace {

constexpr s2sdk::ConVarFlag kClientCommandFlags =
	static_cast<s2sdk::ConVarFlag>(static_cast<int64_t>(s2sdk::ConVarFlag::Release) |
									static_cast<int64_t>(s2sdk::ConVarFlag::ClientCanExecute));
constexpr s2sdk::ConVarFlag kServerCommandFlags = s2sdk::ConVarFlag::Release;

struct CommandRegistration {
	std::string name;
	s2sdk::ConCommandCallback callback = nullptr;
	s2sdk::ConVarFlag flags = s2sdk::ConVarFlag::None;
	bool attempted = false;
	bool owned = false; // true: this plugin created the name; false: only a listener is attached
};

std::vector<CommandRegistration> g_commands;
bool g_commands_registered = false;

std::string_view ArgView(const plg::vector<plg::string>& args, std::size_t index) {
	if (index >= args.size()) {
		return {};
	}
	return std::string_view(args[index].data(), args[index].size());
}

std::string ArgText(const plg::vector<plg::string>& args, std::size_t index) {
	return std::string(ArgView(args, index));
}

bool CommandBindingsComplete() {
	return __s2sdk_AddConsoleCommand != nullptr && __s2sdk_RemoveCommand != nullptr &&
		   __s2sdk_RemoveCommandListener != nullptr;
}

// The player command: mm_vip / sm_vip in the console and !vip in chat open the VIP menu with the
// re-open flag, which is VipPlugin::OpenMenu here.
s2sdk::ResultType OnVipCommand(int32_t caller, s2sdk::ConCommandContext context,
							   const plg::vector<plg::string>& args) {
	try {
		const std::string_view name = ArgView(args, 0);
		if (context == s2sdk::ConCommandContext::Chat) {
			// Only "vip" is a chat spelling of the original; "!mm_vip" stays a normal chat line.
			if (name != "vip") {
				return s2sdk::ResultType::Continue;
			}
			if (caller < 0) {
				return s2sdk::ResultType::Handled; // a chat line from the server console has no client
			}
			g_vipPlugin.OpenMenu(caller);
			return s2sdk::ResultType::Handled; // a chat command is swallowed, like the original
		}
		// Console context: only the two console spellings; the "vip" ConCommand exists for the
		// chat chain and has no console behaviour in the original.
		if (name != "mm_vip" && name != "sm_vip") {
			return s2sdk::ResultType::Continue;
		}
		if (caller < 0) {
			return s2sdk::ResultType::Continue; // the server console has no menu
		}
		g_vipPlugin.OpenMenu(caller);
		return s2sdk::ResultType::Continue; // the original's callback answered false
	} catch (const std::exception& ex) {
		LogError(std::string("vip: !vip failed: ") + ex.what());
	} catch (...) {
		LogError("vip: !vip failed");
	}
	return s2sdk::ResultType::Handled;
}

// Diagnostics (not in the original): every check of the !vip path, per connected slot.
s2sdk::ResultType OnStatusCommand(int32_t caller, s2sdk::ConCommandContext /*context*/,
								  const plg::vector<plg::string>& /*args*/) {
	try {
		if (caller != -1) {
			return s2sdk::ResultType::Handled; // server console only
		}
		for (int32_t target = 0; target < kMaxSlots; ++target) {
			const std::string line = g_vipPlugin.DiagnoseSlot(target);
			if (!line.empty()) {
				ServerConsole(line);
			}
		}
	} catch (const std::exception& ex) {
		LogError(std::string("vip: vip_status failed: ") + ex.what());
	} catch (...) {
		LogError("vip: vip_status failed");
	}
	return s2sdk::ResultType::Handled;
}

s2sdk::ResultType OnReloadCommand(int32_t caller, s2sdk::ConCommandContext /*context*/,
								  const plg::vector<plg::string>& /*args*/) {
	try {
		if (caller != -1) {
			return s2sdk::ResultType::Handled;
		}
		if (g_vipPlugin.ReloadVipData()) {
			ServerConsole("VIP groups has been successfully updated");
		}
	} catch (const std::exception& ex) {
		LogError(std::string("vip: vip_reload failed: ") + ex.what());
	} catch (...) {
		LogError("vip: vip_reload failed");
	}
	return s2sdk::ResultType::Handled;
}

s2sdk::ResultType OnReloadVipCommand(int32_t caller, s2sdk::ConCommandContext /*context*/,
									 const plg::vector<plg::string>& args) {
	try {
		if (caller != -1) {
			return s2sdk::ResultType::Handled;
		}
		if (args.size() > 1 && !ArgView(args, 1).empty()) {
			const int target = g_vipPlugin.FindPlayerSlot(ArgText(args, 1));
			if (target >= 0) {
				g_vipPlugin.ReloadClientVip(target);
			} else {
				ServerConsole("[VIP] Player not found");
			}
		} else {
			ServerConsole("[VIP] Usage: mm_reload_vip <userid|nickname|accountid>");
		}
	} catch (const std::exception& ex) {
		LogError(std::string("vip: mm_reload_vip failed: ") + ex.what());
	} catch (...) {
		LogError("vip: mm_reload_vip failed");
	}
	return s2sdk::ResultType::Handled;
}

s2sdk::ResultType OnRemoveCommand(int32_t caller, s2sdk::ConCommandContext /*context*/,
								  const plg::vector<plg::string>& args) {
	try {
		if (caller != -1) {
			return s2sdk::ResultType::Handled;
		}
		if (args.size() > 1 && !ArgView(args, 1).empty()) {
			const int target = g_vipPlugin.FindPlayerSlot(ArgText(args, 1));
			if (target < 0) {
				ServerConsole("[VIP] Player not found");
			} else if (!g_vipPlugin.IsClientVip(target)) {
				ServerConsole("[VIP] The player has no VIP status");
			} else {
				// PARITY: the removal notifies the player and writes the database (the original's defaults).
				g_vipPlugin.RemoveVip(target, true, true);
				ServerConsole("[VIP] You have successfully removed the player's VIP status");
			}
		} else {
			ServerConsole("[VIP] Usage: vip_remove <userid|nickname|accountid>");
		}
	} catch (const std::exception& ex) {
		LogError(std::string("vip: vip_remove failed: ") + ex.what());
	} catch (...) {
		LogError("vip: vip_remove failed");
	}
	return s2sdk::ResultType::Handled;
}

s2sdk::ResultType OnGiveCommand(int32_t caller, s2sdk::ConCommandContext /*context*/,
								const plg::vector<plg::string>& args) {
	try {
		if (caller != -1) {
			return s2sdk::ResultType::Handled;
		}
		const std::string token = ArgText(args, 1);
		const std::string timeText = ArgText(args, 2);
		const std::string group = ArgText(args, 3);
		if (args.size() > 3 && !token.empty() && ContainsOnlyDigits(timeText) && !group.empty()) {
			const int32_t time = std::atoi(timeText.c_str());
			// PARITY: vip_give only accepted a player that already has a pawn.
			const int target = g_vipPlugin.FindPlayerSlot(token, /*require_pawn=*/true);
			if (target >= 0) {
				if (g_vipPlugin.IsClientVip(target)) {
					ServerConsole("[VIP] The player already has VIP status");
				} else {
					// PARITY: the give parses the time argument with atoi and writes the database.
					g_vipPlugin.GiveVip(target, time, group, true);
					ServerConsole("[VIP] You have successfully granted VIP status");
				}
			} else if (ContainsOnlyDigits(token) && token.size() >= 9 && token.size() <= 11) {
				// PARITY (vip_give, offline branch): an account id typed on the command line
				// becomes a vip_users row with the name 'none'. A 9..11 character token that
				// is not a number is a mistyped nickname, not an account id.
				g_vipPlugin.GiveOfflineVip(token, time, group);
				ServerConsole("[VIP] You have successfully granted VIP status");
			} else {
				ServerConsole("[VIP] Player not found");
			}
		} else {
			ServerConsole("[VIP] Usage: vip_give <userid|nickname|accountid> <time_second> <group>");
		}
	} catch (const std::exception& ex) {
		LogError(std::string("vip: vip_give failed: ") + ex.what());
	} catch (...) {
		LogError("vip: vip_give failed");
	}
	return s2sdk::ResultType::Handled;
}

void AddCommandRegistration(const std::string& name, s2sdk::ConCommandCallback callback,
							s2sdk::ConVarFlag flags) {
	if (name.empty()) {
		return;
	}
	for (const CommandRegistration& existing : g_commands) {
		if (existing.name == name) {
			return;
		}
	}
	g_commands.push_back(CommandRegistration{name, callback, flags, false, false});
}

void BuildCommandRegistrations() {
	g_commands.clear();
	// The VIP menu: the console spellings of the original plus the chat spelling. s2sdk chains a
	// chat "!vip" to the ConCommand "vip", so that name has to exist even though it only answers
	// the Chat context.
	AddCommandRegistration("mm_vip", &OnVipCommand, kClientCommandFlags);
	AddCommandRegistration("sm_vip", &OnVipCommand, kClientCommandFlags);
	AddCommandRegistration("vip", &OnVipCommand, kClientCommandFlags);
	// The server-console commands (Release only: a client console must not execute them at all;
	// the callbacks reject every caller != -1 as well, which covers the chat dispatch path).
	AddCommandRegistration("vip_status", &OnStatusCommand, kServerCommandFlags);
	AddCommandRegistration("vip_reload", &OnReloadCommand, kServerCommandFlags);
	AddCommandRegistration("mm_reload_vip", &OnReloadVipCommand, kServerCommandFlags);
	AddCommandRegistration("vip_remove", &OnRemoveCommand, kServerCommandFlags);
	AddCommandRegistration("vip_give", &OnGiveCommand, kServerCommandFlags);
}

void AttachCommand(CommandRegistration& command) {
	if (command.attempted) {
		return;
	}
	if (!CommandBindingsComplete()) {
		return; // RegisterCommands() logged once
	}
	command.attempted = true;
	try {
		command.owned = s2sdk::AddConsoleCommand(plg::string(command.name), plg::string("vip"),
												 command.flags, command.callback, s2sdk::HookMode::Pre);
	} catch (...) {
		command.owned = false;
	}
	if (!command.owned) {
		// false = the name already existed (engine or another plugin) and s2sdk attached our
		// callback as a listener: the command keeps working through it, and OnPluginEnd must
		// detach only that listener (PITFALLS C11).
		LogInfo("vip: '" + command.name + "' already exists; reusing the entry (the listener is attached)");
	}
}

void DetachCommand(CommandRegistration& command) {
	if (!command.attempted) {
		return;
	}
	const bool owned = command.owned;
	command.attempted = false;
	command.owned = false;
	if (owned) {
		// This plugin created the command: RemoveCommand detaches the listener and the command.
		if (__s2sdk_RemoveCommand == nullptr) {
			return;
		}
		try {
			s2sdk::RemoveCommand(plg::string(command.name), command.callback);
		} catch (...) {
		}
		return;
	}
	if (__s2sdk_RemoveCommandListener == nullptr) {
		return;
	}
	// The name already existed: only the listener is ours. RemoveCommand would unregister
	// somebody else's command.
	try {
		s2sdk::RemoveCommandListener(plg::string(command.name), command.callback, s2sdk::HookMode::Pre);
	} catch (...) {
	}
}

} // namespace

void RegisterCommands() {
	if (g_commands_registered) {
		return;
	}
	BuildCommandRegistrations();
	if (!CommandBindingsComplete()) {
		LogError("vip: the s2sdk command bindings are unavailable (AddConsoleCommand, RemoveCommand and "
				 "RemoveCommandListener are all required) - no command is registered");
		g_commands.clear();
		return;
	}
	for (CommandRegistration& command : g_commands) {
		AttachCommand(command);
	}
	g_commands_registered = true;
}

void RemoveCommands() {
	for (CommandRegistration& command : g_commands) {
		DetachCommand(command);
	}
	g_commands.clear();
	g_commands_registered = false;
}

} // namespace vip
