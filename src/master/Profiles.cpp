#include "Profiles.h"

#include <Preferences.h>

#include <cstring>

namespace {
const char* kNs = "vicprof";
}

void ProfileManager::begin() {
    load();
    if (usedCount() == 0) {
        strncpy(names_[0], "Profile 1", sizeof(names_[0]) - 1);
        active_ = 0;
        save();
    }
}

int ProfileManager::usedCount() const {
    int n = 0;
    for (int i = 0; i < kMax; ++i)
        if (names_[i][0] != '\0') ++n;
    return n;
}

void ProfileManager::load() {
    Preferences p;
    p.begin(kNs, /*readOnly=*/true);
    active_ = p.getInt("active", 0);
    for (int i = 0; i < kMax; ++i) {
        char k[8];
        snprintf(k, sizeof(k), "n%d", i);
        String s = p.getString(k, "");
        names_[i][0] = '\0';
        strncpy(names_[i], s.c_str(), sizeof(names_[i]) - 1);
    }
    p.end();
    if (active_ < 0 || active_ >= kMax || !used(active_)) active_ = 0;
}

void ProfileManager::save() {
    Preferences p;
    p.begin(kNs, /*readOnly=*/false);
    p.putInt("active", active_);
    for (int i = 0; i < kMax; ++i) {
        char k[8];
        snprintf(k, sizeof(k), "n%d", i);
        p.putString(k, names_[i]);
    }
    p.end();
}

void ProfileManager::setActive(int id) {
    if (used(id)) {
        active_ = id;
        save();
    }
}

int ProfileManager::create(const char* name) {
    for (int i = 0; i < kMax; ++i) {
        if (names_[i][0] == '\0') {
            strncpy(names_[i], (name && name[0]) ? name : "Profile", sizeof(names_[i]) - 1);
            save();
            return i;
        }
    }
    return -1;
}

void ProfileManager::rename(int id, const char* name) {
    if (used(id) && name && name[0]) {
        strncpy(names_[id], name, sizeof(names_[id]) - 1);
        names_[id][sizeof(names_[id]) - 1] = '\0';
        save();
    }
}

void ProfileManager::setName(int id, const char* name) {
    if (id >= 0 && id < kMax && name && name[0]) {
        strncpy(names_[id], name, sizeof(names_[id]) - 1);
        names_[id][sizeof(names_[id]) - 1] = '\0';
        save();
    }
}

void ProfileManager::remove(int id) {
    if (id >= 0 && id < kMax && id != active_ && usedCount() > 1) {
        names_[id][0] = '\0';
        save();
    }
}
