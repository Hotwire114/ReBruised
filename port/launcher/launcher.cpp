// ReBruised launcher (Black & Bruised native PC recompilation): an animated front end (raylib) for the PC settings
// and Play.
//
//   build\program\bin\BlackAndBruisedLauncher.exe   (BlackAndBruised-Launcher.cmd starts it)
//
// Reads/writes settings.ini in the project root (the same file the game reads, port/main.cpp loadSettings) and starts
// build\program\bin\BlackAndBruised.exe with the disc. Artwork: our own ReBruised logo (assets\rebruised-logo-alpha.png
// next to the exe, else port\launcher\assets); no game artwork. Fonts: Windows' own (Arial Rounded MT Bold, Segoe UI).
// Controller glyphs are vector shapes (Xbox / PlayStation / Nintendo / keyboard), chosen from the last device used and
// the controller's name, or forced by settings.ini "launcher_glyphs".
#include "raylib.h"
#include "rlgl.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iterator>
#include <sstream>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "../disc_setup.h"
#include "../include/rebruised_report.h"

std::string bnbExePath(); // launch_win.cpp
std::string bnbPickDiscImage();
void bnbOpenTarget(const std::string &target);
void bnbShowInFolder(const std::string &file);
std::string bnbDesktopDir();
bool bnbProcessAlive(unsigned long pid);
bool bnbRunWait(const std::string &cmdLine, const std::string &cwd, unsigned long timeoutMs);
std::string bnbSystemInfo();
bool bnbLaunch(const std::string &exe, const std::string &args, const std::string &cwd, const std::string &logOut,
               const std::string &logErr);
void *bnbSpawn(const std::string &cmdLine, const std::string &cwd, const std::string &logFile);
void bnbErrorBox(const std::string &text);
int bnbPoll(void *process);

namespace fs = std::filesystem;

namespace
{
// ---- virtual canvas -------------------------------------------------------------------------------------------------
constexpr float VW = 1920.0f, VH = 1080.0f;
float g_scale = 1.0f, g_offX = 0.0f, g_offY = 0.0f;

Vector2 toVirtual(Vector2 p) { return {(p.x - g_offX) / g_scale, (p.y - g_offY) / g_scale}; }
Rectangle g_clip{0, 0, VW, VH};
void scissorVirtual(float x, float y, float w, float h)
{
    g_clip = {x, y, w, h};
    BeginScissorMode(static_cast<int>(g_offX + x * g_scale), static_cast<int>(g_offY + y * g_scale),
                     static_cast<int>(w * g_scale + 1), static_cast<int>(h * g_scale + 1));
}

// ---- palette (the logo's colours) -------------------------------------------------------------------------------------
const Color BG_TOP{22, 14, 26, 255}, BG_BOT{10, 8, 14, 255};
const Color ACCENT{255, 106, 0, 255}, ACCENT_HOT{255, 150, 50, 255}, CREAM{252, 240, 224, 255}, INKC{20, 14, 10, 255};
const Color PANEL_FILL{30, 22, 30, 240}, MUTED{176, 160, 150, 255}, TEXTC{246, 238, 228, 255};

Color withA(Color c, float a) { return Color{c.r, c.g, c.b, static_cast<unsigned char>(std::clamp(a, 0.0f, 1.0f) * c.a)}; }
float smoothTo(float cur, float target, float speed, float dt) { return cur + (target - cur) * (1.0f - std::exp(-speed * dt)); }
float easeOutBack(float t)
{
    const float c1 = 1.70158f, c3 = c1 + 1.0f;
    t = std::clamp(t, 0.0f, 1.0f);
    return 1.0f + c3 * std::pow(t - 1.0f, 3.0f) + c1 * std::pow(t - 1.0f, 2.0f);
}
float easeOutCubic(float t) { t = std::clamp(t, 0.0f, 1.0f); return 1.0f - std::pow(1.0f - t, 3.0f); }

// ---- fonts ----------------------------------------------------------------------------------------------------------
Font g_head{}, g_bold{}, g_body{};
Font loadFont(const char *const *names, int size)
{
    for (int i = 0; names[i]; ++i)
    {
        const std::string p = std::string("C:/Windows/Fonts/") + names[i];
        if (fs::exists(p))
        {
            Font f = LoadFontEx(p.c_str(), size, nullptr, 0);
            if (f.texture.id != 0)
            {
                GenTextureMipmaps(&f.texture);
                SetTextureFilter(f.texture, TEXTURE_FILTER_TRILINEAR);
                return f;
            }
        }
    }
    return GetFontDefault();
}
Vector2 measure(const Font &f, const std::string &s, float size, float spacing = 0.0f) { return MeasureTextEx(f, s.c_str(), size, spacing); }
void text(const Font &f, const std::string &s, float x, float y, float size, Color c, float spacing = 0.0f)
{
    DrawTextEx(f, s.c_str(), {x, y}, size, spacing, c);
}
void textCentered(const Font &f, const std::string &s, float cx, float y, float size, Color c, float spacing = 0.0f)
{
    const Vector2 m = measure(f, s, size, spacing);
    text(f, s, cx - m.x / 2, y, size, c, spacing);
}
// the logo's lettering: fill, black outline, cream rim, drop shadow
void logoText(const Font &f, const std::string &s, float x, float y, float size, Color fill, float alpha, float spacing = 1.0f,
              bool rim = true)
{
    const float ol = size * 0.07f, rr = size * 0.13f;
    const int n = 16;
    DrawTextEx(f, s.c_str(), {x + size * 0.05f, y + size * 0.07f}, size, spacing, withA(BLACK, 0.45f * alpha));
    if (rim)
        for (int i = 0; i < n; ++i)
        {
            const float a = i * 2 * PI / n;
            DrawTextEx(f, s.c_str(), {x + std::cos(a) * rr, y + std::sin(a) * rr}, size, spacing, withA(CREAM, alpha));
        }
    for (int i = 0; i < n; ++i)
    {
        const float a = i * 2 * PI / n;
        DrawTextEx(f, s.c_str(), {x + std::cos(a) * ol, y + std::sin(a) * ol}, size, spacing, withA(INKC, alpha));
    }
    DrawTextEx(f, s.c_str(), {x, y}, size, spacing, withA(fill, alpha));
}
std::vector<std::string> wrap(const Font &f, const std::string &s, float size, float w)
{
    std::vector<std::string> lines;
    std::string line, word;
    auto flushWord = [&]()
    {
        if (word.empty()) return;
        const std::string cand = line.empty() ? word : line + " " + word;
        if (!line.empty() && measure(f, cand, size).x > w) { lines.push_back(line); line = word; }
        else line = cand;
        word.clear();
    };
    for (char ch : s)
    {
        if (ch == ' ') flushWord();
        else word += ch;
    }
    flushWord();
    if (!line.empty()) lines.push_back(line);
    return lines;
}

// ---- shapes -----------------------------------------------------------------------------------------------------------
void tri(Vector2 a, Vector2 b, Vector2 c, Color ca, Color cb, Color cc)
{
    rlSetTexture(rlGetTextureIdDefault());
    rlBegin(RL_TRIANGLES);
    rlColor4ub(ca.r, ca.g, ca.b, ca.a); rlVertex2f(a.x, a.y);
    rlColor4ub(cb.r, cb.g, cb.b, cb.a); rlVertex2f(b.x, b.y);
    rlColor4ub(cc.r, cc.g, cc.b, cc.a); rlVertex2f(c.x, c.y);
    rlEnd();
    rlSetTexture(0);
}
void arrow(Vector2 c, float s, int dir, Color col) // 0 up 1 right 2 down 3 left
{
    const float a = dir * PI / 2.0f;
    auto rot = [&](float x, float y) { return Vector2{c.x + x * std::cos(a) - y * std::sin(a), c.y + x * std::sin(a) + y * std::cos(a)}; };
    tri(rot(0, -s), rot(-s * 0.9f, s * 0.6f), rot(s * 0.9f, s * 0.6f), col, col, col);
}
// the logo's bandage cross
void bandage(Vector2 c, float len, float rot, float alpha)
{
    const float w = len * 0.34f;
    rlPushMatrix();
    rlTranslatef(c.x, c.y, 0);
    rlRotatef(rot * RAD2DEG, 0, 0, 1);
    for (float ang : {-42.0f, 42.0f})
    {
        rlPushMatrix();
        rlRotatef(ang, 0, 0, 1);
        const Rectangle r{-len / 2, -w / 2, len, w};
        DrawRectangleRounded({r.x + len * 0.02f, r.y + len * 0.04f, r.width, r.height}, 0.6f, 10, withA(BLACK, 0.35f * alpha));
        DrawRectangleRounded(r, 0.6f, 10, withA(CREAM, alpha));
        DrawRectangleRoundedLinesEx(r, 0.6f, 10, std::max(1.5f, len * 0.045f), withA(INKC, alpha));
        for (float sx : {-1.0f, 1.0f})
            for (float sy : {-1.0f, 1.0f})
                DrawCircleV({sx * len * 0.36f, sy * w * 0.18f}, std::max(1.0f, len * 0.022f), withA(Color{205, 180, 150, 255}, alpha));
        rlPopMatrix();
    }
    DrawRectangleRounded({-w * 0.42f, -w * 0.42f, w * 0.84f, w * 0.84f}, 0.3f, 6, withA(Color{238, 214, 186, 255}, alpha));
    rlPopMatrix();
}
// comic panel: shadow, cream rim, black outline, dark fill
void comicPanel(Rectangle r, float alpha)
{
    DrawRectangleRounded({r.x + 12, r.y + 16, r.width, r.height}, 0.06f, 12, withA(BLACK, 0.45f * alpha));
    DrawRectangleRounded({r.x - 10, r.y - 10, r.width + 20, r.height + 20}, 0.07f, 12, withA(CREAM, alpha));
    DrawRectangleRounded({r.x - 4, r.y - 4, r.width + 8, r.height + 8}, 0.065f, 12, withA(INKC, alpha));
    DrawRectangleRounded(r, 0.06f, 12, withA(PANEL_FILL, alpha));
}

// ---- assets -------------------------------------------------------------------------------------------------------------
fs::path g_root, g_exeDir;
Texture2D g_logo{};
void loadLogo()
{
    for (const fs::path &p : {g_exeDir / "assets" / "rebruised-logo-alpha.png", g_root / "port" / "launcher" / "assets" / "rebruised-logo-alpha.png"})
        if (fs::exists(p))
        {
            g_logo = LoadTexture(p.string().c_str());
            if (g_logo.id)
            {
                GenTextureMipmaps(&g_logo);
                SetTextureFilter(g_logo, TEXTURE_FILTER_TRILINEAR);
                return;
            }
        }
}
void drawLogo(Vector2 centre, float width, float rot, float alpha)
{
    if (!g_logo.id)
    {
        const float size = width * 0.16f;
        const Vector2 m = measure(g_head, "ReBruised", size, 2);
        logoText(g_head, "ReBruised", centre.x - m.x / 2, centre.y - m.y / 2, size, ACCENT, alpha, 2);
        return;
    }
    const float h = width * g_logo.height / g_logo.width;
    DrawTexturePro(g_logo, {0, 0, static_cast<float>(g_logo.width), static_cast<float>(g_logo.height)},
                   {centre.x + width * 0.012f, centre.y + h * 0.04f, width, h}, {width / 2, h / 2}, rot, withA(BLACK, 0.5f * alpha));
    DrawTexturePro(g_logo, {0, 0, static_cast<float>(g_logo.width), static_cast<float>(g_logo.height)},
                   {centre.x, centre.y, width, h}, {width / 2, h / 2}, rot, withA(WHITE, alpha));
}

// ---- settings model ---------------------------------------------------------------------------------------------------
struct Option
{
    std::string key, label;
    std::vector<std::pair<std::string, std::string>> values; // stored value, shown text
    std::string def, desc;
};
std::vector<Option> g_options = {
    {"framerate", "Frame rate", {{"60", "60 FPS"}, {"50", "50 FPS (PAL)"}}, "60",
     "60 runs the game's own 60 Hz timing, as in the NTSC release. 50 is the original PAL speed."},
    {"display", "Display", {{"windowed", "Windowed"}, {"borderless", "Borderless"}, {"exclusive", "Fullscreen"}}, "windowed",
     "How the game window opens. F11 or Alt+Enter switches between window and borderless while playing."},
    {"widescreen", "Aspect ratio", {{"off", "4:3"}, {"on", "16:9"}}, "off",
     "16:9 widens the view in fights and the menus; movies stay 4:3. F9 switches it while playing."},
    {"widescreen_menus", "Widescreen menus", {{"on", "On"}, {"off", "Off"}}, "on",
     "With 16:9 on: menus fill the screen too (backgrounds stretch, text and portraits keep their shape). Off: menus stay 4:3."},
    {"resolution", "Resolution", {{"auto", "Auto"}, {"native", "Native 448p"}, {"720p", "720p"}, {"1080p", "1080p"}, {"1440p", "1440p"}, {"4k", "4K"}, {"lowest", "Lowest"}}, "auto",
     "Internal render resolution. Auto picks the step that fits your monitor."},
    {"antialiasing", "Anti-aliasing", {{"fxaa", "FXAA"}, {"off", "Off"}, {"ssaa2", "SSAA 2x"}, {"ssaa2+fxaa", "SSAA 2x + FXAA"}}, "fxaa",
     "Smooths jagged edges. SSAA renders at twice the resolution and costs the most."},
    {"anisotropic", "Texture filtering", {{"off", "Off"}, {"2", "2x"}, {"4", "4x"}, {"8", "8x"}, {"16", "16x"}}, "4",
     "Anisotropic filtering keeps the ring canvas sharp at an angle."},
    {"scaling", "Window scaling", {{"area", "Smooth"}, {"sharp", "Sharp"}, {"bilinear", "Bilinear"}, {"nearest", "Pixelated"}}, "area",
     "How the finished picture is scaled to your window."},
    {"hud", "HUD", {{"hd", "HD"}, {"soft", "Soft"}, {"sharp", "Sharp"}}, "hd",
     "How the health bars, stars, icons and timer are scaled up. HD keeps their edges crisp; Soft is plain bilinear; Sharp keeps the original pixels."},
    {"sound", "Sound", {{"on", "On"}, {"off", "Off"}}, "on",
     "The game's own sound driver, running on the emulated sound processor."},
    {"launcher_glyphs", "Button prompts", {{"auto", "Auto"}, {"xbox", "Xbox"}, {"playstation", "PlayStation"}, {"nintendo", "Nintendo"}, {"keyboard", "Keyboard"}}, "auto",
     "Controller symbols in the launcher and in the game. Auto follows your controller; PlayStation keeps the game's own symbols."},
    {"show_fps", "FPS log", {{"off", "Off"}, {"on", "On"}}, "off",
     "Writes the game's frames per second to logs\\play.log every second."},
    {"debug_panel", "Debug panel", {{"off", "Off"}, {"on", "On"}}, "off",
     "Opens the runtime's debug panel at start. F1 toggles it while playing."},
};
std::map<std::string, std::string> g_values;
std::vector<std::string> g_lines;

std::string lower(std::string s) { for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; }
std::string trim(std::string s)
{
    const auto b = s.find_first_not_of(" \t\r\n"), e = s.find_last_not_of(" \t\r\n");
    return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}
void readSettings()
{
    g_values.clear();
    g_lines.clear();
    for (const Option &o : g_options) g_values[o.key] = o.def;
    g_values["movie_timeout"] = "0";
    std::ifstream in(g_root / "settings.ini");
    for (std::string line; std::getline(in, line);)
    {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        g_lines.push_back(line);
        const std::string body = line.substr(0, line.find('#'));
        const auto eq = body.find('=');
        if (eq != std::string::npos) g_values[lower(trim(body.substr(0, eq)))] = lower(trim(body.substr(eq + 1)));
    }
}
void writeSettings()
{
    std::vector<std::string> out;
    std::map<std::string, bool> seen;
    for (const std::string &line : g_lines)
    {
        const std::string body = line.substr(0, line.find('#'));
        const auto eq = body.find('=');
        if (eq != std::string::npos)
        {
            const std::string k = lower(trim(body.substr(0, eq)));
            if (g_values.count(k)) { out.push_back(k + " = " + g_values[k]); seen[k] = true; continue; }
        }
        out.push_back(line);
    }
    if (g_lines.empty()) out.push_back("# Black & Bruised - PC settings (written by the launcher; hand-editable)");
    for (const Option &o : g_options)
        if (!seen[o.key]) out.push_back(o.key + " = " + g_values[o.key]);
    if (!seen["movie_timeout"]) out.push_back("movie_timeout = " + g_values["movie_timeout"]);
    if (!seen["disc_image"] && !g_values["disc_image"].empty())
    {
        out.push_back("# your own disc image (launcher: GAME DISC)");
        out.push_back("disc_image = " + g_values["disc_image"]);
    }
    std::ofstream f(g_root / "settings.ini", std::ios::binary | std::ios::trunc);
    for (const std::string &l : out) f << l << "\n";
    g_lines = out;
}
int valueIndex(const Option &o)
{
    for (size_t i = 0; i < o.values.size(); ++i)
        if (o.values[i].first == g_values[o.key]) return static_cast<int>(i);
    for (size_t i = 0; i < o.values.size(); ++i)
        if (o.values[i].first == o.def) return static_cast<int>(i);
    return 0;
}
std::string shown(const char *key)
{
    for (const Option &o : g_options)
        if (o.key == key) return o.values[valueIndex(o)].second;
    return "";
}

// ---- controller glyphs ------------------------------------------------------------------------------------------------
enum class Style { Keyboard, Xbox, PlayStation, Nintendo };
enum class Btn { South, East, West, North, DpadUD, DpadLR, Start, Select, LB, RB, LT, RT, LStick };

const Color GLYPH_DISC{34, 30, 38, 255}, GLYPH_RIM{86, 76, 92, 255};

void keycap(const std::string &label, Vector2 c, float h)
{
    const float w = std::max(h, measure(g_bold, label, h * 0.5f).x + h * 0.55f);
    const Rectangle r{c.x - w / 2, c.y - h / 2, w, h};
    DrawRectangleRounded({r.x, r.y + 3, r.width, r.height}, 0.28f, 8, Color{8, 6, 10, 255});
    DrawRectangleRounded(r, 0.28f, 8, Color{70, 62, 74, 255});
    DrawRectangleRounded({r.x + 3, r.y + 2, r.width - 6, r.height - 8}, 0.25f, 8, Color{104, 94, 108, 255});
    DrawRectangleRoundedLinesEx(r, 0.28f, 8, 1.5f, Color{150, 138, 150, 255});
    textCentered(g_bold, label, c.x, c.y - h * 0.31f, h * 0.5f, TEXTC);
}
float keycapWidth(const std::string &label, float h) { return std::max(h, measure(g_bold, label, h * 0.5f).x + h * 0.55f); }
void disc(Vector2 c, float r)
{
    DrawCircleV({c.x, c.y + r * 0.08f}, r, Color{6, 5, 8, 220});
    DrawCircleV(c, r, GLYPH_DISC);
    DrawRing(c, r - 2.2f, r, 0, 360, 48, GLYPH_RIM);
}
void pill(const std::string &label, Vector2 c, float h, Color fill, Color fg)
{
    const float w = measure(g_bold, label, h * 0.48f).x + h * 0.8f;
    DrawRectangleRounded({c.x - w / 2, c.y - h / 2 + 2, w, h}, 1.0f, 12, Color{6, 5, 8, 220});
    DrawRectangleRounded({c.x - w / 2, c.y - h / 2, w, h}, 1.0f, 12, fill);
    DrawRectangleRoundedLinesEx({c.x - w / 2, c.y - h / 2, w, h}, 1.0f, 12, 1.5f, GLYPH_RIM);
    textCentered(g_bold, label, c.x, c.y - h * 0.3f, h * 0.48f, fg);
}
float pillWidth(const std::string &label, float h) { return measure(g_bold, label, h * 0.48f).x + h * 0.8f; }
struct Face { std::string label; Color col; };
Face faceOf(Style s, Btn b)
{
    if (s == Style::Xbox)
        switch (b)
        {
        case Btn::South: return {"A", {96, 196, 72, 255}};
        case Btn::East: return {"B", {232, 74, 72, 255}};
        case Btn::West: return {"X", {70, 140, 236, 255}};
        default: return {"Y", {242, 196, 48, 255}};
        }
    if (s == Style::Nintendo) // positions: south B, east A, west Y, north X
        switch (b)
        {
        case Btn::South: return {"B", TEXTC};
        case Btn::East: return {"A", TEXTC};
        case Btn::West: return {"Y", TEXTC};
        default: return {"X", TEXTC};
        }
    return {"", TEXTC};
}
float glyph(Style s, Btn b, Vector2 c, float size)
{
    const float r = size / 2;
    const bool face = b == Btn::South || b == Btn::East || b == Btn::West || b == Btn::North;
    if (s == Style::Keyboard)
    {
        std::string k;
        switch (b)
        {
        case Btn::South: k = "Enter"; break;
        case Btn::East: k = "Esc"; break;
        case Btn::West: k = "Z"; break;
        case Btn::North: k = "V"; break;
        case Btn::Start: k = "Enter"; break;
        case Btn::Select: k = "Tab"; break;
        case Btn::LB: k = "Q"; break;
        case Btn::RB: k = "E"; break;
        case Btn::LT: k = "L Shift"; break;
        case Btn::RT: k = "R Shift"; break;
        case Btn::LStick: k = "W A S D"; break;
        case Btn::DpadUD:
        case Btn::DpadLR:
        {
            const float w = size * 0.95f;
            keycap("", {c.x - w * 0.55f, c.y}, size);
            keycap("", {c.x + w * 0.55f, c.y}, size);
            arrow({c.x - w * 0.55f, c.y - 1}, size * 0.16f, b == Btn::DpadUD ? 0 : 3, TEXTC);
            arrow({c.x + w * 0.55f, c.y - 1}, size * 0.16f, b == Btn::DpadUD ? 2 : 1, TEXTC);
            return w * 2.1f;
        }
        }
        keycap(k, c, size);
        return keycapWidth(k, size);
    }
    if (face)
    {
        disc(c, r);
        if (s == Style::PlayStation)
        {
            const float t = std::max(2.5f, size * 0.085f);
            if (b == Btn::South)
            {
                const Color col{124, 178, 232, 255};
                const float d = r * 0.42f;
                DrawLineEx({c.x - d, c.y - d}, {c.x + d, c.y + d}, t, col);
                DrawLineEx({c.x - d, c.y + d}, {c.x + d, c.y - d}, t, col);
            }
            else if (b == Btn::East)
                DrawRing(c, r * 0.44f - t / 2, r * 0.44f + t / 2, 0, 360, 48, Color{255, 102, 102, 255});
            else if (b == Btn::West)
            {
                const float d = r * 0.40f;
                DrawRectangleLinesEx({c.x - d, c.y - d, d * 2, d * 2}, t, Color{228, 160, 216, 255});
            }
            else
            {
                const Color col{64, 224, 176, 255};
                const Vector2 p0{c.x, c.y - r * 0.46f}, p1{c.x - r * 0.48f, c.y + r * 0.34f}, p2{c.x + r * 0.48f, c.y + r * 0.34f};
                DrawLineEx(p0, p1, t, col); DrawLineEx(p1, p2, t, col); DrawLineEx(p2, p0, t, col);
                DrawCircleV(p0, t / 2, col); DrawCircleV(p1, t / 2, col); DrawCircleV(p2, t / 2, col);
            }
        }
        else
        {
            const Face f = faceOf(s, b);
            textCentered(g_bold, f.label, c.x, c.y - size * 0.36f, size * 0.66f, f.col);
        }
        return size;
    }
    switch (b)
    {
    case Btn::DpadUD:
    case Btn::DpadLR:
    {
        const float a = size * 0.30f, l = size * 0.5f;
        const Color base{70, 62, 74, 255}, hi = TEXTC;
        DrawRectangleRounded({c.x - a / 2, c.y - l, a, l * 2}, 0.3f, 6, base);
        DrawRectangleRounded({c.x - l, c.y - a / 2, l * 2, a}, 0.3f, 6, base);
        if (b == Btn::DpadUD) { arrow({c.x, c.y - l * 0.62f}, a * 0.32f, 0, hi); arrow({c.x, c.y + l * 0.62f}, a * 0.32f, 2, hi); }
        else { arrow({c.x - l * 0.62f, c.y}, a * 0.32f, 3, hi); arrow({c.x + l * 0.62f, c.y}, a * 0.32f, 1, hi); }
        return size;
    }
    case Btn::LStick:
    {
        DrawCircleV({c.x, c.y + 2}, r, Color{6, 5, 8, 220});
        DrawCircleV(c, r, Color{54, 48, 58, 255});
        DrawCircleV(c, r * 0.72f, Color{82, 74, 86, 255});
        DrawRing(c, r * 0.72f - 1.5f, r * 0.72f, 0, 360, 40, Color{130, 120, 134, 255});
        textCentered(g_bold, s == Style::PlayStation ? "L" : "LS", c.x, c.y - size * 0.24f, size * 0.42f, TEXTC);
        return size;
    }
    case Btn::Start:
    case Btn::Select:
    {
        if (s == Style::Xbox)
        {
            disc(c, r * 0.82f);
            if (b == Btn::Start)
                for (int i = -1; i <= 1; ++i)
                    DrawLineEx({c.x - r * 0.36f, c.y + i * r * 0.24f}, {c.x + r * 0.36f, c.y + i * r * 0.24f}, std::max(2.0f, size * 0.06f), TEXTC);
            else
            {
                DrawRectangleRoundedLinesEx({c.x - r * 0.38f, c.y - r * 0.30f, r * 0.46f, r * 0.42f}, 0.25f, 4, 2.0f, TEXTC);
                DrawRectangleRoundedLinesEx({c.x - r * 0.10f, c.y - r * 0.12f, r * 0.46f, r * 0.42f}, 0.25f, 4, 2.0f, TEXTC);
            }
            return size * 0.82f;
        }
        if (s == Style::Nintendo)
        {
            disc(c, r * 0.72f);
            const float t = std::max(2.5f, size * 0.08f), d = r * 0.34f;
            DrawLineEx({c.x - d, c.y}, {c.x + d, c.y}, t, TEXTC);
            if (b == Btn::Start) DrawLineEx({c.x, c.y - d}, {c.x, c.y + d}, t, TEXTC);
            return size * 0.72f;
        }
        const std::string l = b == Btn::Start ? "OPTIONS" : "SHARE";
        pill(l, c, size * 0.62f, Color{54, 48, 58, 255}, TEXTC);
        return pillWidth(l, size * 0.62f);
    }
    default:
    {
        std::string l;
        if (s == Style::Xbox) l = b == Btn::LB ? "LB" : b == Btn::RB ? "RB" : b == Btn::LT ? "LT" : "RT";
        else if (s == Style::PlayStation) l = b == Btn::LB ? "L1" : b == Btn::RB ? "R1" : b == Btn::LT ? "L2" : "R2";
        else l = b == Btn::LB ? "L" : b == Btn::RB ? "R" : b == Btn::LT ? "ZL" : "ZR";
        const bool trigger = b == Btn::LT || b == Btn::RT;
        const float h = size * 0.74f, w = std::max(size * 1.15f, measure(g_bold, l, h * 0.55f).x + h * 0.7f);
        const Rectangle rr{c.x - w / 2, c.y - h / 2, w, h};
        DrawRectangleRounded({rr.x, rr.y + 2, rr.width, rr.height}, trigger ? 0.55f : 0.9f, 10, Color{6, 5, 8, 220});
        DrawRectangleRounded(rr, trigger ? 0.55f : 0.9f, 10, Color{54, 48, 58, 255});
        DrawRectangleRoundedLinesEx(rr, trigger ? 0.55f : 0.9f, 10, 1.5f, GLYPH_RIM);
        textCentered(g_bold, l, c.x, c.y - h * 0.33f, h * 0.55f, TEXTC);
        return w;
    }
    }
}
float glyphWidth(Style s, Btn b, float size)
{
    if (s == Style::Keyboard)
    {
        switch (b)
        {
        case Btn::South: case Btn::Start: return keycapWidth("Enter", size);
        case Btn::East: return keycapWidth("Esc", size);
        case Btn::DpadUD: case Btn::DpadLR: return size * 0.95f * 2.1f;
        default: return size;
        }
    }
    if (b == Btn::Start || b == Btn::Select)
        return s == Style::PlayStation ? pillWidth(b == Btn::Start ? "OPTIONS" : "SHARE", size * 0.62f) : size * 0.82f;
    return size;
}

// ---- input -------------------------------------------------------------------------------------------------------------
Style g_lastDevice = Style::Keyboard;
Style padStyleFromName()
{
    if (!IsGamepadAvailable(0)) return Style::Xbox;
    const std::string n = lower(GetGamepadName(0) ? GetGamepadName(0) : "");
    for (const char *k : {"ps4", "ps5", "dualsense", "dualshock", "sony", "playstation", "wireless controller"})
        if (n.find(k) != std::string::npos) return Style::PlayStation;
    for (const char *k : {"nintendo", "switch", "joy-con", "pro controller"})
        if (n.find(k) != std::string::npos) return Style::Nintendo;
    return Style::Xbox;
}
Style promptStyle()
{
    const std::string g = g_values["launcher_glyphs"];
    if (g == "xbox") return Style::Xbox;
    if (g == "playstation") return Style::PlayStation;
    if (g == "nintendo") return Style::Nintendo;
    if (g == "keyboard") return Style::Keyboard;
    return g_lastDevice;
}
struct Input { bool up = false, down = false, left = false, right = false, confirm = false, back = false; };
float g_repeatTimer = 0.0f;
int g_heldDir = -1;
Input pollInput(float dt)
{
    Input in;
    auto key = [](int k) { return IsKeyPressed(k) || IsKeyPressedRepeat(k); };
    const bool kUp = key(KEY_UP) || key(KEY_W), kDown = key(KEY_DOWN) || key(KEY_S);
    const bool kLeft = key(KEY_LEFT) || key(KEY_A), kRight = key(KEY_RIGHT) || key(KEY_D);
    const bool kOk = IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_SPACE) || IsKeyPressed(KEY_KP_ENTER);
    const bool kBack = IsKeyPressed(KEY_ESCAPE) || IsKeyPressed(KEY_BACKSPACE);
    if (kUp || kDown || kLeft || kRight || kOk || kBack) g_lastDevice = Style::Keyboard;
    in = {kUp, kDown, kLeft, kRight, kOk, kBack};
    if (IsGamepadAvailable(0))
    {
        const Style ps = padStyleFromName();
        const int okBtn = ps == Style::Nintendo ? GAMEPAD_BUTTON_RIGHT_FACE_RIGHT : GAMEPAD_BUTTON_RIGHT_FACE_DOWN;
        const int backBtn = ps == Style::Nintendo ? GAMEPAD_BUTTON_RIGHT_FACE_DOWN : GAMEPAD_BUTTON_RIGHT_FACE_RIGHT;
        const bool pOk = IsGamepadButtonPressed(0, okBtn) || IsGamepadButtonPressed(0, GAMEPAD_BUTTON_MIDDLE_RIGHT);
        const bool pBack = IsGamepadButtonPressed(0, backBtn);
        const float ax = GetGamepadAxisMovement(0, GAMEPAD_AXIS_LEFT_X), ay = GetGamepadAxisMovement(0, GAMEPAD_AXIS_LEFT_Y);
        int dir = -1;
        if (IsGamepadButtonDown(0, GAMEPAD_BUTTON_LEFT_FACE_UP) || ay < -0.6f) dir = 0;
        else if (IsGamepadButtonDown(0, GAMEPAD_BUTTON_LEFT_FACE_DOWN) || ay > 0.6f) dir = 2;
        else if (IsGamepadButtonDown(0, GAMEPAD_BUTTON_LEFT_FACE_LEFT) || ax < -0.6f) dir = 3;
        else if (IsGamepadButtonDown(0, GAMEPAD_BUTTON_LEFT_FACE_RIGHT) || ax > 0.6f) dir = 1;
        bool fire = false;
        if (dir != g_heldDir) { g_heldDir = dir; g_repeatTimer = 0.38f; fire = dir >= 0; }
        else if (dir >= 0) { g_repeatTimer -= dt; if (g_repeatTimer <= 0) { g_repeatTimer = 0.09f; fire = true; } }
        if (fire) { in.up |= dir == 0; in.right |= dir == 1; in.down |= dir == 2; in.left |= dir == 3; }
        if (pOk || pBack || fire) g_lastDevice = ps;
        in.confirm |= pOk;
        in.back |= pBack;
    }
    return in;
}

// ---- background: comic burst, halftone corners, drifting bandages ----------------------------------------------------------
struct Drifter { Vector2 p; float len, rot, spin, vx, vy, a; };
std::vector<Drifter> g_drift;
void initBackground()
{
    std::mt19937 rng(11);
    std::uniform_real_distribution<float> U(0, 1);
    for (int i = 0; i < 16; ++i)
    {
        const float depth = U(rng);
        g_drift.push_back({{U(rng) * VW, U(rng) * VH}, 40 + depth * 70, U(rng) * PI, (U(rng) - 0.5f) * 0.5f,
                           (U(rng) - 0.5f) * 14, -8 - depth * 16, 0.05f + depth * 0.09f});
    }
}
void halftone(float cx, float cy, float radius, float spacing, Color col, float alpha)
{
    for (float y = cy - radius; y <= cy + radius; y += spacing)
        for (float x = cx - radius; x <= cx + radius; x += spacing)
        {
            const float d = std::sqrt((x - cx) * (x - cx) + (y - cy) * (y - cy)) / radius;
            if (d >= 1.0f) continue;
            DrawCircleV({x, y}, spacing * 0.42f * (1.0f - d), withA(col, alpha));
        }
}
void drawBackground(float t, float dt, Vector2 burstCentre)
{
    DrawRectangleGradientV(0, 0, static_cast<int>(VW), static_cast<int>(VH), BG_TOP, BG_BOT);
    // comic burst behind the logo
    const int rays = 28;
    for (int i = 0; i < rays; ++i)
    {
        if (i % 2) continue;
        const float a0 = t * 0.04f + i * 2 * PI / rays, a1 = a0 + 2 * PI / rays;
        const float R = 1700;
        const Color c0 = withA(ACCENT, 0.10f), c1 = withA(ACCENT, 0.0f);
        tri(burstCentre, {burstCentre.x + std::cos(a0) * R, burstCentre.y + std::sin(a0) * R},
            {burstCentre.x + std::cos(a1) * R, burstCentre.y + std::sin(a1) * R}, c0, c1, c1);
    }
    // warm glow
    DrawCircleGradient(static_cast<int>(burstCentre.x), static_cast<int>(burstCentre.y), 620, withA(ACCENT, 0.16f), withA(ACCENT, 0.0f));
    // halftone corners
    halftone(0, VH, 520, 26, ACCENT, 0.22f);
    halftone(VW, 0, 460, 26, CREAM, 0.08f);
    // drifting bandages
    for (Drifter &d : g_drift)
    {
        d.p.x += d.vx * dt; d.p.y += d.vy * dt; d.rot += d.spin * dt;
        if (d.p.y < -120) { d.p.y += VH + 240; }
        if (d.p.x < -120) d.p.x += VW + 240;
        if (d.p.x > VW + 120) d.p.x -= VW + 240;
        bandage(d.p, d.len, d.rot, d.a);
    }
    // vignette
    DrawRectangleGradientV(0, static_cast<int>(VH) - 300, static_cast<int>(VW), 300, withA(BG_BOT, 0.0f), withA(BG_BOT, 0.85f));
}

// ---- pages -------------------------------------------------------------------------------------------------------------------
enum class Page { Main, Options, Controls, Disc, Report, Build };
struct MainItem { std::string label, desc; };
const std::vector<MainItem> g_main = {
    {"PLAY", "Into the ring with your PC settings."},
    {"OPTIONS", "Frame rate, display, widescreen, resolution and more."},
    {"CONTROLS", "Your controller and keyboard, button by button."},
    {"GAME DISC", "Your own Black & Bruised disc image: the game is read from it."},
    {"REPORT A PROBLEM", "Something wrong? Collect the logs that show what happened."},
    {"QUIT", "See you next round."},
};
struct State
{
    Page page = Page::Main;
    float pageT = 1.0f;
    int mainSel = 0, optSel = 0;
    float selPop[8] = {1, 0, 0, 0, 0, 0, 0, 0}; // per item 0..1 selection animation
    std::string discMsg;                         // Disc page: result of the last check
    float optBarY = 0, optScroll = 0, valueNudge = 0;
    float descT = 1.0f;
    int lastMainSel = 0;
    bool quit = false;
    std::string toast;
    float toastT = 0;
    float introT = 0;
};
State S;
void showToast(const std::string &t) { S.toast = t; S.toastT = 3.5f; }
void goPage(Page p) { S.page = p; S.pageT = 0; }
void openReport(bool fromCrash); // report page, below

fs::path gameExe() { return g_exeDir / "BlackAndBruised.exe"; }
fs::path gameElf() { return disc_setup::elfPath(); }
fs::path gameIso() { return fs::path(g_values["disc_image"]); }
bool discReady() { return disc_setup::ready(gameIso()); }

// ---- building the game on this PC (release package) ----------------------------------------------------------------
// A release (tools/make_release.py) has no game code: kit\ holds the runtime and tools, and the first PLAY recompiles the
// game from the player's disc (kit\tools\player_build.py) into BlackAndBruised.exe. build-game\built_id.txt records the
// kit it was built with; a new release (other kit\build_id.txt) builds again. A development tree has no kit\.
std::string firstLine(const fs::path &p)
{
    std::ifstream f(p);
    std::string s;
    std::getline(f, s);
    return trim(s);
}
bool releaseMode() { return fs::exists(g_root / "kit" / "build_id.txt"); }
bool needsBuild()
{
    if (!releaseMode()) return false;
    return !fs::exists(gameExe()) || firstLine(g_root / "build-game" / "built_id.txt") != firstLine(g_root / "kit" / "build_id.txt");
}
struct Build
{
    void *proc = nullptr;
    bool running = false, failed = false, done = false;
    int pct = 0;
    std::string text, error;
    float pollT = 0, shownPct = 0, startT = 0;
};
Build B;
void startBuild()
{
    std::error_code ec;
    fs::create_directories(g_root / "build-game", ec);
    {   // a folder players cannot write to (Program Files, a read-only drive): say so instead of a bare exit code
        std::ofstream probe(g_root / "build-game" / "write-test.tmp");
        const bool writable = static_cast<bool>(probe);
        probe.close();
        fs::remove(g_root / "build-game" / "write-test.tmp", ec);
        if (!writable)
        {
            B = Build{};
            B.failed = true;
            B.error = "ReBruised cannot write to its folder (" + g_root.string() + "). Move the ReBruised folder somewhere "
                      "you can write to, such as Documents or C:\\Games (not Program Files), and start it again.";
            goPage(Page::Build);
            return;
        }
    }
    fs::remove(g_root / "build-game" / "progress.txt", ec);
    const fs::path py = g_root / "python" / "python.exe";
    const std::string cmd = "\"" + py.string() + "\" -I \"" + (g_root / "kit" / "tools" / "player_build.py").string() +
                            "\" --root \"" + g_root.string() + "\" --elf \"" + gameElf().string() + "\"";
    B = Build{};
    B.proc = bnbSpawn(cmd, g_root.string(), (g_root / "build-game" / "build.log").string());
    B.running = B.proc != nullptr;
    B.failed = !B.running;
    B.text = B.running ? "Starting" : "";
    B.error = B.running ? "" : "Could not start the build (python\\python.exe missing?)";
    goPage(Page::Build);
}
void pollBuild(float dt)
{
    if (!B.running) return;
    B.startT += dt;
    B.pollT -= dt;
    if (B.pollT > 0) return;
    B.pollT = 0.25f;
    std::ifstream f(g_root / "build-game" / "progress.txt");
    std::string a, b;
    if (std::getline(f, a) && std::getline(f, b))
    {
        if (a == "error") B.error = b;
        else { B.pct = std::atoi(a.c_str()); B.text = b; }
    }
    const int code = bnbPoll(B.proc);
    if (code < 0) return;
    B.proc = nullptr;
    B.running = false;
    if (code == 0 && !needsBuild())
    {
        B.done = true;
        B.pct = 100;
        showToast("Game built from your disc. Have a good fight!");
        if (S.page == Page::Build) goPage(Page::Main);
    }
    else
    {
        B.failed = true;
        if (B.error.empty()) B.error = "The build stopped (exit code " + std::to_string(code) + ").";
    }
}

bool play()
{
    if (!discReady())
    {
        S.discMsg = "Select your disc image first.";
        goPage(Page::Disc);
        return false;
    }
    if (B.running) { goPage(Page::Build); return false; }
    if (needsBuild()) { startBuild(); return false; }
    if (!fs::exists(gameExe()))
    {
        // no kit\ and no development tree: started from inside the zip (Windows extracts only the exe)
        showToast(fs::exists(g_root / "tools" / "build_program.ps1") ? "The game is not built yet: run tools\\build_program.ps1"
                                                                      : "Extract the whole ReBruised zip to a folder first");
        return false;
    }
    writeSettings();
    std::error_code ec;
    fs::create_directories(g_root / "logs", ec);
    if (!bnbLaunch(gameExe().string(), "\"" + gameElf().string() + "\" \"" + gameIso().string() + "\"", g_root.string(),
                   (g_root / "logs" / "play.log").string(), (g_root / "logs" / "play.err").string()))
    {
        showToast("Could not start the game");
        return false;
    }
    S.quit = true; // straight into the game: the launcher closes
    return true;
}

void drawPrompts(const std::vector<std::pair<Btn, std::string>> &items)
{
    const Style st = promptStyle();
    DrawRectangle(0, static_cast<int>(VH) - 92, static_cast<int>(VW), 92, Color{8, 6, 10, 230});
    DrawRectangle(0, static_cast<int>(VH) - 92, static_cast<int>(VW), 4, ACCENT);
    float x = 90;
    const float cy = VH - 44, size = 46;
    for (const auto &[b, label] : items)
    {
        const float w = glyphWidth(st, b, size);
        glyph(st, b, {x + w / 2, cy}, size);
        x += w + 14;
        text(g_bold, label, x, cy - 18, 34, TEXTC);
        x += measure(g_bold, label, 34).x + 46;
    }
    // current settings on the right
    const std::string chips[] = {shown("framerate"), std::string(g_values["widescreen"] == "on" ? "16:9" : "4:3"), shown("resolution"), shown("display")};
    float cx = VW - 90;
    for (int i = 3; i >= 0; --i)
    {
        const Vector2 m = measure(g_bold, chips[i], 26);
        cx -= m.x + 32;
        DrawRectangleRounded({cx, cy - 21, m.x + 32, 42}, 0.5f, 8, Color{36, 28, 36, 255});
        DrawRectangleRoundedLinesEx({cx, cy - 21, m.x + 32, 42}, 0.5f, 8, 1.5f, withA(ACCENT, 0.7f));
        text(g_bold, chips[i], cx + 16, cy - 14, 26, CREAM);
        cx -= 12;
    }
}

// ---- main menu -----------------------------------------------------------------------------------------------------------
void drawMain(float t, float dt, float alpha, Input &in, Vector2 mouse, bool click)
{
    const float cx = VW / 2, y0 = 480, step = 74;
    const int n = static_cast<int>(g_main.size());
    if (in.up) S.mainSel = (S.mainSel + n - 1) % n;
    if (in.down) S.mainSel = (S.mainSel + 1) % n;
    for (int i = 0; i < n; ++i)
    {
        const Rectangle r{cx - 330, y0 + i * step - 6, 660, step - 4};
        const bool moved = GetMouseDelta().x != 0 || GetMouseDelta().y != 0;
        if (CheckCollisionPointRec(mouse, r) && moved) S.mainSel = i;
        if (CheckCollisionPointRec(mouse, r) && click) { S.mainSel = i; in.confirm = true; }
    }
    if (S.mainSel != S.lastMainSel) { S.lastMainSel = S.mainSel; S.descT = 0; }
    if (in.confirm)
    {
        if (S.mainSel == 0) play();
        else if (S.mainSel == 1) goPage(Page::Options);
        else if (S.mainSel == 2) goPage(Page::Controls);
        else if (S.mainSel == 3) { S.discMsg.clear(); goPage(Page::Disc); }
        else if (S.mainSel == 4) openReport(false);
        else S.quit = true;
    }
    if (in.back) S.quit = true;

    // logo: drops in with a bounce, then a slow bob and tilt
    const float intro = easeOutBack(std::clamp(S.introT / 0.8f, 0.0f, 1.0f));
    const float logoW = 1040 * (0.75f + 0.25f * intro);
    drawLogo({cx, 250 + std::sin(t * 1.5f) * 7 - (1 - intro) * 80}, logoW, std::sin(t * 0.9f) * 1.2f, alpha * std::clamp(S.introT / 0.3f, 0.0f, 1.0f));
    const float subA = alpha * easeOutCubic(std::clamp((S.introT - 0.5f) / 0.5f, 0.0f, 1.0f));
    textCentered(g_bold, "BLACK & BRUISED  -  NATIVE PC RECOMPILATION", cx, 448, 26, withA(MUTED, subA), 4);

    for (int i = 0; i < n; ++i)
    {
        S.selPop[i] = smoothTo(S.selPop[i], i == S.mainSel ? 1.0f : 0.0f, 14, dt);
        const float d = easeOutCubic(std::clamp((S.introT - 0.45f - i * 0.08f) / 0.45f, 0.0f, 1.0f));
        const float pop = S.selPop[i];
        const float size = 60 + 12 * easeOutBack(pop);
        const std::string &label = g_main[i].label;
        const Vector2 m = measure(g_head, label, size, 3);
        const float y = y0 + i * step + (step - m.y) / 2 - 8 + (1 - d) * 40;
        const Color fill = Color{static_cast<unsigned char>(CREAM.r + (ACCENT.r - CREAM.r) * pop),
                                 static_cast<unsigned char>(CREAM.g + (ACCENT.g - CREAM.g) * pop),
                                 static_cast<unsigned char>(CREAM.b + (ACCENT.b - CREAM.b) * pop), 255};
        logoText(g_head, label, cx - m.x / 2, y, size, fill, alpha * d * (0.75f + 0.25f * pop), 3, pop > 0.5f);
        if (pop > 0.02f)
        {
            // bandages fly in at both sides of the selected item
            const float off = m.x / 2 + 70 + (1 - pop) * 160;
            const float spin = (1 - pop) * 2.5f;
            bandage({cx - off, y + m.y / 2 + 4}, 62, -0.25f - spin + std::sin(t * 2.0f) * 0.08f, alpha * d * pop);
            bandage({cx + off, y + m.y / 2 + 4}, 62, 0.25f + spin - std::sin(t * 2.0f) * 0.08f, alpha * d * pop);
        }
    }
    // description of the selected item
    S.descT = std::min(1.0f, S.descT + dt * 4);
    const float da = alpha * easeOutCubic(S.descT) * easeOutCubic(std::clamp((S.introT - 0.8f) / 0.4f, 0.0f, 1.0f));
    textCentered(g_body, g_main[S.mainSel].desc, cx, y0 + n * step + 10 + (1 - easeOutCubic(S.descT)) * 10, 32, withA(TEXTC, da * 0.85f));
}

// ---- sub-page header ---------------------------------------------------------------------------------------------------
void pageHeader(const std::string &title, float t, float alpha)
{
    drawLogo({250, 92 + std::sin(t * 1.5f) * 4}, 380, 0, alpha);
    const Vector2 m = measure(g_head, title, 96, 3);
    logoText(g_head, title, VW / 2 - m.x / 2, 58, 96, ACCENT, alpha, 3);
}

// ---- options page --------------------------------------------------------------------------------------------------------
void drawOptions(float t, float dt, float alpha, Input &in, Vector2 mouse, bool click)
{
    const int rows = static_cast<int>(g_options.size()) + 1;
    const Rectangle panel{330, 200, 1260, 720};
    const float x0 = panel.x + 60, y0 = panel.y + 34, step = 58, visible = 9;
    const float vx = panel.x + 760, vw = 360;
    if (in.up) S.optSel = (S.optSel + rows - 1) % rows;
    if (in.down) S.optSel = (S.optSel + 1) % rows;
    int change = 0;
    if (in.left) change = -1;
    if (in.right || (in.confirm && S.optSel < rows - 1)) change = 1;
    for (int i = 0; i < rows; ++i)
    {
        const float y = y0 + (i - S.optScroll) * step;
        if (y < y0 - 10 || y > y0 + (visible - 0.5f) * step) continue;
        const Rectangle r{panel.x + 20, y - 4, panel.width - 40, step - 4};
        if (CheckCollisionPointRec(mouse, r) && (GetMouseDelta().x != 0 || GetMouseDelta().y != 0)) S.optSel = i;
        if (CheckCollisionPointRec(mouse, r) && click)
        {
            S.optSel = i;
            if (i == rows - 1) in.confirm = true;
            else change = mouse.x < vx + vw / 2 ? -1 : 1;
        }
    }
    const float wheel = GetMouseWheelMove();
    if (wheel != 0) S.optSel = std::clamp(S.optSel - static_cast<int>(wheel), 0, rows - 1);
    if (change != 0 && S.optSel < rows - 1)
    {
        Option &o = g_options[S.optSel];
        const int n = static_cast<int>(o.values.size());
        g_values[o.key] = o.values[(valueIndex(o) + change + n) % n].first;
        S.valueNudge = static_cast<float>(change);
        writeSettings();
    }
    if (in.confirm && S.optSel == rows - 1)
    {
        for (const Option &o : g_options) g_values[o.key] = o.def;
        writeSettings();
        showToast("Defaults restored");
    }
    if (in.back) { writeSettings(); goPage(Page::Main); }
    S.valueNudge = smoothTo(S.valueNudge, 0, 14, dt);
    const float targetScroll = std::clamp(static_cast<float>(S.optSel) - (visible - 3), 0.0f, std::max(0.0f, rows - visible));
    S.optScroll = smoothTo(S.optScroll, targetScroll, 12, dt);

    pageHeader("OPTIONS", t, alpha);
    const float pin = easeOutBack(std::clamp(S.pageT * 1.4f, 0.0f, 1.0f));
    const Rectangle p{panel.x, panel.y + (1 - pin) * 60, panel.width, panel.height};
    comicPanel(p, alpha);
    const float selY = y0 + (S.optSel - S.optScroll) * step;
    S.optBarY = smoothTo(S.optBarY == 0 ? selY : S.optBarY, selY, 18, dt);
    scissorVirtual(p.x, y0 - 6 + (p.y - panel.y), p.width, visible * step + 2);
    // selected row: orange pill with black outline
    const Rectangle bar{p.x + 24, S.optBarY - 4 + (p.y - panel.y), p.width - 48, step - 6};
    DrawRectangleRounded(bar, 0.5f, 12, withA(INKC, alpha));
    DrawRectangleRounded({bar.x + 3, bar.y + 3, bar.width - 6, bar.height - 6}, 0.5f, 12, withA(ACCENT, alpha));
    for (int i = 0; i < rows; ++i)
    {
        const float y = y0 + (i - S.optScroll) * step + (p.y - panel.y);
        const float d = easeOutCubic(std::clamp(S.pageT * 2.2f - i * 0.05f, 0.0f, 1.0f));
        const bool sel = i == S.optSel;
        const Color c = sel ? INKC : TEXTC;
        if (i == rows - 1)
        {
            text(g_bold, "Restore defaults", x0, y + 4, 36, withA(c, alpha * d));
            continue;
        }
        const Option &o = g_options[i];
        text(g_bold, o.label, x0, y + 4, 36, withA(c, alpha * d));
        const std::string v = o.values[valueIndex(o)].second;
        const float nudge = sel ? S.valueNudge * 26 : 0;
        const Color ac = sel ? INKC : withA(MUTED, 0.9f);
        arrow({vx, y + 24}, 10, 3, withA(ac, alpha * d));
        arrow({vx + vw, y + 24}, 10, 1, withA(ac, alpha * d));
        const Vector2 m = measure(g_bold, v, 34);
        text(g_bold, v, vx + vw / 2 - m.x / 2 + nudge, y + 6, 34, withA(sel ? INKC : ACCENT_HOT, alpha * d * (1.0f - std::fabs(S.valueNudge) * 0.5f)));
    }
    EndScissorMode();
    scissorVirtual(0, 0, VW, VH);
    // description strip at the bottom of the panel
    const Option *o = S.optSel < rows - 1 ? &g_options[S.optSel] : nullptr;
    const std::string desc = o ? o->desc : "Put every option back to its default (60 FPS, windowed, 4:3, auto resolution, FXAA).";
    const float dy = p.y + p.height - 150;
    DrawRectangle(static_cast<int>(p.x + 30), static_cast<int>(dy), static_cast<int>(p.width - 60), 3, withA(ACCENT, alpha * 0.8f));
    float ly = dy + 22;
    for (const std::string &line : wrap(g_body, desc, 32, p.width - 120))
    {
        text(g_body, line, p.x + 60, ly, 32, withA(TEXTC, alpha * 0.9f));
        ly += 42;
    }
}

// ---- game disc page ------------------------------------------------------------------------------------------------------
// Nothing from the game ships with ReBruised: the player points it at their own disc image. Shown at the first start
// and whenever the image is missing.
void drawDisc(float t, float alpha, Input &in, Vector2 mouse, bool click)
{
    const bool ready = discReady();
    pageHeader("GAME DISC", t, alpha);
    const float pin = easeOutBack(std::clamp(S.pageT * 1.4f, 0.0f, 1.0f));
    const Rectangle box{330, 210 + (1 - pin) * 60, 1260, 620};
    comicPanel(box, alpha);
    const float x0 = box.x + 60;
    float y = box.y + 44;
    for (const std::string &line : wrap(g_body, "ReBruised does not include the game. To play, select a disc image (.iso) "
                                                  "made from your own copy of Black & Bruised (PAL, SLES-51620).", 36, box.width - 120))
    {
        text(g_body, line, x0, y, 36, withA(TEXTC, alpha));
        y += 48;
    }
    y += 24;
    text(g_bold, "DISC IMAGE", x0, y, 28, withA(ACCENT_HOT, alpha), 2);
    y += 40;
    std::string path = g_values["disc_image"].empty() ? "None selected" : g_values["disc_image"];
    while (path.size() > 8 && measure(g_body, path, 32).x > box.width - 120) path = "..." + path.substr(4);
    text(g_body, path, x0, y, 32, withA(ready ? TEXTC : MUTED, alpha));
    y += 60;
    // status chip
    const std::string status = ready ? "Ready to play" : "Not set up";
    const Vector2 sm = measure(g_bold, status, 30);
    DrawRectangleRounded({x0, y, sm.x + 48, 52}, 0.5f, 10, withA(ready ? Color{40, 140, 70, 255} : Color{150, 50, 40, 255}, alpha));
    text(g_bold, status, x0 + 24, y + 10, 30, withA(CREAM, alpha));
    y += 84;
    if (!S.discMsg.empty())
        for (const std::string &line : wrap(g_body, S.discMsg, 30, box.width - 120))
        {
            text(g_body, line, x0, y, 30, withA(ACCENT_HOT, alpha));
            y += 40;
        }
    // button
    const std::string label = ready ? "CHANGE DISC IMAGE" : "SELECT DISC IMAGE";
    const Vector2 m = measure(g_head, label, 56, 2);
    const Rectangle btn{box.x + box.width / 2 - m.x / 2 - 40, box.y + box.height - 120, m.x + 80, 84};
    const bool hover = CheckCollisionPointRec(mouse, btn);
    DrawRectangleRounded({btn.x - 4, btn.y - 4, btn.width + 8, btn.height + 8}, 0.4f, 12, withA(INKC, alpha));
    DrawRectangleRounded(btn, 0.4f, 12, withA(hover ? ACCENT_HOT : ACCENT, alpha));
    text(g_head, label, btn.x + 40, btn.y + 14, 56, withA(INKC, alpha), 2);
    if (in.confirm || (click && hover))
    {
        const std::string picked = bnbPickDiscImage();
        if (!picked.empty())
        {
            S.discMsg = "Checking...";
            const std::string err = disc_setup::prepare(picked);
            if (err.empty())
            {
                g_values["disc_image"] = picked;
                writeSettings();
                S.discMsg = "";
                if (needsBuild() && !B.running)
                    startBuild(); // a release: build the game from this disc right away
                else
                {
                    showToast("Disc ready. Have a good fight!");
                    goPage(Page::Main);
                }
            }
            else
                S.discMsg = err;
        }
    }
    else if (in.back)
    {
        if (ready) goPage(Page::Main);
        else S.quit = true;
    }
}

// ---- build page ----------------------------------------------------------------------------------------------------------
// Shown while kit\tools\player_build.py recompiles the game from the player's disc (first PLAY, or a new release).
void drawBuild(float t, float alpha, Input &in, Vector2 mouse, bool click)
{
    pageHeader("BUILDING THE GAME", t, alpha);
    const float pin = easeOutBack(std::clamp(S.pageT * 1.4f, 0.0f, 1.0f));
    const Rectangle box{330, 210 + (1 - pin) * 60, 1260, 620};
    comicPanel(box, alpha);
    const float x0 = box.x + 60, bw = box.width - 120;
    float y = box.y + 44;
    const char *intro = B.failed ? "The build did not finish. Nothing was changed in your disc image."
                                 : "ReBruised is turning the game on your disc into a PC program. This happens once (and again "
                                   "after an update); it takes a few minutes and downloads nothing. You can leave this page: "
                                   "the build carries on.";
    for (const std::string &line : wrap(g_body, intro, 34, bw))
    {
        text(g_body, line, x0, y, 34, withA(TEXTC, alpha));
        y += 46;
    }
    y += 30;
    // progress bar
    B.shownPct = B.shownPct + (static_cast<float>(B.pct) - B.shownPct) * std::min(1.0f, GetFrameTime() * 6.0f);
    const Rectangle bar{x0, y, bw, 46};
    DrawRectangleRounded({bar.x - 4, bar.y - 4, bar.width + 8, bar.height + 8}, 0.5f, 10, withA(INKC, alpha));
    DrawRectangleRounded(bar, 0.5f, 10, withA(Color{54, 44, 54, 255}, alpha));
    const float fw = bar.width * std::clamp(B.shownPct / 100.0f, 0.0f, 1.0f);
    if (fw > 8)
        DrawRectangleRounded({bar.x, bar.y, fw, bar.height}, 0.5f, 10, withA(B.failed ? Color{150, 50, 40, 255} : ACCENT, alpha));
    if (B.running) // moving stripes: alive even while one step takes a while
        for (float sx = bar.x + std::fmod(t * 80.0f, 40.0f) - 40; sx < bar.x + fw - 12; sx += 40)
            if (sx > bar.x + 6) DrawRectangle(static_cast<int>(sx), static_cast<int>(bar.y + 8), 12, static_cast<int>(bar.height - 16), withA(ACCENT_HOT, alpha * 0.5f));
    const std::string pctText = std::to_string(B.pct) + "%";
    text(g_bold, pctText, bar.x + bar.width - measure(g_bold, pctText, 30).x, bar.y + 60, 30, withA(MUTED, alpha));
    y += 64;
    text(g_bold, B.failed ? "STOPPED" : (B.running ? "NOW" : "DONE"), x0, y, 28, withA(ACCENT_HOT, alpha), 2);
    y += 40;
    const std::string &msg = B.failed ? B.error : B.text;
    for (const std::string &line : wrap(g_body, msg, 32, bw))
    {
        text(g_body, line, x0, y, 32, withA(B.failed ? ACCENT_HOT : TEXTC, alpha));
        y += 42;
        if (y > box.y + box.height - 150) break;
    }
    if (B.failed)
        text(g_body, "Details: build-game\\build.log (REPORT A PROBLEM collects it).", x0, y + 10, 28, withA(MUTED, alpha));
    if (B.failed)
    {
        const std::string label = "TRY AGAIN";
        const Vector2 m = measure(g_head, label, 56, 2);
        const Rectangle btn{box.x + box.width / 2 - m.x / 2 - 40, box.y + box.height - 120, m.x + 80, 84};
        const bool hover = CheckCollisionPointRec(mouse, btn);
        DrawRectangleRounded({btn.x - 4, btn.y - 4, btn.width + 8, btn.height + 8}, 0.4f, 12, withA(INKC, alpha));
        DrawRectangleRounded(btn, 0.4f, 12, withA(hover ? ACCENT_HOT : ACCENT, alpha));
        text(g_head, label, btn.x + 40, btn.y + 14, 56, withA(INKC, alpha), 2);
        if (in.confirm || (click && hover))
        {
            if (discReady()) startBuild();
            else { S.discMsg = "Select your disc image first."; goPage(Page::Disc); }
        }
    }
    if (in.back) goPage(Page::Main);
}

// ---- report a problem ----------------------------------------------------------------------------------------------------
// Collects what a developer needs (settings, the last session's logs, the game's crash note, system info) into a zip on
// the Desktop. SEND REPORT opens the issue tracker (REBRUISED_REPORT_URL in rebruised_report.h) once there is one.
// The game opens this page itself after a crash (BlackAndBruisedLauncher.exe --report); a session that ended without
// closing normally (hang, killed) is noticed at the next start through logs\session.txt.
struct ReportState
{
    std::string reason;   // what went wrong last time ("" = nothing detected)
    std::string detail;   // first lines of the crash note
    std::string savedZip; // the report written this time
    std::string msg;
    int sel = 0;
    bool pending = false; // reason comes from the last session's files (cleared when the page is left)
};
ReportState R;

std::string readText(const fs::path &p, size_t maxBytes = 1u << 20)
{
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (s.size() > maxBytes) s = "[... " + std::to_string(s.size() - maxBytes) + " bytes cut ...]\n" + s.substr(s.size() - maxBytes);
    return s;
}

std::string stamp()
{
    const std::time_t now = std::time(nullptr);
    char b[32];
    std::strftime(b, sizeof(b), "%Y%m%d-%H%M%S", std::localtime(&now));
    return b;
}

// What happened in the last session, from logs\crash.txt and a session.txt left behind.
void detectLastSession()
{
    R = ReportState{};
    const fs::path logs = g_root / "logs";
    std::error_code ec;
    const std::string crash = readText(logs / REBRUISED_CRASH_FILE, 64 * 1024);
    const std::string session = readText(logs / REBRUISED_SESSION_FILE, 4096);
    unsigned long pid = 0;
    std::string started;
    std::istringstream sessionLines(session);
    for (std::string line; std::getline(sessionLines, line);)
    {
        if (line.rfind("pid=", 0) == 0) pid = std::strtoul(line.c_str() + 4, nullptr, 10);
        if (line.rfind("started=", 0) == 0) started = line.substr(8);
    }
    const bool sessionLeft = !session.empty() && !(pid && bnbProcessAlive(pid));
    if (!crash.empty())
    {
        R.reason = "The game crashed last time.";
        std::istringstream ss(crash);
        while (R.detail.empty() && std::getline(ss, R.detail))
            R.detail.erase(std::remove(R.detail.begin(), R.detail.end(), '\r'), R.detail.end());
    }
    else if (sessionLeft)
        R.reason = "The game did not close normally last time" + (started.empty() ? std::string() : " (started " + started + ")") +
                   ": it crashed, stopped responding or was ended.";
    R.pending = !R.reason.empty();
}

// The last session's problem was looked at: do not ask again at the next start.
void clearLastSession()
{
    if (!R.pending) return;
    std::error_code ec;
    const fs::path logs = g_root / "logs";
    fs::remove(logs / REBRUISED_SESSION_FILE, ec);
    if (fs::exists(logs / REBRUISED_CRASH_FILE, ec))
        fs::rename(logs / REBRUISED_CRASH_FILE, logs / ("crash-" + stamp() + ".txt"), ec);
    R.pending = false;
}

void openReport(bool fromCrash)
{
    const std::string keepZip = R.savedZip;
    detectLastSession();
    R.savedZip = keepZip;
    if (fromCrash && R.reason.empty()) R.reason = "The game stopped with an error.";
    goPage(Page::Report);
}

std::string urlEncode(const std::string &s)
{
    static const char *hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s)
    {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += static_cast<char>(c);
        else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
    }
    return o;
}

std::string gpuLine(const std::string &err)
{
    const size_t p = err.find("renderer ready");
    if (p == std::string::npos) return "unknown";
    const size_t open = err.find('(', p), close = err.find(')', open == std::string::npos ? p : open);
    return open != std::string::npos && close != std::string::npos ? err.substr(open + 1, close - open - 1) : "unknown";
}

std::string reportSummary(const std::string &err)
{
    std::string s = "ReBruised " REBRUISED_VERSION "\n";
    const std::time_t now = std::time(nullptr);
    char when[64];
    std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    s += std::string("Report made: ") + when + "\n";
    s += bnbSystemInfo();
    s += "Graphics: " + gpuLine(err) + "\n";
    s += "Problem: " + (R.reason.empty() ? std::string("reported by the player (no crash detected)") : R.reason) + "\n";
    if (!R.detail.empty()) s += "Detail: " + R.detail + "\n";
    return s;
}

// Writes the report zip to the Desktop; returns "" or an error.
std::string saveReport()
{
    std::error_code ec;
    const fs::path logs = g_root / "logs";
    const std::string name = "ReBruised-report-" + stamp();
    const fs::path dir = fs::temp_directory_path(ec) / name;
    fs::create_directories(dir, ec);
    if (ec) return "Could not create a temporary folder.";
    const std::string err = readText(logs / "play.err", 4u << 20);

    std::string summary = reportSummary(err);
    // the lines that usually explain a crash
    std::string marks;
    std::istringstream errLines(err);
    for (std::string line; std::getline(errLines, line);)
        if (line.find("crash") != std::string::npos || line.find("missing-target") != std::string::npos ||
            line.find("terminate") != std::string::npos || line.find("fatal") != std::string::npos)
            if (marks.size() < 8000) marks += line.substr(0, 400) + "\n";
    {
        std::ofstream f(dir / "report.txt", std::ios::binary);
        f << summary << "\n";
        if (!marks.empty()) f << "---- crash lines from play.err ----\n" << marks << "\n";
        f << "---- what to add when you send it ----\nWhat were you doing when it happened (menu, fight, movie; which boxers/stage)?\n";
    }
    // settings.ini with the disc image path cut to its file name (no user folders in the report)
    {
        std::ofstream f(dir / "settings.ini", std::ios::binary);
        std::istringstream settingLines(readText(g_root / "settings.ini"));
        for (std::string line; std::getline(settingLines, line);)
        {
            if (lower(trim(line)).rfind("disc_image", 0) == 0)
                line = "disc_image = <...>\\" + fs::path(g_values["disc_image"]).filename().string();
            f << line << "\n";
        }
    }
    for (const char *file : {"play.log", "play.err", REBRUISED_CRASH_FILE, REBRUISED_SESSION_FILE})
        if (fs::exists(logs / file, ec))
        {
            std::ofstream f(dir / file, std::ios::binary);
            f << readText(logs / file, 4u << 20);
        }
    if (fs::exists(g_root / "build-game" / "build.log", ec)) // the build from the player's disc (release package)
    {
        std::ofstream f(dir / "build.log", std::ios::binary);
        f << readText(g_root / "build-game" / "build.log", 4u << 20);
    }
    std::string out = bnbDesktopDir();
    if (out.empty()) out = g_root.string();
    const std::string zip = (fs::path(out) / (name + ".zip")).string();
    const bool zipped = bnbRunWait("tar.exe -a -c -f \"" + zip + "\" *", dir.string(), 60000) && fs::exists(zip, ec);
    if (zipped)
        fs::remove_all(dir, ec);
    else
    {
        // no tar.exe (older Windows): leave the folder on the Desktop instead
        const fs::path folder = fs::path(out) / name;
        fs::rename(dir, folder, ec);
        if (ec) return "Could not write the report to your Desktop.";
        R.savedZip = folder.string();
        return {};
    }
    R.savedZip = zip;
    return {};
}

void drawReport(float t, float alpha, Input &in, Vector2 mouse, bool click)
{
    pageHeader("REPORT", t, alpha);
    const bool online = std::strlen(REBRUISED_REPORT_URL) > 0;
    const float pin = easeOutBack(std::clamp(S.pageT * 1.4f, 0.0f, 1.0f));
    const Rectangle box{290, 200 + (1 - pin) * 60, 1340, 680};
    comicPanel(box, alpha);
    const float x0 = box.x + 60, w = box.width - 120;
    float y = box.y + 40;
    const auto para = [&](const std::string &s, const Font &f, float size, Color c)
    {
        for (const std::string &line : wrap(f, s, size, w))
        {
            text(f, line, x0, y, size, withA(c, alpha));
            y += size * 1.3f;
        }
    };
    if (!R.reason.empty())
    {
        para(R.reason, g_bold, 36, ACCENT_HOT);
        if (!R.detail.empty()) para(R.detail.substr(0, 160), g_body, 26, MUTED);
        y += 10;
        para("Saving a report collects the logs that show why, so it can be fixed.", g_body, 32, TEXTC);
    }
    else
        para("Something looked, sounded or behaved wrong? Save a report straight after it happened: it holds the log of "
             "your last game session.", g_body, 34, TEXTC);
    y += 16;
    text(g_bold, "WHAT'S IN IT", x0, y, 28, withA(ACCENT_HOT, alpha), 2);
    y += 42;
    para("Your ReBruised settings, the game's logs from the last session, Windows version, processor, memory and graphics "
         "card. No game files and nothing from your own folders.", g_body, 28, TEXTC);
    if (!online)
    {
        y += 8;
        para("Online reports are not connected yet: send the saved file to the ReBruised team.", g_body, 26, MUTED);
    }

    // buttons
    const char *labels[2] = {"SAVE REPORT", "SEND REPORT"};
    float bw[2], total = 0;
    for (int i = 0; i < 2; ++i) { bw[i] = measure(g_head, labels[i], 50, 2).x + 80; total += bw[i]; }
    float bx = box.x + box.width / 2 - (total + 40) / 2;
    const float by = box.y + box.height - 112;
    if (in.left || in.up) R.sel = 0;
    if (in.right || in.down) R.sel = online ? 1 : 0;
    bool activate[2] = {false, false};
    for (int i = 0; i < 2; ++i)
    {
        const Rectangle btn{bx, by, bw[i], 78};
        const bool enabled = i == 0 || online;
        const bool hover = enabled && CheckCollisionPointRec(mouse, btn);
        if (hover && (GetMouseDelta().x != 0 || GetMouseDelta().y != 0)) R.sel = i;
        if (hover && click) activate[i] = true;
        const bool sel = R.sel == i && enabled;
        DrawRectangleRounded({btn.x - 4, btn.y - 4, btn.width + 8, btn.height + 8}, 0.4f, 12, withA(INKC, alpha));
        DrawRectangleRounded(btn, 0.4f, 12, withA(!enabled ? Color{70, 62, 70, 255} : sel ? ACCENT_HOT : ACCENT, alpha));
        text(g_head, labels[i], btn.x + 40, btn.y + 12, 50, withA(enabled ? INKC : MUTED, alpha), 2);
        bx += bw[i] + 40;
    }
    if (in.confirm) activate[R.sel] = true;
    if (!R.msg.empty())
    {
        std::string m = R.msg;
        while (m.size() > 8 && measure(g_body, m, 26).x > w) m = "..." + m.substr(4);
        textCentered(g_body, m, VW / 2, by - 44, 26, withA(CREAM, alpha));
    }

    if (activate[0] || activate[1])
    {
        if (R.savedZip.empty() || activate[0])
        {
            const std::string err = saveReport();
            R.msg = err.empty() ? "Saved: " + R.savedZip : err;
            if (err.empty()) { clearLastSession(); bnbShowInFolder(R.savedZip); }
        }
        if (activate[1] && online && !R.savedZip.empty())
        {
            const std::string err = readText(g_root / "logs" / "play.err", 4u << 20);
            const std::string title = "[Report] " + (R.reason.empty() ? std::string("Problem while playing") : R.reason.substr(0, 80));
            const std::string body = reportSummary(err) + "\nWhat happened:\n\n(describe it here)\n\nPlease attach the report file: " +
                                     fs::path(R.savedZip).filename().string() + " (saved on your Desktop).";
            bnbOpenTarget(std::string(REBRUISED_REPORT_URL) + "?title=" + urlEncode(title) + "&body=" + urlEncode(body));
            R.msg = "Your browser opened the report page: attach " + fs::path(R.savedZip).filename().string();
        }
    }
    if (in.back)
    {
        clearLastSession();
        goPage(Page::Main);
    }
}

// ---- controls page -------------------------------------------------------------------------------------------------------
void drawControls(float t, float alpha, const Input &in)
{
    if (in.back || in.confirm) goPage(Page::Main);
    pageHeader("CONTROLS", t, alpha);
    Style pad = promptStyle();
    if (pad == Style::Keyboard) pad = IsGamepadAvailable(0) ? padStyleFromName() : Style::Xbox;
    struct Row { const char *ps2; Btn b; const char *keys; };
    const Row rows[] = {
        {"Cross", Btn::South, "Down  /  X  /  Space"}, {"Circle", Btn::East, "Right  /  C  /  Esc"},
        {"Square", Btn::West, "Left  /  Z"},          {"Triangle", Btn::North, "Up  /  V"},
        {"L1  /  R1", Btn::LB, "Q  /  E"},             {"L2  /  R2", Btn::LT, "Left Shift  /  Right Shift"},
        {"Start", Btn::Start, "Enter"},                 {"Select", Btn::Select, "Tab"},
        {"Move", Btn::LStick, "W  A  S  D"},
    };
    const float pin = easeOutBack(std::clamp(S.pageT * 1.4f, 0.0f, 1.0f));
    const Rectangle box{230, 200 + (1 - pin) * 60, 1460, 690};
    comicPanel(box, alpha);
    const float x0 = box.x + 50;
    text(g_bold, "PS2 BUTTON", x0, box.y + 26, 28, withA(ACCENT_HOT, alpha), 2);
    text(g_bold, "YOUR CONTROLLER", x0 + 470, box.y + 26, 28, withA(ACCENT_HOT, alpha), 2);
    text(g_bold, "KEYBOARD", x0 + 920, box.y + 26, 28, withA(ACCENT_HOT, alpha), 2);
    for (int i = 0; i < 9; ++i)
    {
        const float d = easeOutCubic(std::clamp(S.pageT * 2.5f - i * 0.07f, 0.0f, 1.0f));
        const float y = box.y + 80 + i * 62;
        if (i % 2 == 0) DrawRectangle(static_cast<int>(box.x + 6), static_cast<int>(y - 8), static_cast<int>(box.width - 12), 60, withA(Color{255, 255, 255, 8}, alpha));
        const float a = alpha * d;
        if (i < 4) glyph(Style::PlayStation, rows[i].b, {x0 + 22, y + 22}, 42);
        else if (i == 4) glyph(Style::PlayStation, Btn::LB, {x0 + 28, y + 22}, 42);
        else if (i == 5) glyph(Style::PlayStation, Btn::LT, {x0 + 28, y + 22}, 42);
        else if (i < 8) pill(i == 6 ? "START" : "SELECT", {x0 + 38, y + 22}, 30, Color{54, 48, 58, 255}, TEXTC);
        else glyph(Style::PlayStation, Btn::LStick, {x0 + 22, y + 22}, 42);
        text(g_bold, rows[i].ps2, x0 + 100, y + 4, 36, withA(TEXTC, a));
        float gx = x0 + 470;
        if (i == 4 || i == 5)
        {
            const Btn b1 = i == 4 ? Btn::LB : Btn::LT, b2 = i == 4 ? Btn::RB : Btn::RT;
            const float w1 = glyphWidth(pad, b1, 42);
            glyph(pad, b1, {gx + w1 / 2, y + 22}, 42);
            gx += w1 + 18;
            glyph(pad, b2, {gx + glyphWidth(pad, b2, 42) / 2, y + 22}, 42);
        }
        else if (i == 8)
        {
            glyph(pad, Btn::LStick, {gx + 21, y + 22}, 42);
            glyph(pad, Btn::DpadUD, {gx + 21 + 66, y + 22}, 42);
        }
        else
            glyph(pad, rows[i].b, {gx + glyphWidth(pad, rows[i].b, 42) / 2, y + 22}, 42);
        text(g_body, rows[i].keys, x0 + 920, y + 6, 34, withA(TEXTC, a * 0.9f));
    }
    textCentered(g_body, "The game reads XInput controllers. PlayStation and Nintendo pads work through Steam Input or DS4Windows.",
                 VW / 2, box.y + box.height + 24, 26, withA(MUTED, alpha));
}

fs::path findRoot()
{
    const std::string exe = bnbExePath();
    fs::path p = exe.empty() ? fs::current_path() : fs::path(exe).parent_path();
    g_exeDir = p;
    for (int i = 0; i < 6 && !p.empty(); ++i, p = p.parent_path())
        if (fs::exists(p / "kit" / "build_id.txt") || fs::exists(p / "game" / "disc" / "SLES_516.20") || fs::exists(p / "settings.ini"))
            return p;
    return fs::current_path();
}
} // namespace

int main(int argc, char **argv)
{
    g_root = findRoot();
    readSettings();
    SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT);
    SetTraceLogLevel(LOG_WARNING);
    InitWindow(1280, 720, "ReBruised - Black & Bruised");
    if (!IsWindowReady()) // no OpenGL 3.3 window (tools/patch_raylib.py retries first): say so instead of crashing later
    {
        bnbErrorBox("ReBruised could not start OpenGL 3.3 on this PC.\n\nUpdate your graphics driver (NVIDIA, AMD or Intel) "
                    "and start ReBruised again. Remote Desktop sessions and the basic Microsoft display driver are not supported.");
        return 1;
    }
    SetWindowMinSize(960, 540);
    SetExitKey(KEY_NULL);
    SetTargetFPS(144);
    rlDisableBackfaceCulling();
    {
        static const char *head[] = {"ARLRDBD.TTF", "seguibl.ttf", "impact.ttf", "arialbd.ttf", nullptr};
        static const char *bold[] = {"segoeuib.ttf", "arialbd.ttf", nullptr};
        static const char *body[] = {"segoeui.ttf", "arial.ttf", nullptr};
        g_head = loadFont(head, 160);
        g_bold = loadFont(bold, 72);
        g_body = loadFont(body, 64);
    }
    loadLogo();
    initBackground();
    if (IsGamepadAvailable(0)) g_lastDevice = padStyleFromName();
    // test hooks (screenshots): BNB_LAUNCHER_PAGE=options|controls (+ BNB_LAUNCHER_ROW, BNB_LAUNCHER_SEL),
    // BNB_LAUNCHER_STYLE=xbox|playstation|nintendo|keyboard, BNB_LAUNCHER_AUTOPLAY=1 (presses PLAY after 1.5 s)
    if (const char *p = std::getenv("BNB_LAUNCHER_PAGE"))
    {
        if (std::strcmp(p, "options") == 0) S.page = Page::Options;
        else if (std::strcmp(p, "controls") == 0) S.page = Page::Controls;
    }
    if (const char *row = std::getenv("BNB_LAUNCHER_ROW")) S.optSel = std::atoi(row);
    if (const char *sel = std::getenv("BNB_LAUNCHER_SEL")) S.mainSel = S.lastMainSel = std::clamp(std::atoi(sel), 0, static_cast<int>(g_main.size()) - 1);
    if (const char *st = std::getenv("BNB_LAUNCHER_STYLE")) g_values["launcher_glyphs"] = st;
    // test hook: BNB_LAUNCHER_PREPARE=<iso> runs the disc check + extraction as if picked in the dialog
    if (const char *iso = std::getenv("BNB_LAUNCHER_PREPARE"))
    {
        const std::string err = disc_setup::prepare(iso);
        if (err.empty()) { g_values["disc_image"] = iso; writeSettings(); }
        else S.discMsg = err;
        if (FILE *f = std::fopen((g_root / "logs" / "launcher-prepare.txt").string().c_str(), "w"))
        {
            std::fprintf(f, "%s\n", err.empty() ? "OK" : err.c_str());
            std::fclose(f);
        }
    }
    // no disc set up yet (first start, or the image moved): ask for it before anything else
    if (!discReady() && !std::getenv("BNB_LAUNCHER_PAGE"))
        S.page = Page::Disc;
    if (const char *p = std::getenv("BNB_LAUNCHER_PAGE"); p && std::strcmp(p, "disc") == 0)
        S.page = Page::Disc;
    // test hook: BNB_LAUNCHER_PAGE=build starts the build from the set-up disc (release package)
    if (const char *p = std::getenv("BNB_LAUNCHER_PAGE"); p && std::strcmp(p, "build") == 0 && discReady())
        startBuild();
    // A problem report: the game opens the launcher with --report after a crash; a session that did not end normally is
    // noticed here (logs\session.txt left behind). BNB_LAUNCHER_PAGE=report shows the page (screenshots).
    bool reportArg = false, reportPage = false;
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--report") == 0) reportArg = true;
    if (const char *p = std::getenv("BNB_LAUNCHER_PAGE"); p && std::strcmp(p, "report") == 0) reportPage = true;
    if (reportArg || reportPage)
        openReport(reportArg);
    else if (discReady() && !std::getenv("BNB_LAUNCHER_PAGE"))
    {
        detectLastSession();
        if (!R.reason.empty()) { S.page = Page::Report; S.pageT = 0; }
    }
    // test hook: BNB_LAUNCHER_SAVE_REPORT=1 saves a report at once (result in logs\launcher-report.txt)
    if (std::getenv("BNB_LAUNCHER_SAVE_REPORT"))
    {
        const std::string err = saveReport();
        if (FILE *f = std::fopen((g_root / "logs" / "launcher-report.txt").string().c_str(), "w"))
        {
            std::fprintf(f, "%s\n", err.empty() ? R.savedZip.c_str() : err.c_str());
            std::fclose(f);
        }
    }
    const bool autoplay = std::getenv("BNB_LAUNCHER_AUTOPLAY") != nullptr;

    float t = 0;
    while (!WindowShouldClose() && !S.quit)
    {
        const float dt = std::min(GetFrameTime(), 0.05f);
        t += dt;
        S.introT += dt;
        if (IsKeyPressed(KEY_F11)) ToggleBorderlessWindowed();
        const float sw = static_cast<float>(GetScreenWidth()), sh = static_cast<float>(GetScreenHeight());
        g_scale = std::min(sw / VW, sh / VH);
        g_offX = (sw - VW * g_scale) / 2;
        g_offY = (sh - VH * g_scale) / 2;
        const Vector2 mouse = toVirtual(GetMousePosition());
        const bool click = IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && S.pageT >= 1.0f;
        Input in = pollInput(dt);
        if (S.pageT < 1.0f) in = Input{};
        if (autoplay && S.introT > 1.5f && S.page == Page::Main) play();
        S.pageT = std::min(1.0f, S.pageT + dt * 3.2f);
        S.toastT = std::max(0.0f, S.toastT - dt);
        pollBuild(dt);

        BeginDrawing();
        ClearBackground(BLACK);
        rlPushMatrix();
        rlTranslatef(g_offX, g_offY, 0);
        rlScalef(g_scale, g_scale, 1);
        scissorVirtual(0, 0, VW, VH);
        drawBackground(t, dt, S.page == Page::Main ? Vector2{VW / 2, 250} : Vector2{250, 92});
        const float pa = easeOutCubic(S.pageT);
        switch (S.page)
        {
        case Page::Main: drawMain(t, dt, pa, in, mouse, click); break;
        case Page::Options: drawOptions(t, dt, pa, in, mouse, click); break;
        case Page::Controls: drawControls(t, pa, in); break;
        case Page::Disc: drawDisc(t, pa, in, mouse, click); break;
        case Page::Report: drawReport(t, pa, in, mouse, click); break;
        case Page::Build: drawBuild(t, pa, in, mouse, click); break;
        }
        const bool nin = promptStyle() == Style::Nintendo;
        const Btn ok = nin ? Btn::East : Btn::South, back = nin ? Btn::South : Btn::East;
        if (S.page == Page::Disc) drawPrompts({{ok, "Select disc image"}, {back, discReady() ? "Back" : "Quit"}});
        else if (S.page == Page::Main) drawPrompts({{Btn::DpadUD, "Move"}, {ok, "Select"}, {back, "Quit"}});
        else if (S.page == Page::Options) drawPrompts({{Btn::DpadUD, "Move"}, {Btn::DpadLR, "Change"}, {back, "Back"}});
        else if (S.page == Page::Report) drawPrompts({{Btn::DpadLR, "Move"}, {ok, "Select"}, {back, "Back"}});
        else if (S.page == Page::Build && B.failed) drawPrompts({{ok, "Try again"}, {back, "Back"}});
        else drawPrompts({{back, "Back"}});
        if (S.toastT > 0)
        {
            const float a = std::min(1.0f, S.toastT / 0.4f);
            const Vector2 m = measure(g_bold, S.toast, 36);
            const Rectangle r{VW / 2 - m.x / 2 - 36, VH - 200, m.x + 72, 70};
            comicPanel(r, a);
            text(g_bold, S.toast, r.x + 36, r.y + 14, 36, withA(TEXTC, a));
        }
        EndScissorMode();
        rlPopMatrix();
        EndDrawing();
    }
    CloseWindow();
    return 0;
}
