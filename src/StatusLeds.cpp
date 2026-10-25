#include "StatusLeds.h"
#include "ProjectConfig.h"
#include <Arduino.h>

namespace {
volatile uint32_t readUntil = 0;
volatile uint32_t writeUntil = 0;
bool ready = false;
bool initialized = false;

constexpr uint8_t ON = LOW;
constexpr uint8_t OFF = HIGH;

bool active(uint32_t until, uint32_t now) {
    return int32_t(until - now) > 0;
}
} // namespace

namespace StatusLeds {
void begin() {
    pinMode(Config::LED_WRITE_PIN, OUTPUT);
    pinMode(Config::LED_READ_PIN, OUTPUT);
    pinMode(Config::LED_READY_PIN, OUTPUT);
    digitalWrite(Config::LED_WRITE_PIN, OFF);
    digitalWrite(Config::LED_READ_PIN, OFF);
    digitalWrite(Config::LED_READY_PIN, OFF);
    initialized = true;
}
void setReady(bool value) {
    ready = value;
    if (initialized)
        digitalWrite(Config::LED_READY_PIN, ready ? ON : OFF);
}
void readActivity() {
    readUntil = millis() + Config::LED_ACTIVITY_MS;
}
void writeActivity() {
    writeUntil = millis() + Config::LED_ACTIVITY_MS;
}
void loop() {
    if (!initialized)
        return;
    const uint32_t now = millis();
    digitalWrite(Config::LED_READ_PIN, active(readUntil, now) ? ON : OFF);
    digitalWrite(Config::LED_WRITE_PIN, active(writeUntil, now) ? ON : OFF);
}
} // namespace StatusLeds
