#!/usr/bin/env python3
"""
P2c/P3c verification: confirm the pcap files a gptp-spike/nominal/congestion/
feedback run wrote (with --pcapPrefix) contain REAL, PARSEABLE gPTP frames in the
byte-exact IEEE 802.1AS wire format (P3c) -- not just that a file exists.

Mirrors the spirit of scripts/check_pcap_replay.py (prove the capture is genuine,
content-verifiably). Since P3c the on-wire format is byte-exact 802.1AS, so the
authoritative correctness check is now `tshark -r <f> -Y ptp` (Wireshark's own
unmodified PTPv2 dissector) -- see ns3/gptp/WIRE_FORMAT.md for the verified
transcript. This script remains a standalone, dependency-free cross-check (no
scapy / no tshark needed) that:

  1. parses each *.pcap's global + per-record headers (both endiannesses),
  2. for every Ethernet frame with the real PTP ethertype 0x88F7, reads the
     messageType = LOW NIBBLE of the first PTP payload byte (frame[14] & 0x0F;
     the high nibble is transportSpecific = 0x1) and tallies it,
  3. asserts genuine gPTP traffic was captured with the 2-step message types
     (Pdelay_Resp_Follow_Up, Follow_Up) present, which a 1-step capture could not
     contain. That is the "real, parseable, expected message types" check the C1
     pcap milestone did for the OMNeT++ path.

Usage:
    check_pcap_gptp.py <dir-or-pcap> [<more> ...]
"""
import glob
import os
import struct
import sys

GPTP_ETHERTYPE = 0x88F7  # real IEEE-assigned PTP EtherType (P3c)
# GptpMsgType = real IEEE 1588-2008 messageType nibbles (see ns3/gptp/gptp.h).
MSG_NAMES = {
    0x0: "Sync",
    0x2: "PdelayReq",
    0x3: "PdelayResp",
    0x8: "FollowUp",             # 2-step
    0xA: "PdelayRespFollowUp",   # 2-step
    0xB: "Announce",             # P3c, additive
}
TWO_STEP_TYPES = {0x8, 0xA}  # message types that only exist in the 2-step framing


def iter_pcap_frames(path):
    """Yield each captured frame's raw bytes from a classic .pcap file."""
    with open(path, "rb") as f:
        gh = f.read(24)
        if len(gh) < 24:
            raise ValueError(f"{path}: truncated global header")
        magic = gh[:4]
        if magic in (b"\xa1\xb2\xc3\xd4", b"\xa1\xb2\x3c\x4d"):
            endian = ">"
        elif magic in (b"\xd4\xc3\xb2\xa1", b"\x4d\x3c\xb2\xa1"):
            endian = "<"
        else:
            raise ValueError(f"{path}: not a pcap file (magic {magic!r})")
        linktype = struct.unpack(endian + "I", gh[20:24])[0]
        if linktype != 1:  # DLT_EN10MB (Ethernet)
            raise ValueError(f"{path}: unexpected linktype {linktype} (want 1=Ethernet)")
        while True:
            rec = f.read(16)
            if len(rec) < 16:
                break
            incl_len = struct.unpack(endian + "I", rec[8:12])[0]
            data = f.read(incl_len)
            if len(data) < incl_len:
                break
            yield data


def scan(paths):
    files = []
    for p in paths:
        if os.path.isdir(p):
            files.extend(sorted(glob.glob(os.path.join(p, "*.pcap"))))
        else:
            files.append(p)
    if not files:
        print("[check_pcap_gptp] FAIL: no .pcap files found", file=sys.stderr)
        return 1

    totals = {}
    total_frames = 0
    gptp_frames = 0
    nonempty_files = 0
    for path in files:
        if os.path.getsize(path) <= 24:  # header only, no records
            print(f"[check_pcap_gptp] WARN: {os.path.basename(path)} is empty")
            continue
        nonempty_files += 1
        for frame in iter_pcap_frames(path):
            total_frames += 1
            if len(frame) < 15:
                continue
            ethertype = struct.unpack(">H", frame[12:14])[0]
            if ethertype != GPTP_ETHERTYPE:
                continue
            gptp_frames += 1
            mtype = frame[14] & 0x0F  # messageType = low nibble (high = transportSpecific)
            totals[mtype] = totals.get(mtype, 0) + 1

    print(f"[check_pcap_gptp] scanned {len(files)} file(s), "
          f"{nonempty_files} non-empty, {total_frames} frames total")
    print(f"[check_pcap_gptp] gPTP frames (ethertype 0x{GPTP_ETHERTYPE:04x}): {gptp_frames}")
    for mtype in sorted(totals):
        name = MSG_NAMES.get(mtype, f"UNKNOWN({mtype})")
        print(f"    type {mtype} {name:<20} : {totals[mtype]}")

    # Gate the way check_pcap_replay.py gates: a real, content-verifiable result.
    if nonempty_files == 0:
        print("[check_pcap_gptp] FAIL: every capture was empty", file=sys.stderr)
        return 1
    if gptp_frames == 0:
        print("[check_pcap_gptp] FAIL: no parseable gPTP frames in any capture",
              file=sys.stderr)
        return 1
    unknown = [m for m in totals if m not in MSG_NAMES]
    if unknown:
        print(f"[check_pcap_gptp] FAIL: unparseable gPTP message types {unknown}",
              file=sys.stderr)
        return 1
    present_two_step = TWO_STEP_TYPES & set(totals)
    if not present_two_step:
        print("[check_pcap_gptp] FAIL: no 2-step (Follow_Up) frames -- capture does "
              "not reflect the P2b 2-step framing", file=sys.stderr)
        return 1
    print(f"[check_pcap_gptp] PASS: genuine, parseable gPTP capture with 2-step "
          f"framing (types present: {sorted(totals)})")
    return 0


if __name__ == "__main__":
    raise SystemExit(scan(sys.argv[1:] or ["."]))
