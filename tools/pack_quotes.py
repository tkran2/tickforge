"""Convert MicropriceLab observation CSV to the *local* TKF1 replay format.

Sequence numbers are renumbered local transport IDs, never venue sequence IDs.
Prices and sizes are rounded to 1e-8 using Decimal, matching source precision.
"""
import argparse
import csv
from decimal import Decimal, ROUND_HALF_EVEN
import json
from pathlib import Path
import struct
import zlib

def fixed(value):
    return int((Decimal(value) * 100000000).to_integral_value(rounding=ROUND_HALF_EVEN))

def pack(source, output):
    last_segment, generation, sequence, count, last_time = None, 0, 0, 0, -1
    with open(source, newline="") as file, open(output, "wb") as out:
        for row in csv.DictReader(file):
            timestamp = int(row["available_ns"])
            if timestamp <= last_time: raise ValueError("input must be chronological")
            last_time = timestamp
            if row["segment"] != last_segment:
                generation += 1; sequence = 0; kind = 1; last_segment = row["segment"]
            else: sequence += 1; kind = 2
            bid, ask, bsz, asz = (fixed(row[name]) for name in ("bid", "ask", "bid_size", "ask_size"))
            if not (0 < bid < ask < 2**63 and 0 < bsz < 2**63 and 0 < asz < 2**63): raise ValueError("invalid BBO")
            prefix = struct.pack(">4sB3x6Q", b"TKF1", kind, generation, sequence, bid, ask, bsz, asz)
            out.write(prefix + struct.pack(">I4x", zlib.crc32(prefix)))
            count += 1
    metadata = {"input": Path(source).name, "messages": count, "generations": generation,
                "format": "TKF1 local replay; not a venue protocol", "price_and_size_scale": 100000000}
    Path(str(output) + ".json").write_text(json.dumps(metadata, indent=2) + "\n")
    return metadata

if __name__ == "__main__":
    p = argparse.ArgumentParser(); p.add_argument("input"); p.add_argument("output"); args = p.parse_args()
    print(json.dumps(pack(args.input, args.output), indent=2))
