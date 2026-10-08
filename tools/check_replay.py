"""Independent struct/zlib oracle for a valid TKF1 replay and its C++ digest."""
import argparse
import json
from pathlib import Path
import struct
import subprocess
import zlib

def reference(path):
    generation, sequence, count, checksum = 0, 0, 0, 14695981039346656037
    with open(path, "rb") as file:
        while frame := file.read(64):
            if len(frame) != 64: raise ValueError("truncated frame")
            if frame[5:8] != bytes(3) or frame[60:] != bytes(4): raise ValueError("reserved bits")
            magic, kind, gen, seq, bid, ask, bsz, asz = struct.unpack(">4sB3x6Q", frame[:56])
            if magic != b"TKF1" or zlib.crc32(frame[:56]) != struct.unpack(">I", frame[56:60])[0]:
                raise ValueError("magic or CRC mismatch")
            if gen > generation:
                if kind != 1 or seq != 0: raise ValueError("generation must begin with snapshot zero")
            elif gen != generation or seq != sequence + 1 or kind == 1:
                raise ValueError("invalid sequence")
            if kind not in (1, 2, 3) or not gen: raise ValueError("kind/generation")
            generation, sequence = gen, seq
            if kind == 3:
                if any((bid, ask, bsz, asz)): raise ValueError("invalid heartbeat")
                continue
            if not (0 < bid < ask < 2**63 and 0 < bsz < 2**63 and 0 < asz < 2**63): raise ValueError("invalid BBO")
            for byte in struct.pack("<6Q", gen, seq, bid, ask, bsz, asz):
                checksum = ((checksum ^ byte) * 1099511628211) & (2**64 - 1)
            count += 1
    return {"publications": count, "digest": checksum}

if __name__ == "__main__":
    p = argparse.ArgumentParser(); p.add_argument("file"); p.add_argument("--binary", default="build/tickforge"); args = p.parse_args()
    expected = reference(args.file)
    actual = json.loads(subprocess.check_output([str(Path(args.binary).resolve()), "replay", args.file]))
    if any(actual[key] != value for key, value in expected.items()): raise SystemExit("Oracle mismatch")
    print(json.dumps({"oracle_match": True, **expected}, indent=2))
