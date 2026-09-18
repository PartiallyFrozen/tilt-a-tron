"""Talk to a Tilt-a-tron over USB - the reference client for the link protocol.

The manager app (app/) speaks the same protocol; this is the version to test against and
the one to use from a script or CI.

    python tools/tatlink.py                 # find the watch and list what's on it
    python tools/tatlink.py --port COM4
    python tools/tatlink.py --icon breakout out.png

Opening the port must not toggle DTR/RTS: that resets the chip. See open_port().
"""
import argparse
import struct
import zlib
import sys
import time

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("pyserial is needed:  pip install pyserial")

PROTO = 1
HELLO, INFO, LIST, ICON, ERR = 0x01, 0x02, 0x03, 0x04, 0xFF
FS_FREE, FS_LIST, FS_PUT, FS_DATA, FS_END, FS_GET, FS_DELETE, FS_MKDIR = range(0x10, 0x18)
GAME_BUILTIN, GAME_HIDDEN = 1, 2


def crc16(data, crc=0xFFFF):
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


class Watch:
    def __init__(self, port, timeout=3.0):
        self.ser = open_port(port)
        self.timeout = timeout
        self.seq = 0

    def close(self):
        self.ser.close()

    def call(self, cmd, payload=b""):
        # Anything still unread is log noise from before this request.
        self.ser.reset_input_buffer() if cmd == HELLO else None
        self.seq = (self.seq + 1) & 0xFF
        head = struct.pack("<HBB", len(payload), self.seq, cmd)
        frame = b"\xa5\x5a" + head + payload + struct.pack(">H", crc16(payload, crc16(head[2:])))
        self.ser.write(frame)
        return self._read_reply(cmd)

    def _read_reply(self, cmd):
        # Log lines share this port, so hunt for the sync word and check the CRC.
        deadline = time.time() + self.timeout
        buf = bytearray()
        while time.time() < deadline:
            chunk = self.ser.read(4096)
            if chunk:
                buf += chunk
            while True:
                i = buf.find(b"\xa5\x5a")
                if i < 0 or len(buf) < i + 8:
                    break
                length, seq, rcmd = struct.unpack("<HBB", buf[i + 2:i + 6])
                end = i + 6 + length + 2
                if length > 4096:
                    del buf[:i + 2]
                    continue
                if len(buf) < end:
                    break
                body = bytes(buf[i + 6:i + 6 + length])
                got = struct.unpack(">H", buf[end - 2:end])[0]
                del buf[:end]
                if got != crc16(body, crc16(struct.pack("BB", seq, rcmd))):
                    continue
                if rcmd == ERR:
                    raise RuntimeError(body.decode(errors="replace"))
                if rcmd == (cmd | 0x80):
                    return body
        raise TimeoutError("the watch didn't answer (is it awake, and is this the right port?)")

    def hello(self):
        b = self.call(HELLO)
        proto, api_major, api_minor = struct.unpack("<HHH", b[:6])
        firmware, board = b[6:].split(b"\0")[:2]
        return dict(proto=proto, api=(api_major, api_minor),
                    firmware=firmware.decode(), board=board.decode())

    def info(self):
        total, free, count = struct.unpack("<IIB", self.call(INFO))
        return dict(total=total, free=free, count=count)

    def games(self):
        b = self.call(LIST)
        out = []
        for i in range(b[0]):
            id_, name, accent, flags, _, size = struct.unpack_from("<16s24sHBBI", b, 1 + i * 48)
            out.append(dict(id=id_.rstrip(b"\0").decode(), name=name.rstrip(b"\0").decode(),
                            accent=accent, builtin=bool(flags & GAME_BUILTIN),
                            hidden=bool(flags & GAME_HIDDEN), bytes=size))
        return out

    def icon(self, game_id):
        return self.call(ICON, game_id.encode().ljust(16, b"\0"))

    # ---- files on the watch (themes, watch faces)
    def fs_free(self):
        total, free = struct.unpack("<II", self.call(FS_FREE))
        return total, free

    def fs_list(self, path=""):
        b = self.call(FS_LIST, path.encode())
        out, i = [], 0
        while i < len(b):
            is_dir = b[i]
            size = struct.unpack_from("<I", b, i + 1)[0]
            end = b.index(b"\0", i + 5)
            out.append(dict(name=b[i + 5:end].decode(errors="replace"), dir=bool(is_dir), size=size))
            i = end + 1
        return out

    def fs_mkdir(self, path):
        self.call(FS_MKDIR, path.encode())

    def fs_delete(self, path):
        self.call(FS_DELETE, path.encode())

    def fs_get(self, path):
        return self.call(FS_GET, path.encode())

    def fs_put(self, path, data, progress=None):
        crc = zlib.crc32(data) & 0xFFFFFFFF
        self.call(FS_PUT, struct.pack("<II", len(data), crc) + path.encode())
        CHUNK = 4096   # the watch's maximum payload
        for off in range(0, len(data), CHUNK):
            self.call(FS_DATA, data[off:off + CHUNK])
            if progress:
                progress(min(off + CHUNK, len(data)), len(data))
        self.call(FS_END)

    def send_folder(self, local, remote):
        """Copy a folder to the watch, making directories as it goes."""
        import os
        self.fs_mkdir(remote)
        for root, dirs, files in os.walk(local):
            rel = os.path.relpath(root, local).replace("\\", "/")
            base = remote if rel == "." else f"{remote}/{rel}"
            for d in dirs:
                self.fs_mkdir(f"{base}/{d}")
            for f in files:
                data = open(os.path.join(root, f), "rb").read()
                print(f"    {base}/{f}  {len(data)} bytes")
                self.fs_put(f"{base}/{f}", data)


def open_port(port):
    # dsrdtr/rtscts off and both lines left alone: esptool uses them to reset the chip,
    # and a reset here would reboot the watch the moment the app connects.
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = 115200
    ser.timeout = 0.02   # short: a reply is polled for, not waited on
    ser.dtr = False
    ser.rts = False
    ser.open()
    return ser


def find_watch():
    """Ports that look like an ESP32-S3 USB-Serial/JTAG, best guess first."""
    found = []
    for p in list_ports.comports():
        if p.vid == 0x303A:            # Espressif
            found.append((0, p.device))
        elif p.vid is not None:
            found.append((1, p.device))
    return [d for _, d in sorted(found)]


def main():
    ap = argparse.ArgumentParser(description="Talk to a Tilt-a-tron over USB")
    ap.add_argument("--port")
    ap.add_argument("--icon", nargs=2, metavar=("GAME", "OUT.PNG"))
    ap.add_argument("--ls", metavar="PATH", nargs="?", const="", help="list files on the watch")
    ap.add_argument("--send", nargs=2, metavar=("LOCAL_DIR", "REMOTE_DIR"),
                    help="copy a folder to the watch, e.g. themes/CPU Theme/CPU")
    ap.add_argument("--rm", metavar="PATH", help="delete a file or folder on the watch")
    args = ap.parse_args()

    ports = [args.port] if args.port else find_watch()
    if not ports:
        sys.exit("no serial ports found - is the watch plugged in with a data cable?")

    last = None
    for port in ports:
        try:
            w = Watch(port)
            hi = w.hello()
        except Exception as e:
            last = f"{port}: {e}"
            continue
        print(f"Tilt-a-tron on {port}")
        print(f"  firmware {hi['firmware']}   board {hi['board']}")
        print(f"  link protocol {hi['proto']}, game API {hi['api'][0]}.{hi['api'][1]}")
        nfo = w.info()
        print(f"  games region: {nfo['free'] // 1024} KB free of {nfo['total'] // 1024} KB")
        print()
        for g in w.games():
            where = "built in" if g["builtin"] else f"{g['bytes'] // 1024} KB"
            state = "hidden" if g["hidden"] else "on"
            print(f"  {g['name']:<14} {where:<10} {state}")
        if args.ls is not None:
            total, free = w.fs_free()
            print(f"\n  storage: {free // 1024} KB free of {total // 1024} KB")
            for e in w.fs_list(args.ls):
                print(f"    {'[dir] ' if e['dir'] else '      '}{e['name']}"
                      + ("" if e["dir"] else f"  {e['size']} bytes"))
        if args.rm:
            w.fs_delete(args.rm)
            print(f"\ndeleted {args.rm}")
        if args.send:
            local, remote = args.send
            print(f"\nsending {local} -> {remote}")
            w.send_folder(local, remote)
            print("done")
        if args.icon:
            png = w.icon(args.icon[0])
            open(args.icon[1], "wb").write(png)
            print(f"\nwrote {args.icon[1]} ({len(png)} bytes)")
        w.close()
        return
    sys.exit(f"couldn't reach a watch. last error - {last}")


if __name__ == "__main__":
    main()
