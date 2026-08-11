// Host-side stand-in for the Arduino core, just large enough to compile the
// firmware's display code unmodified.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#define PROGMEM
#define pgm_read_byte(addr) (*reinterpret_cast<const uint8_t *>(addr))
#define pgm_read_word(addr) (*reinterpret_cast<const uint16_t *>(addr))
#define pgm_read_dword(addr) (*reinterpret_cast<const uint32_t *>(addr))
#define F(x) (x)
#define PI 3.1415926535897932384626433832795f

using std::max;
using std::min;

template <typename T, typename L, typename H>
T constrain(T value, L low, H high) {
    return value < static_cast<T>(low) ? static_cast<T>(low)
                                       : (value > static_cast<T>(high) ? static_cast<T>(high) : value);
}

inline long map(long value, long inMin, long inMax, long outMin, long outMax) {
    if (inMax == inMin) {
        return outMin;
    }
    return ((value - inMin) * (outMax - outMin) / (inMax - inMin)) + outMin;
}

inline void analogWrite(int, int) {}
inline void analogWriteFreq(int) {}
inline void analogWriteRange(int) {}
inline void pinMode(int, int) {}
inline void delay(uint32_t) {}
inline void yield() {}
#define OUTPUT 1

uint32_t millis();
void simSetMillis(uint32_t value);

class String {
  public:
    String() {}
    String(const char *value) : text(value ? value : "") {}
    String(const std::string &value) : text(value) {}
    String(char value) : text(1, value) {}
    explicit String(int value) { text = std::to_string(value); }
    explicit String(long value) { text = std::to_string(value); }
    explicit String(unsigned value) { text = std::to_string(value); }
    String(float value, int digits) {
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%.*f", digits, value);
        text = buffer;
    }

    const char *c_str() const { return text.c_str(); }
    int length() const { return static_cast<int>(text.size()); }
    bool isEmpty() const { return text.empty(); }
    char charAt(int index) const { return text[static_cast<size_t>(index)]; }

    int indexOf(char value) const {
        size_t position = text.find(value);
        return position == std::string::npos ? -1 : static_cast<int>(position);
    }
    int indexOf(const char *value) const {
        size_t position = text.find(value);
        return position == std::string::npos ? -1 : static_cast<int>(position);
    }

    String substring(int from) const { return String(text.substr(static_cast<size_t>(from))); }
    String substring(int from, int to) const {
        return String(text.substr(static_cast<size_t>(from), static_cast<size_t>(to - from)));
    }

    void toUpperCase() {
        for (char &character : text) {
            character = static_cast<char>(toupper(static_cast<unsigned char>(character)));
        }
    }
    void toLowerCase() {
        for (char &character : text) {
            character = static_cast<char>(tolower(static_cast<unsigned char>(character)));
        }
    }
    void trim() {
        size_t begin = text.find_first_not_of(" \t\r\n");
        size_t end = text.find_last_not_of(" \t\r\n");
        text = begin == std::string::npos ? "" : text.substr(begin, end - begin + 1);
    }
    void remove(int index) { text.erase(static_cast<size_t>(index)); }
    int lastIndexOf(char value) const {
        size_t position = text.rfind(value);
        return position == std::string::npos ? -1 : static_cast<int>(position);
    }
    void replace(const char *from, const char *to) {
        size_t position = 0;
        while ((position = text.find(from, position)) != std::string::npos) {
            text.replace(position, strlen(from), to);
            position += strlen(to);
        }
    }
    void setCharAt(int index, char value) { text[static_cast<size_t>(index)] = value; }
    void reserve(int) {}
    bool endsWith(const char *value) const {
        size_t length = strlen(value);
        return text.size() >= length && text.compare(text.size() - length, length, value) == 0;
    }
    bool startsWith(const char *value) const { return text.rfind(value, 0) == 0; }

    String &operator+=(const String &other) { text += other.text; return *this; }
    String &operator+=(const char *other) { text += other; return *this; }
    String &operator+=(char other) { text += other; return *this; }

    friend String operator+(String left, const String &right) { left.text += right.text; return left; }
    friend String operator+(String left, const char *right) { left.text += right; return left; }
    friend String operator+(const char *left, const String &right) { return String(std::string(left) + right.text); }
    friend bool operator==(const String &left, const String &right) { return left.text == right.text; }
    friend bool operator!=(const String &left, const String &right) { return left.text != right.text; }

    std::string text;
};

// The firmware asks the chip for free heap when deciding whether to allocate
// the clock sprite; on the host there is always plenty, so the sprite path is
// exercised the same way the device would at rest.
struct SimEsp {
    uint32_t getFreeHeap() const { return 14000; }
    void restart() {}
};
static SimEsp ESP;
