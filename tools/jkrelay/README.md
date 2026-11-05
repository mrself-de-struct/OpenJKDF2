# jkrelay: server for "Imperial Network Protocol"

"Imperial Network Protocol" is a multiplayer connection type for
OpenJKDF2 players only. Every player makes one outbound UDP connection to this
server, which lists games and forwards game traffic between the players of a
session. **Only the server needs a reachable port**; players behind any kind
of NAT (home routers, CGNAT, mobile) can host and join without touching their
routers.

The game's own messages are carried unchanged, so gameplay is the same as a
DirectPlay game. To play with people using the original JK.EXE, pick the
DirectPlay connection instead (that one needs reachable ports or a VPN).

## Ready-to-run binaries

`build_static.sh` builds fully static binaries (musl libc, baseline CPU) for
x86-64 and ARM64 Linux, using Zig as the compiler (`pip install ziglang`, then
`ZIG="python3 -m ziglang" ./build_static.sh`). They run on any 64-bit Linux of
that architecture, with no glibc-version or newer-CPU requirements, so they're
what to hand to people who can't build it themselves.

## Build and run

    cc -O2 -o jkrelay_server tools/jkrelay/jkrelay_server.c -Isrc/Platform/Networking/DPlay
    ./jkrelay_server            # UDP 27900
    ./jkrelay_server -p 27950 -v   # other port, log every player
    ./jkrelay_server -l jkrelay.log   # log to a file

Open/forward **UDP 27900** (or your port) to this machine, e.g.

    sudo firewall-cmd --permanent --add-port=27900/udp && sudo firewall-cmd --reload
    # or: sudo ufw allow 27900/udp

Log lines show games being hosted, joins, leaves, wrong passwords and
timeouts, so "I can't connect" questions start here. They go to stdout, or
to a file with `-l`:

    ./jkrelay_server -l /var/log/jkrelay/jkrelay.log

The file is appended to, one line per event, written immediately. `SIGHUP`
makes the server reopen it, which is what logrotate needs
(`/etc/logrotate.d/jkrelay`):

    /var/log/jkrelay/jkrelay.log {
        weekly
        rotate 8
        compress
        delaycompress
        missingok
        notifempty
        postrotate
            systemctl kill -s HUP jkrelay.service
        endscript
    }

### systemd

`jkrelay.service`:

    [Unit]
    Description=Imperial Network Protocol relay server
    After=network-online.target
    Wants=network-online.target

    [Service]
    ExecStart=/usr/local/bin/jkrelay_server -l /var/log/jkrelay/jkrelay.log
    DynamicUser=yes
    LogsDirectory=jkrelay
    Restart=on-failure

    [Install]
    WantedBy=multi-user.target

## Browsers (WebSocket)

Browsers can't send UDP, so the relay also takes players over WebSocket:
`jkrelay_server -w 27901` listens on TCP 27901 (ws://), one protocol packet per
binary message. Everything else is the same: browser and desktop players share
games, bans and the game list. Pages served over https need wss://, so put a TLS
reverse proxy on the same machine in front of it; it passes the player's address in
`X-Forwarded-For`, which the relay uses for connections from this machine (bans and
logs show the player, not the proxy).

Caddy (WebSocket and X-Forwarded-For are automatic):

```
relay.example.com {
    reverse_proxy /imperial 127.0.0.1:27901
}
```

nginx:

```
location /imperial {
    proxy_pass http://127.0.0.1:27901;
    proxy_http_version 1.1;
    proxy_set_header Upgrade $http_upgrade;
    proxy_set_header Connection "upgrade";
    proxy_set_header X-Forwarded-For $remote_addr;
    proxy_read_timeout 3600s;
}
```

Browsers then connect to `wss://relay.example.com/imperial`. Port 27901 itself needn't be
open to the internet (only the proxy uses it). Browser players don't get direct links.

Dedicated servers on their own server (listed here as a master server) can't be
reached by browsers at their own UDP address, so the relay is their gateway: a
browser opens `wss://relay.example.com/imperial?to=<server ip>:<port>` and the relay
passes each message on to that server as `JKR_VIA` (with the browser's address) and
its answers back over the WebSocket. Only servers listed here that answered the
reachability check can be named; anything else gets a 404. Servers take `JKR_VIA`
only from their master, so it needs nothing on the dedicated server's side but
`master` pointing at this relay (the default, master.jkjunkie.com) and a build from
2026-10-29 or later. Their rcon and `servermaps` go the same way. Older relays don't
say they're a gateway (`JKR_LR_GATEWAY`), and browsers then leave those servers out
of the list.

## Game list as JSON

The WebSocket port also answers a plain `GET` whose path ends in `/list` (or
`/list.json`) with the games, for web pages that list them (no WebSocket
needed): the games hosted here and the dedicated servers listed here that
answered, as a Join screen sees them. CORS is open (`Access-Control-Allow-Origin: *`),
`Cache-Control: max-age=5`.

```json
{"version":1,"games":[{"guid":"{...}","app":"{BF0613C0-DE79-11D0-99C9-00A02476AD4B}",
  "name":"Coruscant","episode":"JK1MP","map":"m10.jkl","players":1,"maxPlayers":32,
  "password":false,"maxRank":4,"tickRateMs":50,"modeFlags":144,"hitRules":"classic",
  "sharesFiles":true,"openjkdf2Layouts":true,"dedicatedServer":true,
  "address":"203.0.113.10:27960","mod":"JK_Vanilla_Fixed","remasterPack":false,"tail":"4a4b4d44..."}],
 "total":1,"shown":1}
```

- `app`: the game's DirectPlay application GUID (`{BF0613C0-...}` is Jedi Knight).
- `players`/`maxPlayers` count a dedicated server's own slot, as JK does (31 can join a 32).
- `dedicatedServer`: true for a dedicated server on its own server (listed here, with its
  `address`); null for games hosted on the relay (the listing can't tell). `nextToRelay`:
  listed from the relay's own machine or LAN (`address` null; players reach it at the relay's).
- `mod`: the host's mod by name, null for none; `tail`: the raw mods.js tail.
- `hitRules`: `classic` or `hwys` (Hit what you see).

Behind Apache, give it its own line above the WebSocket one, since `ws://` lines only
carry WebSockets:

```
ProxyPass        /imperial/list http://127.0.0.1:27901/list
ProxyPassReverse /imperial/list http://127.0.0.1:27901/list
```

## Players

1. Multiplayer -> pick **Imperial Network Protocol**.
2. On the join screen, type the server's address (`host` or `host:port`) in
   the address box. It's remembered (`relayServer` in OpenJKDF2's settings)
   and also used when hosting. `OPENJKDF2_RELAY=host[:port]` overrides it.
3. Host or join as usual.

`OPENJKDF2_DPLAY_LOG=1` makes the game log relay traffic to stderr.

## Direct links

When the host and a joiner both have `net_directLinks 1` (the default), the
server introduces them and they open a direct connection through their routers
(UDP hole punching, from the same socket they use for the server). Their game
traffic then skips the server: one trip host <-> joiner instead of two. A
joiner with a link sends everything to the host, which passes it on to the
other players, so joiners only ever learn the host's address, never each
other's. When punching fails (CGNAT, strict NATs) or a link goes quiet, that
player's traffic goes through the server as before, automatically.

`net_directLinks 0` (console or `openjkdf2_cvars.json`) keeps a player on the
server only, so nobody in the game sees their IP address. The game console's
`gameinfo` says which way packets go.

Needs a server and both players on builds from 2026-10-27 or later; anyone
older just plays through the server. Nothing changes on the server's port.

## Restarting the server

Games survive a server restart (updating it, a crash, a reboot). The server
keeps nothing on disk: when it comes back, it answers players it doesn't know
with "who are you?", and their games put themselves back, with the same game
name, password and player ids, so the game itself never notices. Hosts first;
joiners that ask before their host is back keep asking.

- While the server is down, players on a direct link to the host keep playing;
  the others freeze until it's back (a few seconds for `systemctl restart`).
- A host waits for the server as long as it takes (a dedicated server shows up
  in the list again on its own). A joiner gives up after 60 s, unless it still
  has a direct link to the host.
- Players who don't come back within 30 s of their host are dropped from the game.
- The game's log says "the relay server lost this game (restarted?); putting it
  back" and "game back on relay server ...".

Needs the server and the players on builds from 2026-10-27 (resuming) or later.
Older players lose the game as before; older servers never ask, so a new host
on an old server just keeps its game, unlisted, after a restart.

## Dedicated servers without a relay

A dedicated server has this server built in: started without `-relayServer`,
it runs its own copy on UDP 27960 (`-port <n>` for another) and hosts its game
on it. Players then join it directly, from the game's console:

```
characters                  lists your multiplayer characters: name, model, force level
character <name or number>  picks one (names with spaces work as typed; saved)
connect <host>[:port] [password]
disconnect                  leaves the game for the main menu (as Esc -> Abort)
reconnect                   joins the last game you were in again
```

`reconnect` works for any Imperial Network Protocol game (joined with
`connect` or from the join screen): it joins the same game again, or after a
server restart the server's only game or the one with the same name. The
server's address is remembered across restarts; a game's password only until
the game is closed. Joining a game also makes that character the one
`connect` and `reconnect` use.

`connect` checks that the server answers and runs a game, and that the picked
character's force level is within the game's limit, then joins straight away
(from the main menu, the Multiplayer menu, or a game, which it leaves first).
With several games on the server (a public relay) it opens the join screen
instead. The relay address saved in the join screen isn't changed.

Only the dedicated server itself can host on its built-in copy, and it sets up
no direct links (everyone reaches it already). Open UDP 27960 (or your `-port`)
in the firewall. A dedicated server with `-relayServer <host>` hosts on that
relay as before.

### Kicks and bans

A kicked or banned player is told why: "You were kicked from the server." / "You were
banned from this server (for N minutes)." in their console when it happens, and in a
dialog when their menus come back. Joining while banned says "You are banned from this
server (for N more minutes)." Players with older builds see the game end as before.

### Voting

Players vote from the console (as in Quake 3): `callvote map <level or GOB:level>`,
`callvote kick <name or slot>`, `callvote rank <0-8>`, `callvote scorelimit <points>`,
`callvote timelimit <minutes>` (0 = none for the limits), then `vote yes` / `vote no`
(`vote 1` or more is yes, `vote 0` or less no). `servermaps [text]` lists the server's
levels (the server's `maplist`, which answers without the rcon password). A vote runs
30 s and passes as soon as more than half the players in the game voted yes, fails as
soon as half voted no, or fails when its time is up (Quake 3's rule); the caller's vote
counts as yes. One vote at a time, at most 3 called per player per level; a new level
cancels a running vote. The votes reach the server as chat for it alone ("!callvote ..."
/ "!vote ...", which a JK.EXE player can type in chat too), and the server announces
the vote, the count and the result to everyone. `cancelvote` (admin) ends a vote.

Which kinds of vote are allowed is the cvar `sv_allowVote` (in the server folder's
`openjkdf2_cvars.json`), one bit each as Urban Terror's `g_allowvote`: 1 map, 2 kick,
4 rank, 8 scorelimit, 16 timelimit; 31 (the default) all, 0 none. `rcon togglevote`
lists them with on/off; `rcon togglevote 2` switches kick votes on or off (values add
up: `togglevote 24` flips both limits). The change is saved.

### Master servers on the join screen

Players' games list asks the master servers in the global cvar `net_masterServers`
(default `master.jkjunkie.com`; several separated by commas or spaces, `host[:port]`,
port 27900 if none) as well as the relay server they host on, and lists every game
once: dedicated servers listed there (joined at their own address) and games hosted
on that relay server (joined through it). A master that doesn't answer is skipped for
30 s. The join screen has no address box for the Imperial Network Protocol any more;
`relayServer` (the host screen's box) is still where games are hosted, and still asked.
Probe: `PROBE_MASTERS=host[,host]`.

### Its settings: server.json

A dedicated server keeps its hosting settings in `server.json` in its folder,
or in the file `-serverConfig <file>` names (a path from the launch folder, no
spaces), so several servers can run from one folder, e.g.
`openjkdf2 -dedicatedServer -headless -serverConfig ctf.json -logFile ctf.log`
with its own `port` in `ctf.json` (the first run moves the settings there from `registry.json`): `gameName`,
`serverPassword`, `serverEpisodeGob`, `serverMapJkl`, `maxPlayers` (with the
server's own slot), `maxRank`, `timeLimit`, `scoreLimit`, `tickRate`, the game
flags, and where it hosts:

| Key | Command line | Meaning |
|---|---|---|
| `port` | `-port <n>` | UDP port of its own server (27960) |
| `master` | `-master <host[:port]>` | relay server to list it on ("" = not listed) |
| `relayServer` | `-relayServer <host>` | host on that relay instead of its own server |
| `connection` | `-connection <kind>` | relay (default), web, tcpip, ipx |
| `maps` | | map rotation, e.g. `["m2", "JK1CTF:c1", "PermaConc Oasis:permaconcoasis"]`: `level` or `GOB:level`, from any episode GOB in `Episode` (a bare level: `serverEpisodeGob` first, then the running episode, then every GOB). Use the array form for GOB names with spaces |
| `shuffleMaps` | | play `maps` in random order instead (never the same map twice in a row) |
| `rconPassword` | | turns rcon on (see below); "" = off |
| `bans` | | banned IP addresses, e.g. `["1.2.3.4"]` (`ban`/`unban` keep it up to date) |

The command line wins over `server.json`. Players' own settings stay in
`registry.json`.

`timeLimit` is in milliseconds (30 minutes = 1800000; the server warns about
anything under a minute), used when `bUseTimeLimit` is true; `scoreLimit` with
`bUseScoreLimit`. When a level ends, a dedicated server goes on with the next
map of `maps` (from the first one, unless the command line names a map; maps
that aren't found are skipped, logged), switching episodes as needed, or without a list the episode's levels
in turn, round and round, or with `bIsSingleLevel` the same level again (the
game doesn't end). Players need a build from 2026-10-27 or later to follow a
`maps` rotation and to stay in a single-level game when it starts over, and the
episode GOBs the rotation uses (a player without one gets "The server went on to
<level> (<GOB>), which you don't have." and leaves). CTF levels (JK1CTF) clear the
time and score limits themselves (`ctf_main.cog`), so they end by their own rules.

Stopping a dedicated server (Ctrl+C, `systemctl stop`) closes its game properly,
so its players hear of it at once; a second Ctrl+C stops it on the spot.

### Admin commands and rcon

Typed at the dedicated server's terminal, or sent from anywhere with rcon:

| Command | Does |
|---|---|
| `status` | game, map, time left, limits, next map, players (slot, score, name, address) |
| `kick <slot or name>` | takes a player out of the game (at the server too: a patched JK.EXE can't ignore it) |
| `ban <slot, name or IP> [minutes]` | kick and keep them out (no minutes: for good, saved in `bans`) |
| `unban <IP>`, `bans` | |
| `say <message>` | "Server: message" to everyone |
| `map <level>` or `map <GOB>:<level>` | that level now (after the usual 10 s end of level), from another episode too |
| `nextmap [<level> or <GOB>:<level>]` | after this level (or shows it) |
| `maplist [<text>]` | every level in the server's `Episode` GOBs, `title \| level \| GOB`; with text, those whose title, level or GOB contain it |
| `restart` | this level again |
| `timelimit <minutes>`, `scorelimit <points>` | for this game; 0 = none (server.json keeps the starting ones) |
| `password [<password>]` | for players joining from now on; none = no password |
| `rank [0-8]` (or `maxrank`) | the highest force rank a joining character may have (0 Uninitiated ... 8 Jedi Lord), for this game; players already in keep theirs; no number: shows it |
| `cancelvote` | ends the vote in progress |
| `togglevote [<value>]` | lists the kinds of vote and their `sv_allowVote` bits, or switches one on or off |

Rcon (like Quake 3's): set `rconPassword` in `server.json`, then in the game's
console

```
rcon_password <password>     (until the game quits)
rcon_address <host[:port]>   (default: the server of the game you're in or were last in)
rcon <command>               e.g. rcon status, rcon kick 3, rcon map m10
```

The password itself never crosses the network: the server sends a fresh
challenge and the game answers with an HMAC-SHA256 of it and the command. After
5 wrong passwords an address is ignored for 5 minutes; everything is logged
(`[rcon]`, `[admin]`). Rcon reaches dedicated servers running their own server
(UDP 27960), even when nobody's in the game. From a shell:
`dplay_probe rcon <host[:port]> <password> <command>` (src/Tests/dplay_probe.c).

## Master server

A relay server is also a master server: dedicated servers running their own
server list themselves on it (`-master relay.example.com` or `"master"`), and its
game list then shows them next to the games hosted on it. Picking one on the
join screen joins it at its own address. Anyone can list a server.

- The dedicated server announces its game (name, map, players, limits) every
  10 s and when it changes; a listing not renewed for 35 s is dropped.
- The relay server lists it at the address the announcement came from, and
  only once it answers there from a different socket than the one it
  announces to, so a server behind a closed firewall isn't listed. The server's
  log then says "master server ... can't reach this server: let UDP 27960 in".
- At most 4 listed servers per IP address.
- `connect <relay server>` also finds them (with one game listed, it joins it).
- Players need a build from 2026-10-27 or later to see listed servers; older
  ones see only the games hosted on the relay server.

## Notes

- Without a direct link, game traffic passes through the server, so latency is
  player -> server -> player. Put the server somewhere central to the group.
- Bandwidth is small (JK sends a few KB/s per player), so a homelab or a small
  VPS is plenty.
- Passwords are checked by the server. Traffic isn't encrypted, like
  DirectPlay's.
- Players time out after 20 s of silence; if the host leaves, everyone in the
  game gets "session lost", as with DirectPlay.
