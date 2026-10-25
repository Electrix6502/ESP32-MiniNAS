#pragma once

namespace StatusLeds {
void begin();
void setReady(bool ready);
void readActivity();
void writeActivity();
void loop();
} // namespace StatusLeds
