# vip (Plugify)

VIP core: VIP groups (groups.ini), per-server VIP users (vip_users, the same table as the Metamod plugin so both can share one database), feature values with per-player toggles (permissions cookies, keys '<group>.<feature>'), the VIP menu and the plugin-facing IVIPApi. A port of the Metamod:Source plugin 'cs2-vip' by Pisex. Pure s2sdk: no pisexcompat and no wrapper libraries; plgdb is the only shared helper library (asynchronous MySQL/SQLite), permissions (the cookie store) and hudmenu (the player's own menu type) are optional plugin APIs.

Plugify port (fork) of **cs2-vip** by Pisex (Metamod (C++)).
Original: https://github.com/Pisex/cs2-vip
The code was rewritten for Plugify / s2sdk.

## Requirements
- [Plugify](https://github.com/untrustedmodders/plugify) with [s2sdk](https://github.com/untrustedmodders/plugify-plugin-s2sdk)
- Plugin version: 1.2.7
- Plugify dependencies: s2sdk (>=2.18.2), hudmenu (>=1.0.0, optional), permissions (>=1.3.1, optional)
- Self-contained s2sdk integration: uses s2sdk directly, without the pisexcompat library.

## Round policy
The gameplay modules vip_regen_hp, vip_regen_armor, vip_vampirism, vip_jumps and vip_duckspeed use completed-round thresholds configured per group. The default `1` skips both pistol rounds and requires at least one completed round; `0` explicitly opts out; `N >= 2` sets a completed-round threshold.
Group keys: `RegenHP_round_min`, `RegenArmor_round_min`, `Vampirism_round_min`, `jumps_round_min`, `duckspeed_round_min`.
VIP core `>= 1.2.7` keeps restricted effects blocked after a late core load until the next prestart.

## Installation
Download `vip-1.2.7.zip` from Releases and extract it into `game/csgo/`.

## Building
Requires Docker. `./build.sh` builds in the Steam Runtime 3 (sniper) SDK image; the package
is written to `build/package/vip`.

## License
GPL-3.0, see LICENSE.

