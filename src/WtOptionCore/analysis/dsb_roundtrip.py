#!/usr/bin/env python3
"""P1 follow-up (6.5): dsb <-> qstream round-trip validator.

Proves qstream_to_dsb.py produces reader-compatible dsb blocks AND that a
recorded/real dsb can be losslessly flattened to the qstream CSV shape:

  real dsb  --dsb_dump--> qstream CSV --qstream_to_dsb--> new dsb
  assert: tick count equal, L1 bid/ask equal at every index.

Usage:
  python3 dsb_roundtrip.py <real.dsb> <workdir>
"""
import csv
import os
import struct
import sys
import zstandard

TICK = struct.Struct(
    "<16s32s" + "d" * 13 + "I" * 4 + "d" * 3 + "d" * 40)
HDR = struct.Struct("<8sHHQ")     # V2 (compressed files)
HDR_RAW = struct.Struct("<8sHH")   # RAW files carry the 12-byte legacy header
BLK_FLAG = b"&^%$#@!\x00"


def read_dsb(path):
    with open(path, "rb") as fh:
        raw = fh.read()
    flag, btype, ver = HDR_RAW.unpack_from(raw, 0)
    assert flag == BLK_FLAG, "bad block flag"
    if ver == 0x04:   # CMP_V2 (20-byte header + zstd payload)
        _, _, _, size = HDR.unpack_from(raw, 0)
        payload = zstandard.ZstdDecompressor().decompress(raw[HDR.size:],
                                                          max_output_size=1 << 28)
    elif ver == 0x03:   # RAW_V2 (12-byte header + raw payload)
        payload = raw[HDR_RAW.size:]
    else:
        raise SystemExit(f"unsupported block version {ver}")
    n = len(payload) // TICK.size
    rows = []
    for i in range(n):
        f = TICK.unpack_from(payload, i * TICK.size)
        rows.append({
            "exchg": f[0].split(b"\x00")[0].decode(),
            "code": f[1].split(b"\x00")[0].decode(),
            "price": f[2],
            "trading_date": f[16],
            "action_time": f[18],
            "bid": f[22], "ask": f[32],
            "bidq": f[42], "askq": f[52],
        })
    return rows


def main():
    src, work = sys.argv[1], sys.argv[2]
    os.makedirs(work, exist_ok=True)
    rows = read_dsb(src)
    assert rows, "no ticks in source dsb"
    code, exchg, tdate = rows[0]["code"], rows[0]["exchg"], rows[0]["trading_date"]

    # dsb -> qstream CSV (ts from action_time HHMMSSmmm on the trading date;
    # the exact epoch is irrelevant for the round-trip identity check)
    qpath = os.path.join(work, f"qstream_{tdate}.csv")
    with open(qpath, "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["ts", "code", "bid", "ask", "last", "iv"])
        base = 1700000000
        for i, r in enumerate(rows):
            w.writerow([base + i, f"{exchg}.{r['code']}", r["bid"], r["ask"],
                        r["price"], 0.0])

    # qstream -> new dsb
    out_root = os.path.join(work, "conv")
    rc = os.system(f"{sys.executable} {os.path.dirname(os.path.abspath(__file__))}"
                   f"/qstream_to_dsb.py {qpath} {out_root} {exchg}")
    assert rc == 0, "converter failed"

    new_path = os.path.join(out_root, "his", "ticks", exchg, str(tdate),
                            f"{code}.dsb")
    rows2 = read_dsb(new_path)

    assert len(rows) == len(rows2), f"tick count {len(rows)} != {len(rows2)}"
    bad = sum(1 for a, b in zip(rows, rows2)
              if abs(a["bid"] - b["bid"]) > 1e-9 or abs(a["ask"] - b["ask"]) > 1e-9)
    print(f"src ticks={len(rows)} conv ticks={len(rows2)} px mismatches={bad}")
    print("PASS" if bad == 0 else "FAIL")
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
