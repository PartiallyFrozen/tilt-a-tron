// Saved settings and scores. Every game and every console screen keeps a handful of small
// values in NVS, and each of them used to open, read with its own ad-hoc range check, and
// close by hand - which is how three games ended up validating the same kind of value three
// different ways, and one not at all.
//
//   void load()
//   {
//       wc::Store s("racer");
//       s.get("mirror", mirror);
//       s.get("pitch", pitch_sens, 4);   // ignored unless 0 <= value < 4
//       s.get("best", best);
//   }
//
//   void save()
//   {
//       wc::Store s("racer", wc::Store::Write);
//       s.set("mirror", mirror);
//       s.set("pitch", pitch_sens);
//       s.set("best", best);
//   }
//
// A value that isn't stored yet, or is out of range, leaves the variable alone - so the
// caller's initialiser is the default, written once where it is declared.
#pragma once

#include <cstdint>

namespace wc {

class Store {
public:
    enum Mode { Read, Write };

    explicit Store(const char *name_space, Mode mode = Read);
    ~Store();                      // commits and closes if opened for writing
    Store(const Store &) = delete;
    Store &operator=(const Store &) = delete;

    bool ok() const { return handle_ != 0; }

    // `limit` > 0 rejects anything outside 0 .. limit-1, for values that index a table.
    void get(const char *key, int &v, int limit = 0) const;
    void get(const char *key, bool &v) const;
    void get(const char *key, float &v) const;

    void set(const char *key, int v);
    void set(const char *key, bool v);
    void set(const char *key, float v);

    void erase(const char *key);

private:
    uint32_t handle_ = 0;
    bool writing_ = false;
};

}  // namespace wc
