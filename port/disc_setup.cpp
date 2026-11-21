#include "disc_setup.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <vector>

std::string bnbLocalAppData(); // launcher/launch_win.cpp (Windows), or main.cpp

namespace disc_setup
{
namespace
{
    constexpr uint32_t kSector = 2048u;

    struct Entry
    {
        std::string name;
        uint32_t lba = 0, size = 0;
        bool dir = false;
    };

    struct Iso
    {
        std::ifstream f;
        bool read(uint64_t lba, uint32_t bytes, std::vector<uint8_t> &out)
        {
            out.assign(bytes, 0);
            f.clear();
            f.seekg(static_cast<std::streamoff>(lba * kSector));
            f.read(reinterpret_cast<char *>(out.data()), bytes);
            return static_cast<uint32_t>(f.gcount()) == bytes;
        }
        std::vector<Entry> list(uint32_t lba, uint32_t size)
        {
            std::vector<Entry> out;
            std::vector<uint8_t> d;
            if (size == 0 || size > 16u * 1024u * 1024u || !read(lba, size, d))
                return out;
            for (uint32_t pos = 0; pos < size;)
            {
                const uint8_t len = d[pos];
                if (len == 0) // records do not cross sectors: next sector
                {
                    pos = (pos / kSector + 1u) * kSector;
                    continue;
                }
                if (pos + 33u > size || pos + len > size)
                    break;
                Entry e;
                std::memcpy(&e.lba, &d[pos + 2], 4);
                std::memcpy(&e.size, &d[pos + 10], 4);
                e.dir = (d[pos + 25] & 2u) != 0;
                const uint8_t nl = d[pos + 32];
                std::string n(reinterpret_cast<const char *>(&d[pos + 33]), nl);
                if (const auto semi = n.find(';'); semi != std::string::npos)
                    n.resize(semi);
                if (!(nl == 1 && (d[pos + 33] == 0 || d[pos + 33] == 1))) // skip "." and ".."
                {
                    for (char &c : n) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                    e.name = n;
                    out.push_back(e);
                }
                pos += len;
            }
            return out;
        }
        bool root(Entry &r)
        {
            std::vector<uint8_t> pvd;
            if (!read(16, kSector, pvd) || pvd[0] != 1 || std::memcmp(&pvd[1], "CD001", 5) != 0)
                return false;
            std::memcpy(&r.lba, &pvd[156 + 2], 4);
            std::memcpy(&r.size, &pvd[156 + 10], 4);
            r.dir = true;
            return true;
        }
        bool extract(const Entry &e, const std::filesystem::path &to)
        {
            std::vector<uint8_t> d;
            if (!read(e.lba, e.size, d))
                return false;
            std::ofstream o(to, std::ios::binary | std::ios::trunc);
            o.write(reinterpret_cast<const char *>(d.data()), static_cast<std::streamsize>(d.size()));
            return static_cast<bool>(o);
        }
    };

    uint64_t fnv64(const uint8_t *p, size_t n, uint64_t h = 1469598103934665603ull)
    {
        for (size_t i = 0; i < n; ++i)
            h = (h ^ p[i]) * 1099511628211ull;
        return h;
    }
}

std::filesystem::path dataDir()
{
    const std::string base = bnbLocalAppData();
    return (base.empty() ? std::filesystem::current_path() : std::filesystem::path(base)) / "ReBruised" / "disc";
}
std::filesystem::path elfPath() { return dataDir() / kElfName; }

uint64_t fnv64File(const std::filesystem::path &file)
{
    std::ifstream f(file, std::ios::binary);
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return fnv64(d.data(), d.size());
}

static bool elfOk(const std::filesystem::path &elf)
{
    std::error_code ec;
    if (!std::filesystem::is_regular_file(elf, ec) || std::filesystem::file_size(elf, ec) != kElfSize)
        return false;
    return kElfFnv64 == 0u || fnv64File(elf) == kElfFnv64;
}

bool ready(const std::filesystem::path &iso)
{
    std::error_code ec;
    return !iso.empty() && std::filesystem::is_regular_file(iso, ec) && elfOk(elfPath()) &&
           std::filesystem::is_directory(dataDir() / "MODULES", ec);
}

std::string prepare(const std::filesystem::path &iso)
{
    Iso img;
    img.f.open(iso, std::ios::binary);
    if (!img.f)
        return "Could not open that file.";
    Entry root;
    if (!img.root(root))
        return "That file is not a PS2 disc image (no ISO 9660 volume). Use an .iso made from your own disc.";
    const std::vector<Entry> top = img.list(root.lba, root.size);
    const auto find = [](const std::vector<Entry> &v, const char *name) -> const Entry * {
        for (const Entry &e : v)
            if (e.name == name) return &e;
        return nullptr;
    };
    const Entry *elf = find(top, kElfName);
    const Entry *mods = find(top, "MODULES");
    if (!elf || !mods || !mods->dir)
        return "This is not Black & Bruised (PAL, SLES-51620). Other versions are not supported yet.";
    if (elf->size != kElfSize)
        return "This disc's program file does not match the supported version (PAL, SLES-51620).";
    std::vector<uint8_t> elfData;
    if (!img.read(elf->lba, elf->size, elfData))
        return "Could not read the disc image (is the file complete?).";
    if (kElfFnv64 != 0u && fnv64(elfData.data(), elfData.size()) != kElfFnv64)
        return "This disc's program file does not match the supported version (PAL, SLES-51620).";

    std::error_code ec;
    std::filesystem::create_directories(dataDir() / "MODULES", ec);
    if (ec)
        return "Could not create " + dataDir().string();
    {
        std::ofstream o(elfPath(), std::ios::binary | std::ios::trunc);
        o.write(reinterpret_cast<const char *>(elfData.data()), static_cast<std::streamsize>(elfData.size()));
        if (!o) return "Could not write " + elfPath().string();
    }
    for (const Entry &m : img.list(mods->lba, mods->size))
        if (!m.dir && !img.extract(m, dataDir() / "MODULES" / m.name))
            return "Could not copy " + m.name + " from the disc image.";
    return "";
}
}
