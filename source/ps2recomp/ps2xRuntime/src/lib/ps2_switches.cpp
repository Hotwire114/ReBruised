#include "runtime/ps2_switches.h"

#include <cctype>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace ps2_switches
{
    namespace
    {
        bool truthy(const char *v)
        {
            if (!v || !v[0])
                return false;
            const std::string s(v);
            return s == "1" || s == "on" || s == "16:9" || s == "true" || s == "yes";
        }

        // A switch starts from the environment: PS2X_SWITCH_<NAME> (name upper-cased) = 1/on/true/yes, and for
        // "widescreen" also the Tekken 4 setting TEKKEN4_WIDESCREEN (1/on/16:9). Anything else, or unset, is off.
        bool initialValue(const std::string &name)
        {
            std::string key = "PS2X_SWITCH_";
            for (char c : name)
                key += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            if (const char *v = std::getenv(key.c_str()))
                return truthy(v);
            if (name == "widescreen")
                return truthy(std::getenv("TEKKEN4_WIDESCREEN"));
            return false;
        }
    }

    std::atomic<bool> &flag(const char *name)
    {
        static std::mutex mutex;
        static std::unordered_map<std::string, std::unique_ptr<std::atomic<bool>>> flags;
        std::lock_guard<std::mutex> lock(mutex);
        std::unique_ptr<std::atomic<bool>> &slot = flags[name];
        if (!slot)
            slot = std::make_unique<std::atomic<bool>>(initialValue(name));
        return *slot;
    }
}
