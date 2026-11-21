#pragma once

// Path lookup in the ISO9660 directory of the CD image the IOP reads from (HostPathKind::CdImage). cdvdman's
// sceCdSearchFile and ioman's cdrom0: device must report the layout of that image: a sector number from any other
// layout (the virtual ISO built from extracted files, or one disc's numbers hard-coded) reads the wrong data.

#include "ps2x/iop/iop_host.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ps2x::iop::detail
{
    struct IsoImageEntry
    {
        uint32_t lsn = 0u;
        uint32_t size = 0u;
        bool directory = false;
    };

    // path: "cdrom0:\\TEKKEN\\DATA.BIN;1", "\\TEKKEN\\DATA.BIN", "TEKKEN/DATA.BIN" (case and ";n" ignored).
    inline bool lookupIsoImagePath(IopHost &host, uint64_t image, std::string_view path, IsoImageEntry &out)
    {
        if (image == 0u)
            return false;
        const size_t colon = path.find(':');
        if (colon != std::string_view::npos)
            path.remove_prefix(colon + 1u);
        std::vector<std::string> parts;
        std::string part;
        for (const char c : path)
        {
            if (c == '\\' || c == '/')
            {
                if (!part.empty())
                    parts.push_back(part);
                part.clear();
            }
            else if (c != '\0')
                part.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        }
        if (!part.empty())
            parts.push_back(part);
        for (std::string &p : parts)
        {
            const size_t semi = p.find(';');
            if (semi != std::string::npos)
                p.resize(semi);
        }
        if (parts.empty())
            return false;

        constexpr uint32_t kSector = 2048u;
        auto readBytes = [&](uint64_t offset, uint8_t *dst, size_t size)
        {
            size_t got = 0u;
            return host.readHostFile(image, offset, dst, size, got) && got == size;
        };
        auto le32 = [](const uint8_t *p)
        {
            return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8u) |
                   (static_cast<uint32_t>(p[2]) << 16u) | (static_cast<uint32_t>(p[3]) << 24u);
        };

        std::array<uint8_t, kSector> pvd{};
        if (!readBytes(16ull * kSector, pvd.data(), pvd.size()) || pvd[0] != 1u || pvd[1] != 'C' || pvd[2] != 'D')
            return false;
        IsoImageEntry current{le32(pvd.data() + 156u + 2u), le32(pvd.data() + 156u + 10u), true};

        for (size_t level = 0; level < parts.size(); ++level)
        {
            if (!current.directory || current.size == 0u || current.size > 4u * 1024u * 1024u)
                return false;
            std::vector<uint8_t> dir((current.size + kSector - 1u) / kSector * kSector);
            if (!readBytes(static_cast<uint64_t>(current.lsn) * kSector, dir.data(), dir.size()))
                return false;
            bool found = false;
            for (size_t i = 0; i < current.size;)
            {
                const uint8_t length = dir[i];
                if (length == 0u)
                {
                    i = (i / kSector + 1u) * kSector; // records do not cross sectors
                    continue;
                }
                if (i + 33u > dir.size() || i + length > dir.size())
                    break;
                const uint8_t nameLength = dir[i + 32u];
                std::string name(reinterpret_cast<const char *>(dir.data() + i + 33u),
                                 std::min<size_t>(nameLength, length > 33u ? length - 33u : 0u));
                const size_t semi = name.find(';');
                if (semi != std::string::npos)
                    name.resize(semi);
                if (!name.empty() && name.back() == '.')
                    name.pop_back();
                for (char &c : name)
                    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                if (nameLength > 1u && name == parts[level])
                {
                    current = IsoImageEntry{le32(dir.data() + i + 2u), le32(dir.data() + i + 10u), (dir[i + 25u] & 2u) != 0u};
                    found = true;
                    break;
                }
                i += length;
            }
            if (!found)
                return false;
        }
        out = current;
        return true;
    }
}
