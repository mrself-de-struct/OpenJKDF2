#!/usr/bin/env python3
"""Decode Jedi Knight DirectPlay traffic from a pcap.

Used to check that OpenJKDF2's PLATFORM_DPLAY backend behaves like JK.EXE 1.0:

  sessions   DirectPlay EnumSessions replies (session name, flags, dwUser1-4,
             decoded the way JK uses them)
  timeline   game (DSS) messages with names, reliable ids and acks
  reliable   reliable-message summary: sends, resends, time to ack
  summary    message counts per type and direction (good for diffing runs)

Usage:
  jk_dplay_pcap.py capture.pcap [--src-dir path/to/OpenJKDF2] [--host IP]
                   [--only sessions,timeline,reliable,summary]

Capture with e.g.:  tcpdump -ni <iface> -w out.pcap 'host <peer>'
Requires scapy (pip install scapy).
"""
import argparse
import collections
import os
import re
import struct
import sys

try:
    from scapy.all import rdpcap, IP, TCP, UDP
except ImportError:
    sys.exit("scapy is required: pip install scapy")

DP_SIG = b"play"

# DirectPlay session flags (dplay.h)
DPSESSION = {
    0x1: "NEWPLAYERSDISABLED", 0x4: "MIGRATEHOST", 0x8: "NOMESSAGEID",
    0x20: "JOINDISABLED", 0x40: "KEEPALIVE", 0x80: "NODATAMESSAGES",
    0x100: "SECURESERVER", 0x200: "PRIVATE", 0x400: "PASSWORDREQUIRED",
    0x800: "MULTICASTSERVER", 0x1000: "CLIENTSERVER", 0x2000: "DIRECTPLAYPROTOCOL",
    0x4000: "NOPRESERVEORDER", 0x8000: "OPTIMIZELATENCY",
}

JK_APP_GUID = bytes.fromhex("c01306bf79ded01199c900a02476ad4b")

# Fallback DSS ids (from OpenJKDF2 src/types_enums.h) if --src-dir isn't given.
DSS_FALLBACK = {
    1: "THINGPOS", 2: "CHAT", 3: "SECTORFLAGS", 4: "FIREPROJECTILE", 5: "DEATH",
    6: "DAMAGE", 7: "SETTHINGMODEL", 8: "SENDTRIGGER", 9: "PLAYKEY", 10: "PLAYSOUND",
    11: "SYNCTHING", 12: "THINGFULLDESC", 13: "SYNCCOG", 14: "SURFACESTATUS",
    15: "AISTATUS", 16: "INVENTORY", 17: "SURFACE", 18: "SECTORSTATUS",
    19: "PLAYKEYMODE", 20: "PATHMOVE", 21: "SYNCPUPPET", 22: "SYNCTHINGATTACHMENT",
    23: "SYNCEVENTS", 24: "SYNCCAMERAS", 25: "TAKEITEM1", 26: "TAKEITEM2",
    27: "STOPKEY", 28: "STOPSOUND", 29: "CREATETHING", 30: "SYNCPALEFFECTS",
    32: "LEAVEJOIN", 33: "WELCOME", 34: "JOINREQUEST", 35: "DESTROYTHING",
    36: "JOINING", 37: "PLAYSOUNDMODE", 38: "PING", 39: "PINGREPLY", 40: "RESET",
    41: "ENUMPLAYERS", 42: "QUIT", 48: "JKENABLESABER", 49: "SABERINFO3",
    53: "HUDTARGET", 55: "JKPRINTUNISTRING", 56: "ENDLEVEL", 57: "SABERINFO1",
    58: "SABERINFO2", 59: "JKSETWEAPONMESH", 60: "SETTEAM",
}


def load_dss_names(src_dir):
    if not src_dir:
        return dict(DSS_FALLBACK)
    names = {}
    path = os.path.join(src_dir, "src", "types_enums.h")
    in_enum = False
    for line in open(path, encoding="utf-8", errors="replace"):
        if "enum DSS_ID" in line:
            in_enum = True
            continue
        if in_enum:
            if line.strip().startswith("}"):
                break
            m = re.match(r"\s*DSS_(\w+)\s*=\s*(\d+)", line)
            if m:
                names[int(m.group(2))] = m.group(1)
    return names or dict(DSS_FALLBACK)


def fmt_guid(b):
    a, b1, c = struct.unpack("<IHH", b[:8])
    return "{%08X-%04X-%04X-%s-%s}" % (a, b1, c, b[8:10].hex().upper(), b[10:16].hex().upper())


def fmt_flags(v):
    parts = [n for bit, n in DPSESSION.items() if v & bit]
    return "0x%X (%s)" % (v, "|".join(parts) if parts else "none")


def utf16z(b, off):
    end = off
    while end + 1 < len(b) and b[end:end + 2] != b"\0\0":
        end += 2
    return b[off:end].decode("utf-16-le", errors="replace")


def parse_session_reply(pl):
    """DirectPlay EnumSessionsReply (command 1) carried over TCP."""
    sig = pl.find(DP_SIG)
    if sig < 0 or len(pl) < sig + 8:
        return None
    cmd, ver = struct.unpack_from("<HH", pl, sig + 4)
    if cmd != 1:
        return None
    sd = sig + 8
    if len(pl) < sd + 0x50:
        return None
    (size, flags) = struct.unpack_from("<II", pl, sd)
    inst = pl[sd + 8:sd + 24]
    app = pl[sd + 24:sd + 40]
    maxp, curp = struct.unpack_from("<II", pl, sd + 40)
    u1, u2, u3, u4 = struct.unpack_from("<IIII", pl, sd + 64)
    # Name follows the descriptor plus a 4-byte name-offset field.
    name = utf16z(pl, sd + 0x50 + 4)
    return dict(flags=flags, inst=inst, app=app, maxp=maxp, curp=curp,
                user=(u1, u2, u3, u4), name=name, ver=ver)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("pcap")
    ap.add_argument("--src-dir", help="OpenJKDF2 checkout, to read DSS ids from src/types_enums.h")
    ap.add_argument("--host", help="IP of the game host (labels direction H>C / C>H)")
    ap.add_argument("--only", default="sessions,reliable,summary",
                    help="comma list of: sessions,timeline,reliable,summary")
    args = ap.parse_args()
    want = set(args.only.split(","))
    names = load_dss_names(args.src_dir)

    pkts = rdpcap(args.pcap)
    if not pkts:
        sys.exit("empty capture")
    t0 = float(pkts[0].time)

    def direction(p):
        if not args.host:
            return "%s>%s" % (p[IP].src, p[IP].dst)
        return "H>C" if p[IP].src == args.host else "C>H"

    seen_sessions = set()
    rel = collections.defaultdict(list)    # (dir, msgId, name) -> [t...]
    acks = collections.defaultdict(list)   # (dir, msgId) -> [t...]
    counts = collections.Counter()
    data_ports = set()

    for p in pkts:
        if IP not in p:
            continue
        t = float(p.time) - t0

        if TCP in p and bytes(p[TCP].payload):
            pl = bytes(p[TCP].payload)
            s = parse_session_reply(pl)
            if s and "sessions" in want:
                key = (s["inst"], s["name"], s["flags"], s["user"], s["curp"], s["maxp"])
                if key not in seen_sessions:
                    seen_sessions.add(key)
                    u1, u2, u3, u4 = s["user"]
                    parts = s["name"].rsplit(":", 2)
                    print("[%9.3f] session from %s" % (t, p[IP].src))
                    print("    name        %r" % s["name"])
                    if len(parts) == 3:
                        print("                server=%r episode=%r map=%r" % tuple(parts))
                    print("    app guid    %s%s" % (fmt_guid(s["app"]), "  (JK)" if s["app"] == JK_APP_GUID else "  (NOT JK!)"))
                    print("    instance    %s" % fmt_guid(s["inst"]))
                    print("    flags       %s" % fmt_flags(s["flags"]))
                    print("    players     %d / %d" % (s["curp"], s["maxp"]))
                    print("    dwUser1     0x%08X  checksumSeed" % u1)
                    print("    dwUser2     0x%08X  multiModeFlags" % u2)
                    print("    dwUser3     0x%08X  field_E0(version)=%d  maxRank=%d" % (u3, u3 & 0xFFFF, u3 >> 16))
                    print("    dwUser4     0x%08X  tickRateMs=%d" % (u4, u4))
            continue

        if UDP not in p:
            continue
        pl = bytes(p[UDP].payload)
        # DirectPlay raw UDP data: [DPID from][DPID to][u16 cogMsgId][u16 msgId][payload]
        if len(pl) < 12 or DP_SIG in pl[:32]:
            continue
        cog, mid = struct.unpack_from("<HH", pl, 8)
        if cog not in names:
            continue
        data_ports.add(p[UDP].sport)
        d = direction(p)
        n = names[cog]
        counts[(d, n)] += 1
        if n == "RESET" and len(pl) >= 14:
            acks[(d, struct.unpack_from("<H", pl, 12)[0])].append(t)
        elif mid:
            rel[(d, mid, n)].append(t)
        if "timeline" in want and n != "THINGPOS":
            extra = " ack=%d" % struct.unpack_from("<H", pl, 12)[0] if n == "RESET" and len(pl) >= 14 else ""
            strs = [x.decode() for x in re.findall(rb"[ -~]{4,}", pl[12:])][:4]
            print("[%9.3f] %-24s %-20s id=%-5d len=%-4d%s %s" % (t, d, n, mid, len(pl), extra, strs if strs else ""))

    if "reliable" in want and rel:
        print("\nReliable messages (resends = sends-1; ack = time from first send):")
        slow = 0
        for (d, mid, n), ts in sorted(rel.items(), key=lambda kv: kv[1][0]):
            other = [k for k in acks if k[1] == mid and k[0] != d]
            a = sorted(x for k in other for x in acks[k] if x >= ts[0])
            ack = (a[0] - ts[0]) if a else None
            if len(ts) > 1 or ack is None or ack > 0.5:
                slow += 1
                print("  [%9.3f] %-24s %-20s id=%-5d sends=%d ack=%s" % (
                    ts[0], d, n, mid, len(ts), "%.3fs" % ack if ack is not None else "NONE"))
        print("  %d of %d reliable messages needed resends or were slow/unacked" % (slow, len(rel)))

    if "summary" in want and counts:
        print("\nMessage counts (UDP data ports seen: %s):" % sorted(data_ports))
        for (d, n), c in sorted(counts.items()):
            print("  %-24s %-20s %6d" % (d, n, c))


if __name__ == "__main__":
    main()
