# Tests

Host tests for the parts of the firmware that have no hardware in them. They compile the
real firmware sources against the stubs in `stubs/`, so they test the code that ships
rather than a copy of it.

```bash
./tests/run.sh
```

Needs `gcc`, `g++` and `python3`. CI runs this on every push and pull request.

| File | What it protects |
|---|---|
| `test_link.c` | `safe_path` - what a computer plugged into the watch may write to. A hole here means anything on the USB port can reach outside the storage folder. Also `crc16`, which both clients have to agree with, and `real_entry`, which keeps a folder walk out of the phantom directory entries an old FAT corruption left behind. |
| `test_store.cpp` | Whether a saved high score survives a firmware update. Games used to write `u8` and `u16`; `wc::Store` writes `i32` and must read all three, and must clear a key before changing its width or every save after an update fails silently. |
| `test_protocol.py` | That the Python client frames and checks a packet exactly the way the firmware does. It builds real frames and takes them apart again. |

## Adding to them

Anything that is pure logic belongs here. Anything that needs the display, the IMU or
real flash does not - that is what `/status`, `tools/tatlink.py` and a watch on the desk
are for.

`stubs/nvs.h` deliberately models NVS's refusal to change a key's width, because that
behaviour is the reason `Store::set` clears a key first. If a stub ever gets friendlier
than the real thing, the test stops being worth anything.
