#!/usr/bin/env python3
"""Cross-check the C++ pipeline against the Python reference implementation.

Runs both over the same ITCH file and requires byte-identical BBO history,
trade tape, final depth snapshot and message counts. With --file an existing
(e.g. real Nasdaq) file is used; otherwise synthetic files are generated, one
per profile, plus a deliberately corrupted copy to compare error handling.

    scripts/crosscheck.py --gen build/itch_gen --replay build/itch_replay
    scripts/crosscheck.py --replay build/itch_replay --file sample.itch --limit-bytes 50000000
"""
import argparse
import filecmp
import os
import random
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
OUTPUTS = ("bbo", "trades", "snapshot", "counts")


def run_pair(replay_bin, itch_file, workdir, tag, depth):
    cpp = {k: os.path.join(workdir, "%s.cpp.%s.csv" % (tag, k)) for k in OUTPUTS}
    ref = {k: os.path.join(workdir, "%s.ref.%s.csv" % (tag, k)) for k in OUTPUTS}

    def flags(paths):
        out = ["--depth", str(depth)]
        for k in OUTPUTS:
            out += ["--%s-csv" % k, paths[k]]
        return out

    # Exit status 3 means "input ended in a truncated frame"; the prefix was
    # still processed, which is exactly what the reference does too.
    cpp_run = subprocess.run(
        [replay_bin, itch_file, "--show", "0"] + flags(cpp), stdout=subprocess.DEVNULL
    )
    if cpp_run.returncode not in (0, 3):
        print("FAIL %s: itch_replay exited %d" % (tag, cpp_run.returncode))
        return False
    subprocess.run(
        [sys.executable, os.path.join(HERE, "itch_ref.py"), itch_file] + flags(ref),
        check=True,
        stderr=subprocess.DEVNULL,
    )

    ok = True
    for k in OUTPUTS:
        same = filecmp.cmp(cpp[k], ref[k], shallow=False)
        with open(cpp[k], encoding="latin-1") as fh:
            rows = sum(1 for _ in fh) - 1
        print("  %-8s %8d rows  %s" % (k, rows, "match" if same else "MISMATCH"))
        if not same:
            ok = False
            with open(cpp[k], encoding="latin-1") as a, open(ref[k], encoding="latin-1") as b:
                for line_no, (la, lb) in enumerate(zip(a, b), 1):
                    if la != lb:
                        print("    first difference at line %d" % line_no)
                        print("      c++: %s" % la.rstrip())
                        print("      ref: %s" % lb.rstrip())
                        break
    print("%s %s" % ("PASS" if ok else "FAIL", tag))
    return ok


def corrupt(src, dst, seed):
    """Flip bytes inside message bodies, then cut the tail mid-frame.

    Length prefixes are left intact so the stream stays framed and every
    damaged message reaches the decoder and book: this produces unknown
    types, bad sides, unknown and duplicate order references, and
    executions larger than the remaining size.
    """
    rng = random.Random(seed)
    with open(src, "rb") as fh:
        data = bytearray(fh.read())
    bodies = []
    pos = 0
    while len(data) - pos >= 2:
        length = (data[pos] << 8) | data[pos + 1]
        bodies.append((pos + 2, length))
        pos += 2 + length
    for start, length in rng.sample(bodies, max(1, len(bodies) // 20)):
        data[start + rng.randrange(length)] = rng.randrange(256)
    del data[len(data) - 7 :]
    with open(dst, "wb") as fh:
        fh.write(data)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--replay", required=True, help="path to itch_replay")
    ap.add_argument("--gen", help="path to itch_gen (needed unless --file is given)")
    ap.add_argument("--file", help="existing length-prefixed ITCH file to check")
    ap.add_argument("--limit-bytes", type=int, default=0, help="with --file: use only a prefix")
    ap.add_argument("--messages", type=int, default=200000)
    ap.add_argument("--symbols", type=int, default=50)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--depth", type=int, default=10)
    args = ap.parse_args()

    ok = True
    with tempfile.TemporaryDirectory(prefix="itch_crosscheck_") as work:
        if args.file:
            path = args.file
            if args.limit_bytes:
                path = os.path.join(work, "prefix.itch")
                with open(args.file, "rb") as src, open(path, "wb") as dst:
                    dst.write(src.read(args.limit_bytes))
            ok &= run_pair(args.replay, path, work, "file", args.depth)
        else:
            if not args.gen:
                ap.error("--gen is required unless --file is given")
            for profile in ("balanced", "add-heavy", "churn"):
                path = os.path.join(work, profile + ".itch")
                subprocess.run(
                    [
                        args.gen,
                        "--out", path,
                        "--messages", str(args.messages),
                        "--symbols", str(args.symbols),
                        "--seed", str(args.seed),
                        "--profile", profile,
                    ],
                    check=True,
                    stdout=subprocess.DEVNULL,
                )
                ok &= run_pair(args.replay, path, work, profile, args.depth)
            bad = os.path.join(work, "corrupted.itch")
            corrupt(os.path.join(work, "balanced.itch"), bad, args.seed)
            ok &= run_pair(args.replay, bad, work, "corrupted", args.depth)

    print("crosscheck: %s" % ("all outputs identical" if ok else "FAILED"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
