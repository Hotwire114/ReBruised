// Black & Bruised: the player's controller glyphs over the game's PS2 button symbols.
//
// The menus draw cross / circle / square / triangle as sprites from the menu font atlas (512x512 T8, the four
// symbols stacked in its left column). runtime/ps2_button_prompts.h marks every such sprite in the GS backend; this
// file registers the atlas and draws high-quality glyphs (rendered once at 256 px, mip-mapped) over the marks in the
// presenter's host overlay. Style: settings.ini launcher_glyphs (BNB_PROMPTS): auto = the connected pad's style
// (Xbox / Nintendo; keyboard keys without a pad), xbox, nintendo, keyboard; playstation keeps the game's own symbols.
// The game reads XInput pads: cross = A, circle = B, square = X, triangle = Y.
#include "raylib.h"
#include "ps2_host_overlay.h"
#include "runtime/ps2_button_prompts.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{
namespace bp = ps2_button_prompts;

enum class Style { Original, Xbox, Nintendo, Keyboard };
Style g_setting = Style::Xbox;
bool g_auto = true;


Font g_font{};
bool g_ready = false;
Texture2D g_tex[3][bp::Count] = {}; // [Xbox, Nintendo, Keyboard][button]

const Color DISC{30, 30, 36, 255}, RIM{78, 80, 92, 255}, LIGHT{240, 240, 246, 255};

void centeredText(const std::string &s, float cx, float cy, float size, Color c)
{
    const Vector2 m = MeasureTextEx(g_font, s.c_str(), size, 0);
    DrawTextEx(g_font, s.c_str(), {cx - m.x / 2, cy - m.y / 2}, size, 0, c);
}
void face(const std::string &letter, Color col)
{
    // 256x256 canvas
    DrawCircle(128, 136, 118, Color{0, 0, 0, 120});
    DrawCircle(128, 128, 118, DISC);
    DrawRing({128, 128}, 108, 118, 0, 360, 96, RIM);
    centeredText(letter, 128, 130, 150, col);
}
void shoulder(const std::string &label, bool trigger)
{
    const Rectangle r{12, 58, 232, 140};
    DrawRectangleRounded({r.x, r.y + 8, r.width, r.height}, trigger ? 0.45f : 0.9f, 16, Color{0, 0, 0, 120});
    DrawRectangleRounded(r, trigger ? 0.45f : 0.9f, 16, DISC);
    DrawRectangleRoundedLinesEx(r, trigger ? 0.45f : 0.9f, 16, 8, RIM);
    centeredText(label, 128, 128, 104, LIGHT);
}
void keycap(const std::string &label)
{
    const Rectangle r{14, 14, 228, 228};
    DrawRectangleRounded({r.x, r.y + 10, r.width, r.height}, 0.25f, 12, Color{0, 0, 0, 140});
    DrawRectangleRounded(r, 0.25f, 12, Color{64, 66, 78, 255});
    DrawRectangleRounded({r.x + 12, r.y + 8, r.width - 24, r.height - 34}, 0.22f, 12, Color{104, 108, 126, 255});
    DrawRectangleRoundedLinesEx(r, 0.25f, 12, 6, Color{150, 154, 176, 255});
    centeredText(label, 128, 116, label.size() > 2 ? 64.0f : 120.0f, LIGHT);
}

Texture2D render(int style, int b)
{
    RenderTexture2D rt = LoadRenderTexture(256, 256);
    BeginTextureMode(rt);
    ClearBackground(BLANK);
    if (style == 0) // Xbox
    {
        static const char *L[] = {"A", "B", "X", "Y"};
        static const Color C[] = {{96, 196, 72, 255}, {232, 74, 72, 255}, {70, 140, 236, 255}, {242, 196, 48, 255}};
        static const char *S[] = {"LB", "RB", "LT", "RT"};
        if (b < 4) face(L[b], C[b]); else shoulder(S[b - 4], b >= 6);
    }
    else if (style == 1) // Nintendo, by position: cross (south) = B, circle (east) = A, square (west) = Y, triangle = X
    {
        static const char *L[] = {"B", "A", "Y", "X"};
        static const char *S[] = {"L", "R", "ZL", "ZR"};
        if (b < 4) face(L[b], LIGHT); else shoulder(S[b - 4], b >= 6);
    }
    else // keyboard (the runtime's default keys)
    {
        static const char *K[] = {"X", "C", "Z", "V", "Q", "E", "LSH", "RSH"};
        keycap(K[b]);
    }
    EndTextureMode();
    // copy out flipped (render textures are upside down) into a mip-mapped texture
    Image img = LoadImageFromTexture(rt.texture);
    ImageFlipVertical(&img);
    Texture2D t = LoadTextureFromImage(img);
    UnloadImage(img);
    UnloadRenderTexture(rt);
    GenTextureMipmaps(&t);
    SetTextureFilter(t, TEXTURE_FILTER_TRILINEAR);
    return t;
}

void ensureReady()
{
    if (g_ready) return;
    g_ready = true;
    for (const char *f : {"C:/Windows/Fonts/segoeuib.ttf", "C:/Windows/Fonts/arialbd.ttf"})
    {
        g_font = LoadFontEx(f, 160, nullptr, 0);
        if (g_font.texture.id) break;
    }
    if (!g_font.texture.id) g_font = GetFontDefault();
    SetTextureFilter(g_font.texture, TEXTURE_FILTER_BILINEAR);
    for (int s = 0; s < 3; ++s)
        for (int b = 0; b < bp::Count; ++b)
            g_tex[s][b] = render(s, b);
}

Style currentStyle()
{
    if (!g_auto) return g_setting;
    if (!IsGamepadAvailable(0)) return Style::Keyboard;
    std::string n = GetGamepadName(0) ? GetGamepadName(0) : "";
    for (char &c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (n.find("nintendo") != std::string::npos || n.find("switch") != std::string::npos || n.find("pro controller") != std::string::npos)
        return Style::Nintendo;
    if (n.find("playstation") != std::string::npos || n.find("dualsense") != std::string::npos || n.find("dualshock") != std::string::npos ||
        n.find("ps4") != std::string::npos || n.find("ps5") != std::string::npos)
        return Style::Original;
    return Style::Xbox;
}

void drawOverlay(PS2Runtime &, const ps2_host_overlay::Frame &f, void *)
{
    const Style st = currentStyle();
    const auto marks = bp::latest();
    static const bool log = std::getenv("BNB_PROMPT_LOG") != nullptr;
    if (log)
    {
        static double last = 0;
        if (GetTime() - last > 1.0)
        {
            last = GetTime();
            std::fprintf(stderr, "[bnb-prompt] overlay style=%d marks=%zu frame=%.0f,%.0f %.0fx%.0f\n", static_cast<int>(st),
                         marks.size(), f.x, f.y, f.w, f.h);
            for (size_t i = 0; i < marks.size() && i < 4; ++i)
                std::fprintf(stderr, "[bnb-prompt]   mark %u box=%.1f-%.1f,%.1f-%.1f a=%.2f\n", marks[i].button, marks[i].x0,
                             marks[i].x1, marks[i].y0, marks[i].y1, marks[i].alpha);
        }
    }
    if (st == Style::Original) return;
    if (marks.empty()) return;
    ensureReady();
    const int si = st == Style::Xbox ? 0 : st == Style::Nintendo ? 1 : 2;
    // marks are in the 640x512 frame the game copies to the display
    const float sx = f.w / 640.0f, sy = f.h / 512.0f;
    for (const bp::Mark &m : marks)
    {
        if (m.alpha <= 0.02f) continue;
        const float cx = f.x + (m.x0 + m.x1) * 0.5f * sx, cy = f.y + (m.y0 + m.y1) * 0.5f * sy;
        const float size = std::max((m.x1 - m.x0) * sx, (m.y1 - m.y0) * sy) * 1.12f;
        const Texture2D &t = g_tex[si][m.button];
        DrawTexturePro(t, {0, 0, static_cast<float>(t.width), static_cast<float>(t.height)},
                       {cx - size / 2, cy - size / 2, size, size}, {0, 0}, 0,
                       Color{255, 255, 255, static_cast<unsigned char>(std::clamp(m.alpha, 0.0f, 1.0f) * 255)});
    }
}
} // namespace

// main.cpp: before the runtime opens its window
void bnbInstallButtonPrompts()
{
    const char *e = std::getenv("BNB_PROMPTS");
    const std::string v = e ? e : "auto";
    g_auto = v == "auto" || v.empty();
    g_setting = v == "nintendo" ? Style::Nintendo : v == "keyboard" ? Style::Keyboard : v == "playstation" ? Style::Original : Style::Xbox;
    if (!g_auto && g_setting == Style::Original && !std::getenv("BNB_PROMPT_LOG"))
        return; // the game's own symbols: nothing to mark
    // the game's font atlases with button symbols: same cell layout, different lettering. Signature = the shape of
    // the cross cell (texels u 1..31, v 449..481; BNB_PROMPT_LOG=1 prints candidate cells and their shapes)
    std::vector<uint32_t> signatures = {0x9de5110cu /* caution / title font */, 0x37a7bd07u /* menu font */};
    if (const char *sig = std::getenv("BNB_PROMPT_SIG")) signatures.push_back(static_cast<uint32_t>(std::strtoul(sig, nullptr, 16)));
    for (uint32_t sig : signatures)
    {
        bp::Atlas atlas;
        atlas.signature = sig;
        atlas.sigU0 = 1; atlas.sigV0 = 449; atlas.sigU1 = 31; atlas.sigV1 = 481;
        atlas.cells = {
            {1, 449, 31, 481, bp::Cross},
            {1, 353, 31, 385, bp::Circle},
            {1, 385, 31, 417, bp::Square},
            {1, 417, 31, 449, bp::Triangle},
        };
        bp::registerAtlas(atlas);
    }
    ps2_host_overlay::set(&drawOverlay, nullptr);
}
