"""The Python client has to frame and check a packet exactly the way the firmware does.

If these two ever drift, transfers fail in ways that look like flaky hardware rather than
a bug, so this builds real frames and takes them apart again. No watch needed.

    python tests/test_protocol.py
"""
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))

# tatlink imports pyserial for talking to a watch; the framing does not need it.
try:
    from tatlink import crc16, HELLO, FS_PUT, ERR
except SystemExit:
    sys.exit("pyserial is needed to import the client:  pip install pyserial")

checks = failed = 0


def check(cond, what):
    global checks, failed
    checks += 1
    if not cond:
        failed += 1
        print(f"  FAIL {what}")


def frame(seq, cmd, payload):
    """Exactly what Watch.call puts on the wire."""
    head = struct.pack("<HBB", len(payload), seq, cmd)
    return b"\xa5\x5a" + head + payload + struct.pack(">H", crc16(payload, crc16(head[2:])))


def unframe(data):
    """The firmware's reader: find the sync word, check the CRC, hand back the body."""
    i = data.find(b"\xa5\x5a")
    if i < 0 or len(data) < i + 8:
        return None
    length, seq, cmd = struct.unpack("<HBB", data[i + 2:i + 6])
    end = i + 6 + length + 2
    if len(data) < end:
        return None
    body = data[i + 6:i + 6 + length]
    got = struct.unpack(">H", data[end - 2:end])[0]
    if got != crc16(body, crc16(struct.pack("BB", seq, cmd))):
        return None
    return seq, cmd, body


def test_crc_matches_the_firmware():
    # The same vectors tests/test_link.c checks the C against.
    check(crc16(b"") == 0xFFFF, "crc of nothing")
    check(crc16(b"A") == 0xB915, "crc of A")
    check(crc16(b"123456789") == 0x29B1, "crc of the standard check string")
    # Split the input; the running CRC must not care where the break is.
    check(crc16(b"3456", crc16(b"12")) == crc16(b"123456"), "crc resumes across a split")


def test_round_trip():
    for payload in (b"", b"x", b"hello", bytes(range(256)), os.urandom(4096)):
        got = unframe(frame(7, FS_PUT, payload))
        check(got is not None, f"{len(payload)}-byte frame parses")
        if got:
            seq, cmd, body = got
            check(seq == 7 and cmd == FS_PUT and body == payload, f"{len(payload)}-byte frame survives")


def test_rejects_damage():
    good = frame(1, HELLO, b"abcdef")
    for i in range(len(good)):
        bad = bytearray(good)
        bad[i] ^= 0xFF
        if bytes(bad[:2]) != b"\xa5\x5a":
            continue   # a broken sync word is a different failure: the reader just keeps hunting
        check(unframe(bytes(bad)) is None, f"a flipped bit at {i} is caught")


def test_finds_a_frame_in_log_noise():
    # The log shares the wire, so a frame usually arrives with text around it.
    noise = b"I (1234) main: Tilt-a-tron 0.6.0\n"
    got = unframe(noise + frame(3, HELLO, b"ok") + b"\nI (1240) wifi: connected\n")
    check(got is not None and got[2] == b"ok", "a frame is found among log lines")


def test_a_false_sync_word_inside_a_payload():
    # Payload bytes can happen to be A5 5A. The length and CRC are what settle it.
    payload = b"\xa5\x5a\x00\x00" * 4
    got = unframe(frame(9, FS_PUT, payload))
    check(got is not None and got[2] == payload, "a payload containing the sync word survives")


def test_error_replies_carry_a_reason():
    got = unframe(frame(2, ERR, b"bad path"))
    check(got is not None and got[1] == ERR and got[2] == b"bad path", "an error carries its reason")


for fn in list(globals().values()):
    if callable(fn) and getattr(fn, "__name__", "").startswith("test_"):
        fn()

print(f"protocol: {checks} checks, {failed} failed")
sys.exit(1 if failed else 0)
