#pragma once

// HD textures: a converted PCSX2 replacement pack (tools: SHD1 files, BC7
// with mip levels), found by the names PCSX2 gives the textures: XXH3 of
// their GS blocks and of their palette. SOTC_HD_DIR, or ./hdtextures.

#include "sotc_xxh3.h"
#include "runtime/native_tuning.h"
#include "runtime/gs/ps2_gs_psmct32.h"
#include "runtime/gs/ps2_gs_psmt4.h"
#include "runtime/gs/ps2_gs_psmt8.h"

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace sotc_hd
{
    struct Blob
    {
        uint64_t id = 0;
        uint32_t width = 0, height = 0, levels = 0;
        uint8_t alphaMin = 0, alphaMax = 255;
        std::vector<uint8_t> bytes;
    };

    inline uint64_t combine(uint64_t pixels, uint64_t palette) { return pixels ^ (palette * 0x9E3779B97F4A7C15ull); }

    // The files: the pack's (group ""), its interface ("ui") and the mods
    // ("mods/<name>"), by combine(pixels, palette); the pack's also by its
    // pixel hash alone under combine(pixels, 0). Read once.
    struct File
    {
        std::string group, path;
    };
    inline const std::unordered_map<uint64_t, std::vector<File>> &index()
    {
        static const std::unordered_map<uint64_t, std::vector<File>> files = []
        {
            std::unordered_map<uint64_t, std::vector<File>> found;
            const char *dir = std::getenv("SOTC_HD_DIR");
            const std::filesystem::path root(dir && *dir ? dir : "hdtextures");
            std::vector<std::pair<std::string, std::pair<uint64_t, uint64_t>>> names;
            std::error_code ec;
            for (auto it = std::filesystem::recursive_directory_iterator(root, ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
            {
                unsigned long long a = 0, b = 0;
                if (it->path().extension() == ".shd" && std::sscanf(it->path().filename().c_str(), "%llx-%llx", &a, &b) == 2)
                    names.emplace_back(it->path().lexically_relative(root).generic_string(), std::pair<uint64_t, uint64_t>{a, b});
            }
            std::sort(names.begin(), names.end()); // the same choice every time
            for (const auto &[name, hash] : names)
            {
                const size_t slash = name.rfind('/');
                const std::string group = slash == std::string::npos ? std::string() : name.substr(0, slash);
                const std::string path = (root / name).string();
                found[combine(hash.first, hash.second)].push_back({group, path});
                if (group.empty() && !found.count(combine(hash.first, 0)))
                    found[combine(hash.first, 0)].push_back({group, path});
            }
            std::fprintf(stderr, "[hd] %zu textures\n", names.size());
            return found;
        }();
        return files;
    }

    // What the menus have on, first of all the mods: the groups looked in, in
    // this order. `changes` counts up when it changes.
    inline const std::vector<std::string> &active(uint64_t &changes)
    {
        static std::vector<std::string> groups;
        static uint64_t count = 0;
        const native_renderer::Tuning &t = native_renderer::tuning();
        static float last[16] = {-1.0f};
        const float now[16] = {t.hdTextures ? 1.0f : 0.0f, t.hdInterface ? 1.0f : 0.0f, t.modOutfit, t.modCloak, t.modHeadband ? 1.0f : 0.0f, t.modAgro,
                               t.modMono, t.modItems, t.modSky, t.modIcons, t.modGameOver, t.modTrail, t.modHintsOff ? 1.0f : 0.0f};
        if (std::memcmp(last, now, sizeof(now)) != 0)
        {
            std::memcpy(last, now, sizeof(now));
            ++count;
            groups.clear();
            const auto pick = [](float v, std::initializer_list<const char *> names)
            {
                const size_t i = static_cast<size_t>(v + 0.5f);
                if (i >= 1u && i <= names.size())
                    groups.emplace_back(std::string("mods/") + names.begin()[i - 1u]);
            };
            pick(now[4], {"headband-rainbow"});
            pick(t.modCloak, {"cloak-ico", "cloak-rainbow"});
            pick(t.modOutfit, {"outfit-beta", "outfit-brazilian", "outfit-cursed", "outfit-pagan"});
            pick(t.modAgro, {"agro-latte", "agro-rainbow", "agro-pagan"});
            pick(t.modMono, {"mono-pagan"});
            pick(t.modItems, {"items-pagan"});
            pick(t.modSky, {"sky-cloudy", "sky-sunset"});
            pick(t.modIcons, {"icons-minimal", "icons-hard"});
            pick(t.modGameOver, {"gameover-orange", "gameover-blue", "gameover-no"});
            pick(t.modTrail, {"trail-black", "trail-blue", "trail-pink", "trail-rainbow", "trail-white"});
            pick(now[12], {"hints-off"});
            if (t.hdInterface)
                groups.emplace_back("ui");
            if (t.hdTextures)
                groups.emplace_back();
        }
        changes = count;
        return groups;
    }

    // The file for `name` in the first active group that has one.
    inline const File *find(uint64_t name)
    {
        const auto &files = index();
        const auto it = files.find(name);
        if (it == files.end())
            return nullptr;
        uint64_t changes = 0;
        for (const std::string &group : active(changes))
            for (const File &file : it->second)
                if (file.group == group)
                    return &file;
        return nullptr;
    }

    // PCSX2's hash of a 4 / 8-bit texture's first level. `pixels`: as the game
    // uploads it (swizzled: as PSMCT32), `reach` bytes of it.
    inline uint64_t pixelHash(const uint8_t *pixels, size_t reach, uint32_t psm, uint32_t w, uint32_t h, bool swizzled)
    {
        const bool four = psm == 0x14u;
        const uint32_t tbw = std::max(w / 64u, 2u);
        std::vector<uint8_t> gs(static_cast<size_t>(std::max(w, 64u)) * std::max(h, 64u) + 0x10000u, 0u);
        if (swizzled)
        {
            const uint32_t cw = std::max(w / 2u, 1u), ch = std::max(four ? h / 4u : h / 2u, 1u), bw = std::max(cw / 64u, 1u);
            for (uint32_t i = 0; i < cw * ch; ++i)
            {
                const uint32_t a = GSPSMCT32::addrPSMCT32(0, bw, i % cw, i / cw);
                if (a + 4u <= gs.size() && (i + 1u) * 4u <= reach)
                    std::memcpy(&gs[a], pixels + i * 4u, 4u);
            }
        }
        else
            for (uint32_t y = 0; y < h; ++y)
                for (uint32_t x = 0; x < w; ++x)
                {
                    const size_t at = four ? (static_cast<size_t>(y) * w + x) / 2u : static_cast<size_t>(y) * w + x;
                    if (at >= reach)
                        continue;
                    if (four)
                    {
                        const uint8_t v = (x & 1u) ? pixels[at] >> 4 : pixels[at] & 15u;
                        const uint32_t a = GSPSMT4::addrPSMT4(0, tbw, x, y);
                        if ((a >> 1) < gs.size())
                            gs[a >> 1] = static_cast<uint8_t>((a & 1u) ? (gs[a >> 1] & 15u) | (v << 4) : (gs[a >> 1] & 0xF0u) | v);
                    }
                    else if (const uint32_t a = GSPSMT8::addrPSMT8(0, tbw, x, y); a < gs.size())
                        gs[a] = pixels[at];
                }
        std::vector<uint8_t> data;
        if (w < (four ? 32u : 16u) || h < 16u) // smaller than a block: its indices
        {
            for (uint32_t y = 0; y < h; ++y)
                for (uint32_t x = 0; x < w; ++x)
                {
                    const uint32_t a = four ? GSPSMT4::addrPSMT4(0, tbw, x, y) : GSPSMT8::addrPSMT8(0, tbw, x, y);
                    data.push_back(four ? ((a & 1u) ? gs[a >> 1] >> 4 : gs[a >> 1] & 15u) : gs[a]);
                }
        }
        else
            for (uint32_t y = 0; y < h; y += 16u)
                for (uint32_t x = 0; x < w; x += four ? 32u : 16u)
                {
                    const uint32_t a = (four ? GSPSMT4::addrPSMT4(0, tbw, x, y) >> 1 : GSPSMT8::addrPSMT8(0, tbw, x, y)) & ~255u;
                    if (a + 256u > gs.size())
                        return 0;
                    data.insert(data.end(), gs.begin() + a, gs.begin() + a + 256u);
                }
        return sotc_xxh3::hash(data.data(), data.size());
    }

    // The colour of a texture's smallest level (1 x 1, BC7 mode 6 as the
    // converter makes it): its mean. False: cannot tell.
    inline bool meanColour(const std::string &path, int rgba[4])
    {
        std::ifstream file(path, std::ios::binary);
        uint8_t b[16];
        if (!file.seekg(-16, std::ios::end) || !file.read(reinterpret_cast<char *>(b), 16) || (b[0] & 127u) != 64u)
            return false;
        const auto bits = [&b](int at, int n)
        {
            int v = 0;
            for (int i = 0; i < n; ++i)
                v |= ((b[(at + i) >> 3] >> ((at + i) & 7)) & 1) << i;
            return v;
        };
        static constexpr int weights[8] = {0, 4, 9, 13, 17, 21, 26, 30}; // the first index has 3 bits
        const int w = weights[bits(65, 3)];
        for (int c = 0; c < 4; ++c)
            rgba[c] = ((64 - w) * (bits(7 + c * 14, 7) * 2 + bits(63, 1)) + w * (bits(14 + c * 14, 7) * 2 + bits(64, 1)) + 32) >> 6;
        return true;
    }

    // Files read on a thread of their own; taken by the game thread.
    class Loader
    {
    public:
        static Loader &get()
        {
            static Loader *loader = new Loader(); // its thread lives as long as the game
            return *loader;
        }
        // Again and again while it is missing: the renderer lets go of what
        // was not drawn for a while.
        void request(uint64_t id, const std::string &path)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_pending.insert(id).second) // on its way, or not readable
                return;
            m_wanted.emplace_back(id, path);
            m_wake.notify_one();
        }
        bool take(Blob &blob)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_done.empty())
                return false;
            blob = std::move(m_done.front());
            m_done.pop_front();
            m_pending.erase(blob.id);
            return true;
        }

    private:
        Loader()
        {
            std::thread([this] { run(); }).detach();
        }
        static bool read(const std::string &path, Blob &blob)
        {
            std::ifstream file(path, std::ios::binary);
            uint8_t head[16];
            if (!file.read(reinterpret_cast<char *>(head), sizeof(head)) || std::memcmp(head, "SHD1", 4) != 0)
                return false;
            uint16_t size[3];
            uint32_t bytes = 0;
            std::memcpy(size, head + 4, sizeof(size));
            std::memcpy(&bytes, head + 12, 4);
            blob.width = size[0], blob.height = size[1], blob.levels = size[2];
            blob.alphaMin = head[10], blob.alphaMax = head[11];
            uint64_t expect = 0;
            for (uint32_t l = 0, w = blob.width, h = blob.height; l < blob.levels; ++l, w = std::max(1u, w / 2u), h = std::max(1u, h / 2u))
                expect += static_cast<uint64_t>((w + 3u) / 4u) * ((h + 3u) / 4u) * 16u;
            if (!blob.width || !blob.height || blob.width > 4096u || blob.height > 4096u || !blob.levels || blob.levels > 13u || expect != bytes)
                return false;
            blob.bytes.resize(bytes);
            return static_cast<bool>(file.read(reinterpret_cast<char *>(blob.bytes.data()), bytes));
        }
        void run()
        {
            for (;;)
            {
                std::pair<uint64_t, std::string> job;
                {
                    std::unique_lock<std::mutex> lock(m_mutex);
                    m_wake.wait(lock, [this] { return !m_wanted.empty(); });
                    job = std::move(m_wanted.front());
                    m_wanted.pop_front();
                }
                Blob blob;
                blob.id = job.first;
                bool ok = false;
                try
                {
                    ok = read(job.second, blob);
                }
                catch (const std::exception &)
                {
                }
                if (!ok)
                {
                    std::fprintf(stderr, "[hd] could not read %s\n", job.second.c_str());
                    continue; // the original stays
                }
                std::lock_guard<std::mutex> lock(m_mutex);
                m_done.push_back(std::move(blob));
            }
        }
        std::mutex m_mutex;
        std::condition_variable m_wake;
        std::deque<std::pair<uint64_t, std::string>> m_wanted;
        std::deque<Blob> m_done;
        std::unordered_set<uint64_t> m_pending;
    };
}
