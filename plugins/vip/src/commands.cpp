// vip — the commands of the original cs2-vip plugin:
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

#include <cstdint>
#include <cstdlib>
#include <exception>
#include <string>
#include <string_view>
#include <vector>

#include <pisex/pisex.hpp>

#include "vip_plugin.hpp"

namespace vip {

namespace {

// META_CONPRINT / ConColorMsg of the original: the server console.
void ConsoleMessage(const std::string& text) {
	try {
		pisex::PrintToConsole(-1, "%s", CStr(text));
	} catch (...) {
	}
}

} // namespace

void RegisterCommands() {
	// The VIP menu (OnVIPCommand of the original): mm_vip / sm_vip in the console and !vip in
	// chat open the VIP menu with the re-open flag, which is VipPlugin::OpenMenu here.
	pisex::RegCommand({"mm_vip", "sm_vip"}, {"!vip"}, [](int slot, const char* content) {
		(void)content;
		if (slot < 0) {
			return false; // the server console has no menu
		}
		try {
			g_vipPlugin.OpenMenu(slot);
		} catch (const std::exception& ex) {
			LogError(std::string("vip: !vip failed: ") + ex.what());
		} catch (...) {
			LogError("vip: !vip failed");
		}
		return false;
	});

	// Diagnostics (not in the original): every check of the !vip path, per connected slot.
	pisex::RegCommand("vip_status", [](int slot, const char* content) {
		(void)content;
		if (slot >= 0) {
			return false; // server console only
		}
		try {
			for (int32_t target = 0; target < kMaxSlots; ++target) {
				const std::string line = g_vipPlugin.DiagnoseSlot(target);
				if (!line.empty()) {
					ConsoleMessage(line);
				}
			}
		} catch (const std::exception& ex) {
			LogError(std::string("vip: vip_status failed: ") + ex.what());
		} catch (...) {
			LogError("vip: vip_status failed");
		}
		return false;
	});

	pisex::RegServerCommand("vip_reload", [](int slot, const char* content) {
		(void)slot;
		(void)content;
		try {
			if (g_vipPlugin.ReloadVipData()) {
				ConsoleMessage("VIP groups has been successfully updated");
			}
		} catch (const std::exception& ex) {
			LogError(std::string("vip: vip_reload failed: ") + ex.what());
		} catch (...) {
			LogError("vip: vip_reload failed");
		}
		return false;
	});

	pisex::RegServerCommand("mm_reload_vip", [](int slot, const char* content) {
		(void)slot;
		try {
			const pisex::CommandArgs args(content);
			if (args.ArgC() > 1 && args.Arg(1)[0] != '\0') {
				const int target = g_vipPlugin.FindPlayerSlot(args.Arg(1));
				if (target >= 0) {
					g_vipPlugin.ReloadClientVip(target);
				} else {
					ConsoleMessage("[VIP] Player not found");
				}
			} else {
				ConsoleMessage("[VIP] Usage: mm_reload_vip <userid|nickname|accountid>");
			}
		} catch (const std::exception& ex) {
			LogError(std::string("vip: mm_reload_vip failed: ") + ex.what());
		} catch (...) {
			LogError("vip: mm_reload_vip failed");
		}
		return false;
	});

	pisex::RegServerCommand("vip_remove", [](int slot, const char* content) {
		(void)slot;
		try {
			const pisex::CommandArgs args(content);
			if (args.ArgC() > 1 && args.Arg(1)[0] != '\0') {
				const int target = g_vipPlugin.FindPlayerSlot(args.Arg(1));
				if (target < 0) {
					ConsoleMessage("[VIP] Player not found");
				} else if (!g_vipPlugin.IsClientVip(target)) {
					ConsoleMessage("[VIP] The player has no VIP status");
				} else {
					// PARITY: the removal notifies the player and writes the database (the original's defaults).
					g_vipPlugin.RemoveVip(target, true, true);
					ConsoleMessage("[VIP] You have successfully removed the player's VIP status");
				}
			} else {
				ConsoleMessage("[VIP] Usage: vip_remove <userid|nickname|accountid>");
			}
		} catch (const std::exception& ex) {
			LogError(std::string("vip: vip_remove failed: ") + ex.what());
		} catch (...) {
			LogError("vip: vip_remove failed");
		}
		return false;
	});

	pisex::RegServerCommand("vip_give", [](int slot, const char* content) {
		(void)slot;
		try {
			const pisex::CommandArgs args(content);
			if (args.ArgC() > 3 && args.Arg(1)[0] != '\0' && ContainsOnlyDigits(args.Arg(2)) && args.Arg(3)[0] != '\0') {
				const std::string token = args.Arg(1);
				const std::string group = args.Arg(3);
				const std::string time_text = args.Arg(2);
				const int32_t time = std::atoi(time_text.c_str());
				// PARITY: vip_give only accepted a player that already has a pawn.
				const int target = g_vipPlugin.FindPlayerSlot(token, /*require_pawn=*/true);
				if (target >= 0) {
					if (g_vipPlugin.IsClientVip(target)) {
						ConsoleMessage("[VIP] The player already has VIP status");
					} else {
						// PARITY: the give parses the time argument with atoi and writes the database.
						g_vipPlugin.GiveVip(target, time, group, true);
						ConsoleMessage("[VIP] You have successfully granted VIP status");
					}
				} else if (token.size() >= 9 && token.size() <= 11) {
					// PARITY (vip_give, offline branch): an account id typed on the command line
					// becomes a vip_users row with the name 'none'.
					g_vipPlugin.GiveOfflineVip(token, time, group);
					ConsoleMessage("[VIP] You have successfully granted VIP status");
				} else {
					ConsoleMessage("[VIP] Player not found");
				}
			} else {
				ConsoleMessage("[VIP] Usage: vip_give <userid|nickname|accountid> <time_second> <group>");
			}
		} catch (const std::exception& ex) {
			LogError(std::string("vip: vip_give failed: ") + ex.what());
		} catch (...) {
			LogError("vip: vip_give failed");
		}
		return false;
	});
}

} // namespace vip
