#pragma once
#include <stdint.h>
#include <string>
class StorageGuard {
  public:
    StorageGuard();
    ~StorageGuard();
    StorageGuard(const StorageGuard &) = delete;
    StorageGuard &operator=(const StorageGuard &) = delete;
};
namespace Storage {
struct Volume {
    std::string name;
    std::string root;
    bool ready = false;
    bool fat32 = false;
};
extern Volume volumes[2];
void begin();
uint64_t total(unsigned volume);
uint64_t used(unsigned volume);
const char *sdStatus();
bool resolve(unsigned volume, const char *relative, std::string &absolute);
} // namespace Storage
