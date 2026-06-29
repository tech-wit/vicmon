#pragma once
#include <Arduino.h>

// Manages up to kMax named profiles (e.g. "Home", "4WD"), each backed by its
// own per-profile NVS namespaces for devices / signals / settings. Profile ids
// are stable (0..kMax-1); an empty name means the slot is unused.
class ProfileManager {
public:
    static constexpr int kMax = 4;

    void begin();  // loads; creates "Profile 1" at id 0 on first run

    int active() const { return active_; }
    const char* name(int id) const { return (id >= 0 && id < kMax) ? names_[id] : ""; }
    bool used(int id) const { return id >= 0 && id < kMax && names_[id][0] != '\0'; }
    int usedCount() const;

    void setActive(int id);
    int create(const char* name);  // -> new id, or -1 if full
    void rename(int id, const char* name);
    void setName(int id, const char* name);  // create-or-rename at a specific id (import)
    void remove(int id);  // clears the name slot (caller wipes NVS data)

private:
    void load();
    void save();
    char names_[kMax][20];
    int active_ = 0;
};
