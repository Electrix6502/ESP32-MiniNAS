#include "Storage.h"
namespace Storage {
bool resolve(unsigned v, const char *input, std::string &output) {
    if (v > 1 || !volumes[v].ready || !input)
        return false;
    std::string relative, component;
    auto append = [&]() -> bool {
        if (component == "..")
            return false;
        if (!component.empty() && component != ".") {
            if (component.size() > 255)
                return false;
            relative += "/" + component;
        }
        component.clear();
        return true;
    };
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(input); *p; ++p) {
        if (*p == '/' || *p == 92) {
            if (!append())
                return false;
        } else {
            if (*p < 32 || *p == ':' || *p == 127)
                return false;
            component.push_back(char(*p));
        }
        if (relative.size() + component.size() > 480)
            return false;
    }
    if (!append())
        return false;
    output = volumes[v].root + relative;
    return true;
}
} // namespace Storage
