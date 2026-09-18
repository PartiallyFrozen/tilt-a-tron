"""Measures a watch over USB, so a performance change can be shown rather than argued.

    python tools/bench.py                 # find a watch and measure it
    python tools/bench.py --port COM4
    python tools/bench.py --json out.json # for comparing two firmware builds

Reports read and write throughput separately, because they are wildly different on this
hardware and an average hides that. See app/README.md for what has already been ruled out
as the cause.
"""
import argparse
import json
import os
import statistics
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__)))
from tatlink import Watch, find_watch   # noqa: E402

SCRATCH = "Theme/.bench"


def timed(fn):
    t = time.perf_counter()
    fn()
    return time.perf_counter() - t


def bench_write(w, size, repeats=3):
    """PC -> watch, the direction that is slow."""
    data = os.urandom(size)
    times = [timed(lambda: w.fs_put(SCRATCH, data)) for _ in range(repeats)]
    return size / 1024 / statistics.median(times)


def bench_read(w, size, repeats=3):
    """watch -> PC."""
    w.fs_put(SCRATCH, os.urandom(size))
    times = [timed(lambda: w.fs_read_all(SCRATCH, size)) for _ in range(repeats)]
    return size / 1024 / statistics.median(times)


def bench_round_trip(w, repeats=20):
    """A whole command with nothing to carry: what a single exchange costs."""
    t = time.perf_counter()
    for _ in range(repeats):
        w.fs_free()
    return (time.perf_counter() - t) * 1000 / repeats


def main():
    ap = argparse.ArgumentParser(description="Measure a Tilt-a-tron over USB")
    ap.add_argument("--port")
    ap.add_argument("--json", metavar="FILE", help="also write the numbers here")
    ap.add_argument("--size", type=int, default=64 * 1024, help="bytes per transfer (default 64 KB)")
    args = ap.parse_args()

    ports = [args.port] if args.port else find_watch()
    w = None
    for port in ports:
        try:
            w = Watch(port)
            hi = w.hello()
            break
        except Exception:
            w = None
    if not w:
        sys.exit("no watch found - plug it in with a data cable and wake it")

    print(f"Tilt-a-tron on {w.ser.port}, firmware {hi['firmware']}")
    print(f"  {args.size // 1024} KB per transfer, median of 3\n")

    result = {"firmware": hi["firmware"], "size": args.size}
    result["round_trip_ms"] = bench_round_trip(w)
    print(f"  round trip, no payload   {result['round_trip_ms']:6.1f} ms")
    result["read_kbs"] = bench_read(w, args.size)
    print(f"  read   (watch -> PC)     {result['read_kbs']:6.1f} KB/s")
    result["write_kbs"] = bench_write(w, args.size)
    print(f"  write  (PC -> watch)     {result['write_kbs']:6.1f} KB/s")

    try:
        w.fs_delete(SCRATCH)
    except Exception:
        pass
    w.close()

    if args.json:
        with open(args.json, "w") as f:
            json.dump(result, f, indent=2)
        print(f"\nwrote {args.json}")


if __name__ == "__main__":
    main()
