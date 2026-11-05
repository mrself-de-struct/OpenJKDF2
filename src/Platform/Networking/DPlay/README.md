# Multiplayer transports (`PLATFORM_DPLAY`)

The multiplayer menu offers two kinds of connection. Both carry the same game
messages through the same `DirectPlay_*`/`stdComm_*` layer
(`stdComm_dplay.c`), so gameplay is identical; `dplay_dispatch.c` routes to
the transport that belongs to the picked connection:

| Connection | Transport | Plays with | Needs |
| --- | --- | --- | --- |
| Internet TCP/IP Connection For DirectPlay | `dplay_native.c` (all platforms; optionally Microsoft's via `dplay_shim.c` on 32-bit Windows) | OpenJKDF2 and the original JK.EXE | reachable ports (TCP 2300, UDP 2350; host UDP 47624), LAN or VPN |
| IPX Wrapper Connection for DirectPlay | `dplay_native.c` (all platforms) | OpenJKDF2 and JK.EXE players using IPXWrapper's IPX connection | the same subnet (LAN, or a VPN such as Radmin), UDP 54792 |
| Imperial Network Protocol | `jkrelay_client.c` | OpenJKDF2 only | a relay server (`tools/jkrelay`) |
| OpenJKDF2 Web Lobby (browser players) | `jkweb_client.c` | the jkdf2mp browser build and other desktop OpenJKDF2 players on the same lobby | a jkdf2mp lobby URL (Cloudflare `lobby.js` or `relay.py`) |

The address box on the join screen holds what the selected connection needs:
the host to search (DirectPlay), the relay server (`relayServer` in
`registry.json`) or the web lobby URL (`webLobbyUrl`). Changes are saved when
the game list refreshes; hosting uses the saved value. `OPENJKDF2_RELAY` and
`OPENJKDF2_WEB_LOBBY` override them.

## Web lobby (jkdf2mp)

`jkweb_client.c` speaks the WebSocket protocol of
[jkdf2mp-bb-server](https://github.com/tristamus/jkdf2mp-bb-server) (`wss://<site>/relay`),
the lobby of [tristamus/OpenJKDF2](https://github.com/tristamus/OpenJKDF2)'s browser build.
WebSocket is done here over libcurl's CONNECT_ONLY stream (plain or TLS), so it
needs no new dependency. A bare `host[:port]` is tried as `wss://` and then
`ws://`; the path defaults to `/relay`.

- Direct links (`jkweb_rtc.c`, libdatachannel): like the browser build, each
  joiner offers a WebRTC data channel to the host ("jk", id 0, reliable and
  ordered; "jk-map", id 1, for map and mod downloads), signaling goes through the
  lobby, and the host forwards packets between joiners. Once a link is up,
  game traffic stays off the lobby; until then, or if it can't be made, it
  goes through the lobby. ICE servers come from the lobby's `ice` line; if it
  sends none, `stun:stun.l.google.com:19302` is used (`OPENJKDF2_ICE` overrides,
  `none` disables). Built by default on Linux (`TARGET_USE_WEBRTC`; CMake
  downloads libdatachannel 0.24.6 unless an installed one is found). Other
  platforms build without it and use the lobby for everything.
- Browser players use OpenJKDF2's own message layouts, not JK.EXE's, so while
  this connection is selected `stdComm_bOpenJkdf2Wire` switches the
  scoreboard record layout, the level checksum seed (0) and two recipient
  rules to OpenJKDF2's. DirectPlay and the relay keep JK.EXE's.
- As in the browser build, the password travels in the game list and is
  checked by the joiner; players are ids 1 (host) to 64.
- Like the browser build, we say `hello <jk1|mots> <base64 name>` on connecting
  (the lobby's landing page counts and names players with it, and it tells the
  lobby which game we host) and ask for `list jk1` or `list mots`, so JK and MotS
  players only see the games they can join. The lobby drops a connection that's
  silent for 2 minutes; we ping every 25 s, in game too.
- Downloads from the host (`jkweb_xfer.c`, the joiner side of the browser
  build's `maps.js`/`mods.js` transfer): after joining, a custom episode GOB
  we lack, or have in another version, comes from the host into
  `episode/` (a different local copy is kept as `<name>.gob.bak`); a host
  mod we don't have comes into `netmods/<first 16 hex digits of its
  fingerprint>` before the restart into it. "JKMT" frames go over the
  "jk-map" channel when it's open, else through the lobby; original game
  files never cross the wire (the host sends references, which we fill in
  from our own retail GOBs, and the result has to match the host's
  SHA-256). A progress dialog with Cancel stays up meanwhile. A host that
  doesn't answer (an older page or desktop build) is waited on 20 s, or 6 s
  when we have a copy of the map, which is then used.
- Serving (`jkweb_xfer.c`, host side of `maps.js`/`mods.js`): when we host,
  our custom episode GOB and our running mod (the `-path` folder, or the one
  extra GOB the engine always loads) are prepared once: the INFO joiners get,
  and the stream with every part that is a copy of a retail file (each file,
  and each file inside a GOB the mod carries) left out as a reference.
  Original episodes, maps over 100 MB, mods over 100 MB or 10000 files,
  mods made only of original files, and mods whose files changed since they
  were fingerprinted aren't sent; the joiner is told why. Our own addition:
  a host can hold back maps and mods by listing them in `noshare.txt` in the
  game folder (one name per line, `#` comments, any path in front ignored,
  case-insensitive, with or without the extension, `*`/`?` wildcards; read
  each time hosting starts). They're answered with status `not-shared`;
  browser joiners show "The host's map couldn't be sent (not-shared)." until
  `maps.js`/`mods.js` get a message for it. Chunks go out a
  few per network poll, only while the link has room (lobby socket
  writable, or under 256 KiB buffered on the "jk-map" channel), so serving
  never holds up the game.
- Mods (`jkmod.c`), matching the browser build's mod support: a host names
  its mod in a tail after its lobby entry ("JKMD", version 2, flags, name,
  SHA-256 of the mod packed into one GOB the way the browser packs it, so the
  same files give the same fingerprint on every platform). Our "running mod" is the
  `-path`/Expansions folder, or else a single non-original GOB in
  `resource/`/`mods/` (the browser counts each such GOB as a mod). The tail
  also says whether the host runs the MotS Remastered pack
  (`JKMRemasteredAssets.goo` in `mods/` or `resource/`, fingerprint = first
  16 bytes of its SHA-256), which isn't counted as a mod: a joiner runs it
  exactly when the host does, restarting without it (`jkRes_bSkipRemasterPack`,
  our files stay where they are) or with it; one without it, or with another
  version, is told so. Version-1 tails from older desktop builds are still
  read; browser pages reject them. Joining a
  game whose host runs another mod restarts into a local copy with the same
  fingerprint (any folder under `expansions/`, `mods/` or `netmods/`) and
  reselects the game; without a local copy it's downloaded from the host
  (above). The join screen shows the game's mod and whether we have it. Not
  supported yet: several always-loaded GOBs at once (the browser combines
  them, we don't name the combination).

Tested against `relay.py`: hosting, clients joining, data both ways, a client
leaving and the host leaving (session lost); with WebRTC, direct links to the
host came up for each joiner, the host forwarded between joiners, and no game
packets went through the lobby once the links were up.

The host screen shows the relay server / web lobby address (in the spot of
OpenJKDF2's old "Server Port" box) when one of those connections is selected,
so a game can be hosted without visiting the join screen first.

# DirectPlay transport

A networking backend that uses real Microsoft DirectPlay, so OpenJKDF2 and
unmodified **JK.EXE 1.0** can host and join each other's multiplayer games.

The game-protocol layer above it (`sithComm`, `sithDSS*`, `jkDSS`, `sithMulti`)
was already wire-accurate. What the standalone builds lacked was JK.EXE's thin
`DirectPlay_*` wrapper; the GNS/Basic/None backends replace it with other
transports. This backend implements the same API on `IDirectPlay3`.

## Files

| File | Purpose |
| --- | --- |
| `dplay_shim.c/.h` | The only code that includes `<dplay.h>`. Wraps COM behind a stdint-only API, so OpenJKDF2's own `DPID`/`DPNAME`/`GUID` typedefs never collide with the Windows ones. |
| `stdComm_dplay.c/.h` | Implements `DirectPlay_*` / `stdComm_*` for the engine: session encoding, receive translation, player enumeration. |
| `scripts/jk_dplay_pcap.py` | Decodes captures: session descriptors, DSS timeline, reliable-message resends/acks. |

Shared-code changes (all behind `PLATFORM_DPLAY`):

- `Win95/stdComm.c`: start the backend; skip the `stdComm_dplayIdSelf = 1` hack
  (real DPIDs are random 32-bit values and appear inside DSS payloads); restore
  the original recipient check in `stdComm_SendToPlayer` (the added
  "always send to slot 0" would make a host send to its own DPID).
- `Devices/sithComm.c`: skip the "dedicated server hax" in reliable-recipient tracking.
- CMake: `TARGET_USE_DPLAY` (default ON for `plat_mingw_x86_32`), no new
  link dependencies (COM via `ole32`/`uuid`, already linked).

## Building

32-bit Windows target (cross-compiles on Linux with `i686-w64-mingw32`):

    ./build_winxp32.sh                       # TARGET_USE_DPLAY on by default
    cmake -DTARGET_USE_DPLAY=OFF ...         # previous behaviour (no networking)

Runtime needs DirectPlay: on Windows, *Turn Windows features on or off →
Legacy Components → DirectPlay*. Under Wine, `winetricks directplay` (native
DirectPlay is more complete than Wine's built-in `dplayx`).

## What is verified vs. inferred

Comments in `stdComm_dplay.c` tag each behaviour `[WIRE]`, `[DECOMP]` or `[INFERRED]`.

Confirmed from a capture of a vanilla JK.EXE host (`[WIRE]`):

- Application GUID `{BF0613C0-DE79-11D0-99C9-00A02476AD4B}`.
- Session name `"<server>:<episode gob>:<map jkl>"`, UTF-16.
- `dwFlags` = `KEEPALIVE | NODATAMESSAGES` (0xC0), `PASSWORDREQUIRED` when passworded.
- `dwUser1` = checksumSeed, `dwUser2` = multiModeFlags,
  `dwUser3` = 0x0008000A (`field_E0` = 10 is the version check), `dwUser4` = tickRateMs.
- Game data uses unguaranteed sends (DirectPlay's UDP path only).
- A client creates its DirectPlay player (short name only) immediately on join.
- A host answers DirectPlay's player-created event with `DSS_LEAVEJOIN`
  (`sithMulti_SyncPlayers`) ~11 ms later, which identifies which
  `stdComm_Recv` branch is the "new player" path.

Still to confirm (`[INFERRED]`), with the check that settles each:

| Item | How to confirm |
| --- | --- |
| High word of `dwUser3` is maxRank | Host vanilla with max rank ≠ 8; run the decoder, check `maxRank=`. |
| `DirectPlay_SetSessionFlagidk(0)` sets `DPSESSION_JOINDISABLED` | Capture a vanilla host's enum reply while it is on the score screen; flags should gain `JOINDISABLED`. |
| `DirectPlay_SetSessionDesc` rewrites only map + max players | Change levels on a vanilla host; compare consecutive session dumps. |
| `DirectPlay_Receive` system-message mapping | Decompile `0x0042FBE0` (166 bytes) in Ghidra. |
| `DirectPlay_StartSession` (implemented as a safe no-op) | Decompile `0x004301A0`. The no-op relies on the client's join-request retry. |
| `stdComm_Close` | Decompile `0x004C9820` (48 bytes). |
| Lobby launch (`EarlyInit`, `SendLobbyMessage`) | Not implemented; only matters for lobby/Zone launching. |

## Interop test matrix

Capture each with `tcpdump -w`, then:

    scripts/jk_dplay_pcap.py run.pcap --src-dir . --host <host-ip> --only sessions,reliable,summary

1. vanilla host + vanilla client (baseline)
2. OpenJKDF2 host + vanilla client
3. vanilla host + OpenJKDF2 client
4. OpenJKDF2 host + OpenJKDF2 client

For each: browse/join, spawn, weapon switches, saber color/skin, kills and
deaths, chat, a level change, one player leaving, and host quitting. Session
dumps in runs 2 and 4 should match run 1 field for field (except the random
instance GUID and checksumSeed). Message counts per type should be in the same
proportions, and reliable messages should be acked without resends on a clean
network.

## Known differences remaining under `QOL_IMPROVEMENTS`

These don't change message formats, but a strict "vanilla profile" would put
them back to original values: `SITH_MAX_THINGS` (thing indices above 640 would
be invalid on vanilla peers; only reachable with more than 641 live things),
multiplayer timeouts and big-sync interval, and the extended tick-rate range
(vanilla peers clamp to 100–300 ms on their side).

## Native backend — `dplay_native.c`

`dplay_native.c` is the default on every platform, Windows included (POSIX
sockets or Winsock). Microsoft's DirectPlay is a 32-bit-only Windows component,
so 64-bit Windows builds can't use it; the native one also needs no "Legacy
Components → DirectPlay" install. 32-bit Windows builds can still use
Microsoft's with `-DDPLAY_MICROSOFT=ON` (`dplay_shim.c`). On Windows, if a
DirectPlay game ran earlier, Microsoft's helper `dplaysvr.exe` may still hold
UDP 47624; end it in Task Manager before hosting.

On non-Windows targets `dplay_native.c` is built instead of the COM shim. It
implements the same `dplay_shim.h` API by speaking the DirectPlay 4 Winsock
protocol ([MC-DPL4CS]) over plain sockets. DirectPlay is now OpenJKDF2's only
multiplayer backend (GameNetworkingSockets and the "basic sockets" backend were
removed); targets that can't run it build the stub backend.

It can join games hosted by JK.EXE and host games JK.EXE can join. As host it
is DirectPlay's name server: it answers session searches on UDP 47624, hands
out player IDs (session key XOR uniqueness/slot, as JK.EXE does), checks the
password, introduces each new machine to the others (ADDFORWARD, waiting for
their ADDFORWARDACKs) before sending it the full player list, pings silent
machines over UDP every 15 s and drops a machine after 8 unanswered pings.
The join screen's address box is also searched directly (IP or host name), so
games outside the LAN/VPN broadcast range can be found.

Ports: TCP 2300 (system messages; next free up to 2349) and UDP 2350 (game
data; up to 2400), same as Windows DirectPlay. Session search goes to UDP
47624 by broadcast on every interface (Radmin's included) and, optionally,
directly to hosts listed in `OPENJKDF2_DPLAY_HOSTS=ip,ip`.
`OPENJKDF2_DPLAY_LOG=1` logs every DirectPlay message.

Checked against captures of a real 3-player JK.EXE session:

- `ADDFORWARDREQUEST` and `CREATEPLAYER` built by the native client are
  byte-identical to JK.EXE's (`src/Tests/dplay_native_test.c`).
- Session replies, the full player list (`SUPERENUMPLAYERSREPLY`) and the
  host's `ADDFORWARD` introductions parse to the right IDs, names and addresses.
- End to end against a replay host: search → join → player list → create
  player → receive game data → leave (game player, then system player).

Seen live since: keepalive `PING`/`PINGREPLY` (0x16/0x17; a JK.EXE host pings
over UDP). Host replies `ENUMSESSIONSREPLY`, `REQUESTPLAYERREPLY` and
`ADDFORWARD` are byte-identical to a JK.EXE host's (offline tests); host plus
two clients verified end to end on one machine (join, introductions, full-mesh
data, password refusal, leave).

Not yet seen in a capture (implemented from the spec, logged when they occur):
join refusal (0x24), `YOUAREDEAD`, and `SESSIONDESCCHANGED` (not sent by the
native host yet: joined players don't get map/flag updates pushed; new
searches always see the current description).

### IPX (IPXWrapper)

The "IPX Wrapper Connection for DirectPlay" connection plays with JK.EXE players who use
[IPXWrapper](https://github.com/solemnwarning/ipxwrapper) and pick DirectPlay's
IPX connection. Newer Windows dropped IPX and DirectPlay's IPX provider;
IPXWrapper brings both back by carrying IPX over UDP and installing its own
DirectPlay IPX provider (`dplay-setup.exe`). We speak both directly, so
OpenJKDF2 players need nothing installed, on any platform (IPXWrapper itself
is a 32-bit Windows DLL and couldn't load into our 64-bit build anyway).
The DirectPlay messages and the game data are exactly those of TCP/IP; only
what carries them differs. From IPXWrapper's source (0.7.x) and captures of
JK.EXE + IPXWrapper over Radmin (2026-10-22):

- Each IPX packet is one UDP datagram: a 27-byte header (u8 type 0,
  destination net(4) node(6) socket(2), source the same, u16 data size; all
  big-endian), then the data. Everyone listens on UDP 54792. A packet for an
  IPX address whose IP isn't known yet is broadcast to every interface's
  subnet; replies come straight from the sender's UDP port, which IPXWrapper
  remembers per IPX address. IPXWrapper drops packets from outside its own
  subnets, so everyone has to share a LAN or VPN; typing an IP over the
  internet doesn't work with IPXWrapper players.
- Addresses: network 1, a random node (`02xxxxxxxxxx`), socket 0x0400 (the
  first one IPXWrapper hands out). Session searches go to node ffffffffffff,
  socket 42367, and are answered from the host's socket 0x0400.
- DirectPlay messages carry no SP header (they start at `play`). Each
  player's SP data is a 14-byte sockaddr_ipx (family 6, net, node, socket),
  filled in for everyone, unlike TCP/IP's zero own address.
- Searches, their replies and pings go as they are. Every other DirectPlay
  message goes through DirectPlay's reliable layer: `PACKET2_DATA` (0x30:
  message GUID, PacketIndex, DataSize, Offset, TotalPackets, MessageSize,
  PackedOffset 0, data) answered by `PACKET2_ACK` (0x31: GUID, PacketIndex).
  JK.EXE resent unacknowledged ones after ~100 ms; we resend every 250 ms for
  ~6 s, acknowledge every copy and deliver each message once. Messages over
  976 bytes are split (the provider's buffer is 1024); none were seen split.
- Game data: `idFrom idTo payload`, as over TCP/IP's UDP.

Built from the same code as TCP/IP: each IPX peer gets a handle that stands in
for an IPv4 address, and the send/receive functions underneath pick IPX or
TCP/IP. Verified: our `CREATEPLAYER` and `ADDFORWARDREQUEST` are
byte-identical to JK.EXE's, acks match, and a JK.EXE host's search reply and
player list parse (`dplay_native_test.c`); OpenJKDF2 host and joiner end to
end through broadcasts and the reliable layer (two network namespaces). Not
yet seen in a capture: split messages.

Live against JK.EXE + IPXWrapper over Radmin (2026-10-22): joining works,
including a 3-machine game whose third machine was only reachable by
broadcast at first. Learned there:

- Radmin VPN's Linux interface has no broadcast address set, and getifaddrs
  then reports the interface's own IP as its broadcast address; the broadcast
  address is worked out from the netmask instead (`dpn_Broadcasts`, also used
  by TCP/IP searches).
- When two machines share a LAN and Radmin, IPXWrapper may answer over the
  LAN, yet take packets for its IPX address (its Radmin adapter's MAC) only
  from the Radmin subnet. Resends of guaranteed messages therefore also go to
  every subnet; the copy that arrives the right way gets through.
- During the stats screen between levels the game doesn't receive; with
  nobody acknowledging, a JK.EXE peer gave up on us. `dpn_Idle` keeps the IPX
  side serviced while menus are up (`dps_Idle`).

### Trying it without building the game

    cc -std=gnu11 -O2 -DPLATFORM_DPLAY -DDPLAY_NATIVE -Isrc/Platform/Networking/DPlay \
       src/Tests/dplay_probe.c src/Platform/Networking/DPlay/dplay_native.c -o dplay_probe
    ./dplay_probe list
    OPENJKDF2_DPLAY_LOG=1 ./dplay_probe join 0 probe     # Ctrl-C leaves cleanly
    OPENJKDF2_DPLAY_LOG=1 ./dplay_probe host hostbot [password]
    # PROBE_SEND=1 makes either side send a DSS_PING to everyone each second
    # PROBE_IPX=1 uses the IPX connection (UDP 54792, one copy per machine)

The probe joins at the DirectPlay level only and never sends JK's join
request, so it doesn't spawn; the host just sees a DirectPlay player come and go.

Offline tests: build `src/Tests/dplay_native_test.c` with `-DDPLAY_NATIVE_TESTS`
and run it on `src/Tests/data/dplay` (messages extracted from captures of real
JK.EXE games, with the addresses, one player name and the password replaced by placeholders).

## Logging

`OPENJKDF2_DPLAY_LOG=1` makes every transport log its traffic to stderr.
`OPENJKDF2_DPLAY_LOGFILE=<path>` appends that log to a file instead, which is
the easy way on Windows (PowerShell: `$env:OPENJKDF2_DPLAY_LOG=1;
$env:OPENJKDF2_DPLAY_LOGFILE="dplay.log"; .\openjkdf2-64.exe`).
