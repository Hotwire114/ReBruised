#pragma once
// ReBruised: the player's own disc. Nothing from the game ships with the project: the launcher asks for the
// player's disc image (ISO), checks it is Black & Bruised PAL (SLES-51620) by a fingerprint of its program file, and
// takes from it only what the runtime opens as host files -- the program file (SLES_516.20) and MODULES\*.IRX -- into
// %LOCALAPPDATA%\ReBruised\disc (saves live there too). Everything else is read from the image itself.
//   settings.ini: disc_image = <path of the ISO>
#include <cstdint>
#include <filesystem>
#include <string>

namespace disc_setup
{
    constexpr const char *kElfName = "SLES_516.20";
    constexpr uint64_t kElfSize = 1446960u;
    constexpr uint64_t kElfFnv64 = 0x3389ad3015049932ull; // FNV-1a 64 of the PAL program file (a fingerprint only)

    std::filesystem::path dataDir();                  // %LOCALAPPDATA%\ReBruised\disc
    std::filesystem::path elfPath();                  // dataDir() / SLES_516.20
    // Checks the image and extracts the program file + MODULES into dataDir(). Returns "" on success, else a
    // message for the player.
    std::string prepare(const std::filesystem::path &iso);
    // true when dataDir() holds the program file of the right game and `iso` exists
    bool ready(const std::filesystem::path &iso);
    uint64_t fnv64File(const std::filesystem::path &file);
}
