# vip (Plugify)

VIP core: VIP groups (groups.ini), per-server VIP users (vip_users, the same table as the Metamod plugin so both can share one database), feature values with per-player toggles (permissions cookies, keys '<group>.<feature>'), the VIP menu and the plugin-facing IVIPApi. A port of the Metamod:Source plugin 'cs2-vip' by Pisex.

Plugify port (fork) of **cs2-vip** by Pisex (Metamod (C++)).
Original: https://github.com/Pisex/cs2-vip
The code was rewritten for Plugify / s2sdk; behaviour follows the original.

## Requirements
- [Plugify](https://github.com/untrustedmodders/plugify) with [s2sdk](https://github.com/untrustedmodders/plugify-plugin-s2sdk)
- Plugify dependencies: s2sdk, hudmenu (optional), permissions (optional)

## Installation
Download `vip-1.2.4.zip` from Releases and extract it into `game/csgo/`.

## Building
Requires Docker. `./build.sh` builds in the Steam Runtime 3 (sniper) SDK image; the package
is written to `build/package/vip`.

## License
GPL-3.0, see LICENSE.

