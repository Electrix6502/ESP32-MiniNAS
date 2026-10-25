#pragma once
#include <stdint.h>
#include <string>
namespace SmbServer {
void begin();
bool ready();
unsigned clients();
unsigned handles();
bool writeOpen(const std::string &absolute);
int serve(uint16_t port);
void stop();
} // namespace SmbServer
