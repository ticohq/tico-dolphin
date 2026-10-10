// Copyright 2026 Azahar Emulator Project
// Copyright 2026 Dan | ticoverse.com
// SPDX-License-Identifier: GPL-2.0-or-later

#include "overlay/imgui_overlay.h"
#include "../TicoSession.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "imgui.h"
#include <stb_image.h>
// nanosvg is implemented in ra_alerts.cpp
#include "deps/nanosvg/nanosvg.h"
#include "deps/nanosvg/nanosvgrast.h"

#ifdef __SWITCH__
#include <switch.h>
#endif

#include "TicoLogger.h"
#include "TicoOverlayHost.h"
#include "overlay/overlay_renderer.h"
#include "overlay/ra_alerts.h"
#include "overlay/tico_config.h"

namespace SwitchFrontend::ImGuiOverlay {
namespace {

constexpr std::array<const char*, 4> kFontPaths = {{
    "romfs:/fonts/font.ttf",
    "sdmc:/tico/fonts/font.ttf",
    "sdmc:/tico/assets/fonts/font.ttf",
    "sdmc:/tico/assets/font.ttf",
}};
// RetroAchievements descriptions use the lighter face
constexpr std::array<const char*, 4> kDescriptionFontPaths = {{
    "romfs:/fonts/description.ttf",
    "sdmc:/tico/fonts/description.ttf",
    "sdmc:/tico/assets/fonts/description.ttf",
    "sdmc:/tico/assets/description.ttf",
}};
constexpr std::array<const char*, 6> kAvatarPaths = {{
    "sdmc:/tico/assets/avatar.jpg",
    "sdmc:/tico/assets/avatar.jpeg",
    "sdmc:/tico/assets/avatar.png",
    "romfs:/assets/avatar.jpg",
    "romfs:/assets/avatar.jpeg",
    "romfs:/assets/avatar.png",
}};
// Selection border strips, as tico-nx ships them: index 0 and "original" (the
// last) use the cyan/violet strip, every other tint has its own. The slugs
// mirror tico-nx's TintPalette::GetBorderGradientFile.
constexpr const char* kBorderDir = "romfs:/assets/border/";
constexpr std::array<const char*, 13> kBorderSlugs = {{
    "default", "aqua", "violet", "sunset", "lime", "rose", "gold", "ice", "ember", "mint",
    "lagoon", "cobalt", "original",
}};
// the overlay's layout is designed for a 720p display
constexpr float kDesignHeight = 720.0f;

bool s_initialized = false;
bool s_visible = false;
bool s_psm_initialized = false;
bool s_pl_initialized = false;
IOverlayHost* s_host = nullptr;
ImFont* s_description_font = nullptr;
OverlayUI::NavInput s_nav{};
OverlayUI::Action s_action = OverlayUI::Action::None;
ImTextureID s_avatar_texture = 0;
ImTextureID s_border_texture = 0;
// the quick menu sidebar's icons: Settings, Restart, Exit Game
std::array<ImTextureID, 3> s_side_icons{};

const char* FirstExisting(const std::array<const char*, 4>& paths) {
    for (const char* path : paths) {
        if (std::FILE* fp = std::fopen(path, "rb")) {
            std::fclose(fp);
            return path;
        }
    }
    return nullptr;
}

// Fills in the characters a font lacks, so every overlay language renders:
// tico's font.ttf for CJK, then the console's system fonts (which also
// cover Cyrillic, Korean and traditional Chinese). The first font that has
// a glyph wins; glyphs load as they are first drawn.
void MergeFallbacks(float size, const char* skip) {
    ImGuiIO& io = ImGui::GetIO();
    ImFontConfig merge;
    merge.MergeMode = true;
    if (const char* main = FirstExisting(kFontPaths); main && main != skip) {
        io.Fonts->AddFontFromFileTTF(main, size, &merge);
    }
#ifdef __SWITCH__
    if (!s_pl_initialized) {
        s_pl_initialized = R_SUCCEEDED(plInitialize(PlServiceType_User));
    }
    if (!s_pl_initialized) {
        return;
    }
    merge.FontDataOwnedByAtlas = false; // the system keeps the font data mapped
    for (const PlSharedFontType type :
         {PlSharedFontType_Standard, PlSharedFontType_ChineseSimplified,
          PlSharedFontType_ExtChineseSimplified, PlSharedFontType_ChineseTraditional,
          PlSharedFontType_KO}) {
        PlFontData font{};
        if (R_SUCCEEDED(plGetSharedFontByType(&font, type))) {
            io.Fonts->AddFontFromMemoryTTF(font.address, static_cast<int>(font.size), size,
                                           &merge);
        }
    }
#endif
}

ImFont* AddFirstFont(const std::array<const char*, 4>& paths, float size) {
    const char* path = FirstExisting(paths);
    ImFont* font = path ? ImGui::GetIO().Fonts->AddFontFromFileTTF(path, size) : nullptr;
    if (!font) {
        return nullptr;
    }
    LOG_INFO("OVERLAY", "loaded font: %s", path);
    MergeFallbacks(size, path);
    return font;
}

// Uploads an RGBA image decoded by stb_image and frees it.
ImTextureID Upload(unsigned char* rgba, int width, int height, const char* source) {
    if (!rgba) {
        return 0;
    }
    const ImTextureID texture = s_host ? s_host->CreateTextureRGBA(rgba, width, height) : 0;
    stbi_image_free(rgba);
    if (texture) {
        LOG_INFO("OVERLAY", "loaded %s (%dx%d)", source, width, height);
    }
    return texture;
}

void LoadBorder() {
    const int tint = TicoConfig::BorderTint();
    const int original = static_cast<int>(kBorderSlugs.size()) - 1;
    std::string path = kBorderDir;
    if (tint <= 0 || tint >= original) {
        path += "border_gradient.png";
    } else {
        path += std::string("border_gradient_") + kBorderSlugs[static_cast<std::size_t>(tint)] +
                ".png";
    }
    int width = 0;
    int height = 0;
    int channels = 0;
    s_border_texture =
        Upload(stbi_load(path.c_str(), &width, &height, &channels, 4), width, height, path.c_str());
    if (!s_border_texture) {
        LOG_WARN("OVERLAY", "no selection border strip at %s", path.c_str());
    }
    OverlayUI::SetBorderTextureId(static_cast<unsigned long long>(s_border_texture));
}

#ifdef __SWITCH__
bool LoadAvatarFromAccount() {
    if (R_FAILED(accountInitialize(AccountServiceType_Application))) {
        return false;
    }

    AccountUid uid{};
    bool found = tico::CurrentSession().AccountId(uid); // Who tico says plays
    if (!found) found = R_SUCCEEDED(accountGetPreselectedUser(&uid)) && accountUidIsValid(&uid);
    if (!found) {
        found = R_SUCCEEDED(accountGetLastOpenedUser(&uid)) && accountUidIsValid(&uid);
    }
    if (!found) {
        AccountUid uids[ACC_USER_LIST_SIZE]{};
        s32 total = 0;
        if (R_SUCCEEDED(accountListAllUsers(uids, ACC_USER_LIST_SIZE, &total)) && total > 0) {
            uid = uids[0];
            found = accountUidIsValid(&uid);
        }
    }

    AccountProfile profile{};
    if (found && R_SUCCEEDED(accountGetProfile(&profile, uid))) {
        AccountProfileBase profile_base{};
        if (R_SUCCEEDED(accountProfileGet(&profile, nullptr, &profile_base)) &&
            profile_base.nickname[0] != '\0') {
            OverlayUI::SetNickname(profile_base.nickname);
        }
        u32 image_size = 0;
        if (R_SUCCEEDED(accountProfileGetImageSize(&profile, &image_size)) && image_size > 0) {
            std::vector<unsigned char> jpeg(image_size);
            u32 actual = 0;
            if (R_SUCCEEDED(accountProfileLoadImage(&profile, jpeg.data(), image_size, &actual)) &&
                actual > 0) {
                int width = 0;
                int height = 0;
                int channels = 0;
                s_avatar_texture =
                    Upload(stbi_load_from_memory(jpeg.data(), static_cast<int>(actual), &width,
                                                 &height, &channels, 4),
                           width, height, "switch-account-avatar");
            }
        }
        accountProfileClose(&profile);
    }
    accountExit();
    return s_avatar_texture != 0;
}
#endif

void LoadAvatar() {
    for (const char* path : tico::AvatarCandidates(kAvatarPaths)) {
        int width = 0;
        int height = 0;
        int channels = 0;
        s_avatar_texture = Upload(stbi_load(path, &width, &height, &channels, 4), width, height, path);
        if (s_avatar_texture) {
            break;
        }
    }
#ifdef __SWITCH__
    if (!s_avatar_texture && !LoadAvatarFromAccount()) {
        LOG_INFO("OVERLAY", "no avatar image found");
    }
#endif
    OverlayUI::SetAvatarTextureId(static_cast<unsigned long long>(s_avatar_texture));
}

// A white SVG icon rasterized to a size x size texture.
ImTextureID LoadSvgIcon(const std::string& path, int size) {
    NSVGimage* image = nsvgParseFromFile(path.c_str(), "px", 96.0f);
    if (!image) {
        LOG_WARN("OVERLAY", "no icon at %s", path.c_str());
        return 0;
    }
    ImTextureID texture = 0;
    if (NSVGrasterizer* rast = nsvgCreateRasterizer()) {
        std::vector<unsigned char> rgba(static_cast<std::size_t>(size) * size * 4, 0);
        const float longest = std::max(image->width, image->height);
        const float scale = longest > 0.0f ? size / longest : 1.0f;
        // centre the shorter side
        const float dx = (size - (image->width * scale)) * 0.5f;
        const float dy = (size - (image->height * scale)) * 0.5f;
        nsvgRasterize(rast, image, dx, dy, scale, rgba.data(), size, size, size * 4);
        nsvgDeleteRasterizer(rast);
        texture = s_host ? s_host->CreateTextureRGBA(rgba.data(), size, size) : 0;
    }
    nsvgDelete(image);
    return texture;
}

void LoadSidebarIcons() {
    const std::string dir = "romfs:/assets/icons/";
    const char* names[] = {"gear.svg", "rotate-left.svg", "right-from-bracket.svg"};
    for (std::size_t i = 0; i < s_side_icons.size(); ++i) {
        s_side_icons[i] = LoadSvgIcon(dir + names[i], 64);
    }
    OverlayUI::SetSidebarIconTextures(static_cast<unsigned long long>(s_side_icons[0]),
                                      static_cast<unsigned long long>(s_side_icons[1]),
                                      static_cast<unsigned long long>(s_side_icons[2]));
}

} // namespace

bool Init(IOverlayHost* host) {
    if (s_initialized) {
        return true;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    if (ImFont* font = AddFirstFont(kFontPaths, 32.0f)) {
        io.FontDefault = font;
    } else {
        LOG_WARN("OVERLAY", "could not load overlay font, using ImGui default");
        io.FontDefault = io.Fonts->AddFontDefault();
    }
    s_description_font = AddFirstFont(kDescriptionFontPaths, 24.0f);
    ImGui::StyleColorsDark();

    if (!OverlayRenderer::Init()) {
        LOG_WARN("OVERLAY", "ImGui Vulkan overlay renderer unavailable");
        ImGui::DestroyContext();
        return false;
    }

#ifdef __SWITCH__
    if (!s_psm_initialized && R_SUCCEEDED(psmInitialize())) {
        s_psm_initialized = true;
    }
#endif
    s_host = host;
    LoadAvatar();
    LoadBorder();
    LoadSidebarIcons();

    s_visible = false;
    s_action = OverlayUI::Action::None;
    s_initialized = true;
    LOG_INFO("OVERLAY", "tico overlay initialized");
    return true;
}

void Shutdown() {
    if (!s_initialized) {
        return;
    }
    for (ImTextureID* texture : {&s_avatar_texture, &s_border_texture, &s_side_icons[0],
                                 &s_side_icons[1], &s_side_icons[2]}) {
        if (*texture && s_host) {
            s_host->DestroyTexture(*texture);
        }
        *texture = 0;
    }
    OverlayUI::SetSidebarIconTextures(0, 0, 0);
    OverlayUI::SetAvatarTextureId(0);
    OverlayUI::SetBorderTextureId(0);
    OverlayRenderer::Shutdown();
    ImGui::DestroyContext();
    OverlayUI::SetVisible(false);
    OverlayUI::ShowToast(std::string{});
#ifdef __SWITCH__
    if (s_psm_initialized) {
        psmExit();
        s_psm_initialized = false;
    }
    if (s_pl_initialized) {
        plExit();
        s_pl_initialized = false;
    }
#endif
    s_host = nullptr;
    s_description_font = nullptr;
    s_initialized = false;
}

void SetVisible(bool visible) {
    s_visible = visible;
    s_nav = {};
    OverlayUI::SetVisible(visible);
}

bool IsVisible() {
    return s_visible;
}

void FeedNav(const OverlayUI::NavInput& nav) {
    s_nav.up |= nav.up;
    s_nav.down |= nav.down;
    s_nav.left |= nav.left;
    s_nav.right |= nav.right;
    s_nav.accept |= nav.accept;
    s_nav.cancel |= nav.cancel;
}

void FeedTouch(const OverlayUI::TouchInput& touch) {
    OverlayUI::FeedTouch(touch);
}

ImDrawData* BuildFrame(float width, float height, float delta_time) {
    if (!s_initialized) {
        return nullptr;
    }
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(width, height);
    io.DeltaTime = delta_time > 0.0f ? delta_time : 1.0f / 60.0f;
    io.FontGlobalScale = height / kDesignHeight;
    OverlayUI::FeedNav(s_nav);
    s_nav = {};

    OverlayRenderer::BeginFrame();
    ImGui::NewFrame();
    const OverlayUI::Action action =
        OverlayUI::Render(static_cast<int>(width), static_cast<int>(height));
    RAAlerts::EnsureIcon(s_host);
    RAAlerts::Render(s_host, ImGui::GetForegroundDrawList(), io.DisplaySize, io.DeltaTime,
                     s_description_font);
    ImGui::Render();
    if (action != OverlayUI::Action::None) {
        s_action = action;
    }
    return ImGui::GetDrawData();
}

OverlayUI::Action ConsumeAction() {
    const OverlayUI::Action action = s_action;
    s_action = OverlayUI::Action::None;
    return action;
}

} // namespace SwitchFrontend::ImGuiOverlay
