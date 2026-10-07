// Copyright 2026 Dan | ticoverse.com
// SPDX-License-Identifier: GPL-2.0-or-later

#include "overlay/ra_alerts.h"

#include "TicoOverlayHost.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <string>

// Nanosvg rasterizes the generic RA icon; badges come decoded from the host.
#define NANOSVG_IMPLEMENTATION
#include "deps/nanosvg/nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "deps/nanosvg/nanosvgrast.h"

namespace SwitchFrontend::RAAlerts {

namespace {
void ResolveNotificationTextures(IOverlayHost* host) {
    IOverlayRAHost *ra = host ? host->RA() : nullptr;
    if (!ra) return;
    std::lock_guard<std::mutex> lock(ra->Mutex());
    auto& notifications = ra->Notifications();
    if (notifications.empty()) return;

    for (auto& n : notifications) {
        if (n.textureId != 0) continue;
        if (n.badge_name.empty()) continue;

        if (n.badge_name == "ra_icon") {
            n.textureId = ra->IconTexture();
        } else {
            // Only check the in-memory texture cache — NO disk I/O, NO stbi_load.
            ImTextureID t = ra->BadgeTexture(n.badge_name);
            if (t != 0)
                n.textureId = t;
            // If not cached yet, leave textureId=0; the badge appears once the
            // host uploads it (next frame).
        }
    }
}

} // namespace

void EnsureIcon(IOverlayHost* host) {
    // Rasterize assets/ra.svg once and upload it through the host so RA toasts
    // (the "Playing:" session toast, mastered/leaderboard alerts) have an icon,
    // matching the other Tico cores. Badge textures for individual achievements
    // are still uploaded by the host on demand; this only owns the generic icon.
    if (!host)
        return;
    IOverlayRAHost *ra = host->RA();
    if (!ra)
        return;                       // standalone path: no RA host
    if (ra->IconTexture() != 0)
        return;                       // already uploaded

    // Retry every frame until the overlay backend is ready (CreateTextureRGBA
    // returns 0 before the Vulkan overlay is up); cheap until it succeeds.
#ifdef __SWITCH__
    const char *svgPath = "romfs:/assets/ra.svg";
#else
    const char *svgPath = "tico/assets/ra.svg";
#endif
    NSVGimage *image = nsvgParseFromFile(svgPath, "px", 96);
    if (!image)
        return;

    float scale = 64.0f / image->height;
    int w = (int)(image->width * scale);
    int h = (int)(image->height * scale);
    if (w <= 0 || h <= 0) {
        nsvgDelete(image);
        return;
    }

    NSVGrasterizer *rast = nsvgCreateRasterizer();
    if (!rast) {
        nsvgDelete(image);
        return;
    }

    unsigned char *img = (unsigned char *)malloc((size_t)w * h * 4);
    if (!img) {
        nsvgDeleteRasterizer(rast);
        nsvgDelete(image);
        return;
    }

    nsvgRasterize(rast, image, 0, 0, scale, img, w, h, w * 4);
    ImTextureID tex = host->CreateTextureRGBA(img, w, h);
    if (tex != 0) {
        ra->SetIconTexture(tex);
    }

    free(img);
    nsvgDeleteRasterizer(rast);
    nsvgDelete(image);
}

bool HasAlerts(IOverlayHost* host) {
    IOverlayRAHost *ra = host ? host->RA() : nullptr;
    if (!ra) return false;
    std::lock_guard<std::mutex> lock(ra->Mutex());
    return !ra->Notifications().empty();
}

void Render(IOverlayHost* host, ImDrawList* dl, ImVec2 displaySize, float deltaTime,
            ImFont* description_font) {
    ResolveNotificationTextures(host);
    IOverlayRAHost *ra = host ? host->RA() : nullptr;
    if (!ra) return;
    std::lock_guard<std::mutex> lock(ra->Mutex());
    auto& notifications = ra->Notifications();
    if (notifications.empty()) return;

    const float scale = ImGui::GetIO().FontGlobalScale > 0.0f ? ImGui::GetIO().FontGlobalScale : 1.0f;
    ImFont *font = ImGui::GetFont();
    ImFont *descFont = description_font ? description_font : font;
    float descFontSize = ImGui::GetFontSize() * 0.65f;
    float titleFontSize = ImGui::GetFontSize() * 0.85f;

    // Alert dimensions
    float alertW = 420.0f * scale;
    float alertH = 100.0f * scale;
    float padding = 12.0f * scale;
    float margin = 16.0f * scale;
    float spacing = 8.0f * scale;
    float cornerRadius = 14.0f * scale;
    float badgeSize = 76.0f * scale;
    float badgeRadius = 4.0f * scale;
    float badgeMargin = 12.0f * scale;

    RAAlertPosition pos = ra->AlertPosition();
    bool isTop = (pos == RAAlertPosition::TopLeft || pos == RAAlertPosition::TopRight);
    bool isRight = (pos == RAAlertPosition::TopRight || pos == RAAlertPosition::BottomRight);

    // Update timers and remove expired
    for (auto& n : notifications) {
        n.timer += deltaTime;
    }
    notifications.erase(
        std::remove_if(notifications.begin(), notifications.end(),
            [](const RANotification& n) { return n.timer >= n.duration; }),
        notifications.end());

    // Render each notification
    for (size_t i = 0; i < notifications.size(); i++) {
        auto& n = notifications[i];

        // Calculate slide animation
        float slideProgress;
        if (n.timer < n.slideIn) {
            float t = n.timer / n.slideIn;
            slideProgress = 1.0f - std::pow(1.0f - t, 3.0f);
        } else if (n.timer > n.duration - n.slideOut) {
            float t = (n.duration - n.timer) / n.slideOut;
            slideProgress = 1.0f - std::pow(1.0f - t, 3.0f);
        } else {
            slideProgress = 1.0f;
        }

        // Calculate position
        float stackOffset = (float)i * (alertH + spacing);
        float anchorX = isRight ? (displaySize.x - alertW - margin) : margin;
        float anchorY = isTop ? (margin + stackOffset) : (displaySize.y - margin - alertH - stackOffset);
        float slideOffsetY = isTop
            ? -(alertH + margin + stackOffset) * (1.0f - slideProgress)
            : (alertH + margin + stackOffset) * (1.0f - slideProgress);

        float drawY = anchorY + slideOffsetY;
        int alpha = (int)(230 * slideProgress);
        if (alpha <= 0) continue;

        ImVec2 rectMin(anchorX, drawY);
        ImVec2 rectMax(anchorX + alertW, drawY + alertH);

        // Background — glassmorphic rounded rectangle
        ImU32 bgColor = IM_COL32(35, 35, 40, alpha);
        ImU32 borderColor = IM_COL32(70, 70, 80, (int)(180 * slideProgress));

        dl->AddRectFilled(rectMin, rectMax, bgColor, cornerRadius);
        dl->AddRect(rectMin, rectMax, borderColor, cornerRadius, 0, 1.5f * scale);

        // Badge image (left side)
        float textX = rectMin.x + padding;
        if (n.textureId != 0) {
            float badgeX = rectMin.x + badgeMargin;
            float badgeY = rectMin.y + (alertH - badgeSize) * 0.5f;

            float drawBadgeSize = badgeSize;
            float drawBadgeX = badgeX;
            float drawBadgeY = badgeY;

            // Make the general RA icon a bit smaller to fit visually better
            if (n.badge_name == "ra_icon") {
                drawBadgeSize = badgeSize * 0.70f;
                drawBadgeX += (badgeSize - drawBadgeSize) * 0.5f;
                drawBadgeY += (badgeSize - drawBadgeSize) * 0.5f;
            }

            ImVec2 bMin(drawBadgeX, drawBadgeY);
            ImVec2 bMax(drawBadgeX + drawBadgeSize, drawBadgeY + drawBadgeSize);
            ImU32 imgCol = IM_COL32(255, 255, 255, alpha);
            dl->AddImageRounded(n.textureId,
                bMin, bMax, ImVec2(0,0), ImVec2(1,1), imgCol, badgeRadius);
            
            textX = badgeX + badgeSize + badgeMargin;
        }

        // Text colors
        ImU32 descColor = IM_COL32(185, 185, 195, alpha);
        float maxDescW = rectMax.x - textX - padding;

        ImU32 titleColor = IM_COL32(255, 255, 255, alpha);

        std::string desc = n.description;
        float maxDescH = descFontSize * 2.5f;
        ImVec2 fullSize = descFont->CalcTextSizeA(descFontSize, FLT_MAX, maxDescW, desc.c_str());
        
        // If content goes through 2 lines, slice and add '...'
        if (fullSize.y > maxDescH) {
            desc += "...";
            while (desc.length() > 4) {
                ImVec2 testSize = descFont->CalcTextSizeA(descFontSize, FLT_MAX, maxDescW, desc.c_str());
                if (testSize.y <= maxDescH) break;
                desc.erase(desc.length() - 4, 1);
            }
        }

        std::string titleStr = n.title;
        ImVec2 titleSize = font->CalcTextSizeA(titleFontSize, FLT_MAX, 0.0f, titleStr.c_str());
        if (titleSize.x > maxDescW) {
            titleStr += "...";
            while (titleStr.length() > 4) {
                ImVec2 testSize = font->CalcTextSizeA(titleFontSize, FLT_MAX, 0.0f, titleStr.c_str());
                if (testSize.x <= maxDescW) break;
                titleStr.erase(titleStr.length() - 4, 1);
            }
            titleSize = font->CalcTextSizeA(titleFontSize, FLT_MAX, 0.0f, titleStr.c_str());
        }

        ImVec2 descSize = descFont->CalcTextSizeA(descFontSize, FLT_MAX, maxDescW, desc.c_str());
        
        float textSpacing = 4.0f * scale;
        float totalTextH = titleSize.y + textSpacing + descSize.y;
        float titleY = rectMin.y + (alertH - totalTextH) * 0.5f;
        float descY = titleY + titleSize.y + textSpacing;

        // Title shadow + text
        dl->AddText(font, titleFontSize,
            ImVec2(textX + 1.0f, titleY + 1.0f),
            IM_COL32(0, 0, 0, (int)(80 * slideProgress)),
            titleStr.c_str());
        dl->AddText(font, titleFontSize,
            ImVec2(textX, titleY), titleColor, titleStr.c_str());

        // Description
        dl->AddText(descFont, descFontSize, ImVec2(textX, descY), descColor, desc.c_str(), nullptr, maxDescW);
    }
}

} // namespace SwitchFrontend::RAAlerts
