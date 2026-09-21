"""Measures how fast a watch sends over Wi-Fi, so a change can be shown rather than argued.

    python tools/wifi_speed.py                  # the watch update.ps1 last talked to
    python tools/wifi_speed.py 192.168.1.50 --kb 1024 --repeats 5
    python tools/wifi_speed.py --json before.json

GET /speed?kb=N is N KB of nothing in particular, sent as fast as the watch can: the
transmit path and nothing else. GET /screen is measured too because it is what that speed is
for, but its first byte waits for a PNG to be encoded, so the clock for it starts there.
A firmware from before /speed existed is measured on /screen alone.

The key comes from --key, TILTATRON_KEY, or where update.ps1 keeps it.
"""
import argparse
import json
import os
import statistics
import sys
import time
import urllib.error
import urllib.request

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))


def device_key(given):
    if given:
        return given
    if os.environ.get("TILTATRON_KEY"):
        return os.environ["TILTATRON_KEY"]
    path = os.path.join(os.environ.get("LOCALAPPDATA", os.path.expanduser("~/.config")), "tiltatron", "key")
    return open(path).read().strip() if os.path.exists(path) else ""


def fetch(url, key, timeout):
    """Bytes received, seconds from the first byte to the last, seconds to the first byte."""
    req = urllib.request.Request(url, headers={"X-Tat-Key": key} if key else {})
    t0 = time.perf_counter()
    with urllib.request.urlopen(req, timeout=timeout) as r:
        first = r.read(1)
        t1 = time.perf_counter()
        n = len(first)
        while True:
            chunk = r.read(65536)
            if not chunk:
                break
            n += len(chunk)
    return n, time.perf_counter() - t1, t1 - t0


def measure(name, url, key, repeats, timeout):
    rates = []
    for k in range(repeats):
        try:
            n, seconds, wait = fetch(url, key, timeout)
        except urllib.error.HTTPError as e:
            if e.code == 404:
                print(f"  {name:8} not on this firmware")
                return None
            raise
        rate = n / 1024 / max(seconds, 1e-6)
        rates.append(rate)
        print(f"  {name:8} {n / 1024:7.0f} KB in {seconds:6.2f} s = {rate:7.1f} KB/s   (first byte after {wait:.2f} s)")
    return {"median_kb_s": round(statistics.median(rates), 1), "runs_kb_s": [round(r, 1) for r in rates]}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("ip", nargs="?")
    ap.add_argument("--kb", type=int, default=512)
    ap.add_argument("--repeats", type=int, default=3)
    ap.add_argument("--key")
    ap.add_argument("--json")
    a = ap.parse_args()

    ip = a.ip
    cache = os.path.join(ROOT, "build", ".watch-ip")
    if not ip and os.path.exists(cache):
        ip = open(cache).read().strip()
    if not ip:
        sys.exit("which watch? give its address")
    key = device_key(a.key)

    status = json.loads(urllib.request.urlopen(f"http://{ip}/status", timeout=8).read())
    print(f"{ip}: firmware {status.get('version')} build {status.get('sha')}, "
          f"{status.get('heap_internal', 0) // 1024} KB internal RAM free")
    out = {"firmware": status.get("version"), "build": status.get("sha")}
    out["speed"] = measure("/speed", f"http://{ip}/speed?kb={a.kb}", key, a.repeats, 180)
    out["screen"] = measure("/screen", f"http://{ip}/screen", key, a.repeats, 180)
    if a.json:
        json.dump(out, open(a.json, "w"), indent=2)


if __name__ == "__main__":
    main()
