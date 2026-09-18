// Bridges the gaps between the compiler the firmware is built with and whatever is on a
// contributor's desk. Include it before any firmware header.
#pragma once

#ifdef _MSC_VER
// MSVC spells packing differently. Nothing here checks a struct's byte layout - that is
// the wire format's business, and tests/test_protocol.py covers it - so dropping the
// attribute is safe for these tests and keeps the firmware header unchanged.
#define __attribute__(x)
#include <direct.h>
#include <sys/stat.h>
#define mkdir(path, mode) _mkdir(path)
#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#endif
#else
#include <sys/stat.h>
#endif
