#pragma once
#include "Arduino.h"
struct File { operator bool() const { return false; } void close() {} };
struct SimFs {
    bool exists(const char *) const { return false; }
    bool exists(const String &) const { return false; }
    File open(const char *, const char *) const { return File(); }
    File open(const String &, const char *) const { return File(); }
};
static SimFs LittleFS;
