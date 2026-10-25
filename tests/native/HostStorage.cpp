#include "Storage.h"
#include "SmbServer.h"
#include <mutex>
#include <sys/statvfs.h>
#include <cstdlib>
#include <csignal>
namespace {
std::recursive_mutex mutex;
}
// ASan/UBSan remain enabled; /proc-dependent leak checking is optional.
extern "C" const char *__asan_default_options() {
    return "detect_leaks=0";
}
StorageGuard::StorageGuard() {
    mutex.lock();
}
StorageGuard::~StorageGuard() {
    mutex.unlock();
}
namespace Storage {
Volume volumes[2];
void begin() {}
const char *sdStatus() {
    return "Native test";
}
uint64_t total(unsigned v) {
    struct statvfs st;
    return statvfs(volumes[v].root.c_str(), &st) == 0 ? uint64_t(st.f_blocks) * st.f_frsize : 0;
}
uint64_t used(unsigned v) {
    struct statvfs st;
    return statvfs(volumes[v].root.c_str(), &st) == 0
               ? uint64_t(st.f_blocks - st.f_bfree) * st.f_frsize
               : 0;
}
} // namespace Storage
int main(int argc, char **argv) {
    if (argc != 4)
        return 2;
    Storage::volumes[0] = {"flash", argv[1], true, false};
    Storage::volumes[1] = {"sd", argv[2], true, true};
    std::signal(SIGTERM, [](int) { SmbServer::stop(); });
    return SmbServer::serve(std::strtoul(argv[3], nullptr, 10));
}
