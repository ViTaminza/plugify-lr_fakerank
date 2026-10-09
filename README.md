# lr_fakerank (Plugify)

[LR] FakeRank: shows the Levels Ranks rank (and a fake win count) in the scoreboard by writing the controller's competitive fields from <configs>/levels_ranks/fakerank.ini (rank type 0-4, LR rank -> Tab rank table) and sending CCSUsrMsg_ServerRankRevealAll when a player presses Tab; mm_fakerank_reveal (0/1/2) and mm_reload_fakerank control it. Ported from the Pisex Levels Ranks module LR_FakeRanks (production plugin lr_fakerank). Requires the levels_ranks core; no exported methods.

Plugify port (fork) of **LR_FakeRanks ([LR] FakeRank)** by Pisex (Metamod (C++)).
Original: https://github.com/Pisex/cs2-lvl_ranks_modules
The code was rewritten for Plugify / s2sdk; behaviour follows the original.

## Requirements
- [Plugify](https://github.com/untrustedmodders/plugify) with [s2sdk](https://github.com/untrustedmodders/plugify-plugin-s2sdk)
- Plugify dependencies: s2sdk, levels_ranks, hudmenu (optional)

## Installation
Download `lr_fakerank-1.0.0.zip` from Releases and extract it into `game/csgo/`.

## Building
Requires Docker. `./build.sh` builds in the Steam Runtime 3 (sniper) SDK image; the package
is written to `build/package/lr_fakerank`.

`plugins/levels_ranks` carry only the API headers of the plugins this one uses.

## License
GPL-3.0, see LICENSE.

