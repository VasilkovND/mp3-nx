# mp3-nx: Max Payne 3 on the Nintendo Switch (homebrew, private)

Max Payne 3 (Xbox 360, Title Update 6) statically recompiled to ARM with ReXGlue and drawn with deko3d, on the stack of
gta4-nx (GTA IV, the same RAGE engine), ds2-nx, ds3-nx, gears2-nx and dantes-inferno-nx. **Playable at 540p with a mild
overclock, not finished**: overexposure under lamps and performance at stock clocks remain ([BACKLOG.md](BACKLOG.md);
details in [mp3/PORTING.md](mp3/PORTING.md)).

Nothing derived from the game is in this repository: the code, the shaders and the NRO are made on your computer from
your own copy (`mp3/tools/build_mac.sh`, [mp3/BUILD.md](mp3/BUILD.md)). Keep this repository local.

## Legal

Unofficial fan port with no affiliation to Rockstar Games or Take-Two Interactive. "Max Payne" is a trademark of
Take-Two Interactive Software. You provide your own legally obtained copy of the game. No prebuilt files are
distributed: the NRO contains code recompiled from the game and the shader library the game's own shaders, so both are
made on your own computer from your copy ([mp3/BUILD.md](mp3/BUILD.md)). Running homebrew requires custom firmware,
which violates Nintendo's terms and can get a console banned.

**Official source.** The only official source of this project is this repository: its source code and its releases. I
do not publish ready-made NRO or NSP files, and never anything with game data. Builds, NSP/XCI packages or archives
offered on other sites (ROM sites, file hosts, forums, chats) are not made, endorsed or checked by me, may be modified,
and are not covered by this project; distributing the game's files that way infringes its rights holders' copyright and
is solely the responsibility of whoever does it.

Source code is under the GPL-3.0 license (see [LICENSE](LICENSE)). The SDK changes are under the SDK's BSD-3-Clause
license, and the shader translator changes under MIT. Credits in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
