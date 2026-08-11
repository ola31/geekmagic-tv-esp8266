#pragma once
#include "Arduino.h"
typedef int JRESULT;
#define JDR_OK 0
struct SimTJpg {
    void setJpgScale(int) {}
    void setSwapBytes(bool) {}
    void setCallback(bool (*)(int16_t, int16_t, uint16_t, uint16_t, uint16_t *)) {}
    JRESULT drawFsJpg(int, int, File &) { return JDR_OK; }
};
static SimTJpg TJpgDec;
