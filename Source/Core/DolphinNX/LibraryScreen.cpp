// Copyright 2026 Dan | ticoverse.com
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinNX/LibraryScreen.h"

#include <chrono>
#include <cstdint>

#include <SDL.h>
#include <imgui.h>
#include <imgui_impl_sdlrenderer2.h>
#include <switch.h>

#include "DolphinNX/Library.h"
#include "TicoOverlayHost.h"
#include "overlay/imgui_overlay.h"
#include "overlay/overlay_ui.h"
#include "overlay/tico_config.h"

namespace DolphinNX::LibraryScreen
{
namespace
{
namespace ImGuiOverlay = SwitchFrontend::ImGuiOverlay;
namespace OverlayUI = SwitchFrontend::OverlayUI;

// held directions repeat after this many frames, then every few
constexpr int kNavInitialDelay = 18;
constexpr int kNavRepeat = 5;

bool s_active = false;
SDL_Window* s_window = nullptr;
SDL_Renderer* s_renderer = nullptr;

// The overlay's textures (avatar, border, icons, covers) as SDL textures.
class Host final : public IOverlayHost
{
public:
  std::string GetGamePath() override { return {}; }
  bool IsGameLoaded() override { return false; }
  bool StateSlotExists(int) override { return false; }
  void SaveStateSlot(int) override {}
  void LoadStateSlot(int) override {}
  void SwapDisc(const std::string&) override {}

  ImTextureID CreateTextureRGBA(const unsigned char* rgba, int width, int height) override
  {
    if (!s_renderer || !rgba || width <= 0 || height <= 0)
      return 0;
    SDL_Texture* texture = SDL_CreateTexture(s_renderer, SDL_PIXELFORMAT_RGBA32,
                                             SDL_TEXTUREACCESS_STATIC, width, height);
    if (!texture)
      return 0;
    SDL_UpdateTexture(texture, nullptr, rgba, width * 4);
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    return static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(texture));
  }

  void DestroyTexture(ImTextureID texture) override
  {
    if (texture)
      SDL_DestroyTexture(reinterpret_cast<SDL_Texture*>(static_cast<std::uintptr_t>(texture)));
  }
};
Host s_host;

struct Nav
{
  u64 held_prev = 0;
  int repeat = 0;
};

// Edge-triggered menu input, with held directions repeating.
OverlayUI::NavInput ReadNav(PadState& pad, Nav& nav)
{
  const u64 held = padGetButtons(&pad);
  const u64 pressed = padGetButtonsDown(&pad);
  const u64 directions =
      held & (HidNpadButton_AnyUp | HidNpadButton_AnyDown | HidNpadButton_AnyLeft |
              HidNpadButton_AnyRight);
  u64 fire = directions & ~nav.held_prev;
  if (directions != 0 && directions == nav.held_prev)
  {
    if (--nav.repeat <= 0)
    {
      fire |= directions;
      nav.repeat = kNavRepeat;
    }
  }
  else if (fire != 0)
  {
    nav.repeat = kNavInitialDelay;
  }
  nav.held_prev = directions;
  return {
      .up = (fire & HidNpadButton_AnyUp) != 0,
      .down = (fire & HidNpadButton_AnyDown) != 0,
      .left = (fire & HidNpadButton_AnyLeft) != 0,
      .right = (fire & HidNpadButton_AnyRight) != 0,
      .accept = (pressed & HidNpadButton_A) != 0,
      .cancel = (pressed & HidNpadButton_B) != 0,
  };
}

void Close()
{
  if (s_renderer)
    SDL_DestroyRenderer(s_renderer);
  if (s_window)
    SDL_DestroyWindow(s_window);
  s_renderer = nullptr;
  s_window = nullptr;
  SDL_QuitSubSystem(SDL_INIT_VIDEO);
}
}  // namespace

std::optional<std::string> Run()
{
  if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0)
    return std::nullopt;
  s_window = SDL_CreateWindow("Dolphin", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 1280,
                              720, 0);
  s_renderer = s_window ? SDL_CreateRenderer(s_window, -1,
                                             SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC) :
                          nullptr;
  if (!s_renderer)
  {
    Close();
    return std::nullopt;
  }

  // the settings are the core's, not a game's
  SwitchFrontend::TicoConfig::ReloadConfig();
  SwitchFrontend::TicoConfig::SetGame("");

  std::optional<std::string> chosen;
  Library::Register([&chosen](const std::string& path, const std::string&) { chosen = path; });
  OverlayUI::SetGameTitle("Dolphin");
  OverlayUI::SetLibraryMode(true);

  s_active = true;
  if (!ImGuiOverlay::Init(&s_host))
  {
    s_active = false;
    Library::Unregister();
    Close();
    return std::nullopt;
  }
  OverlayUI::ReloadSettings();
  ImGuiOverlay::SetVisible(true);

  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  PadState pad;
  padInitializeDefault(&pad);
  hidInitializeTouchScreen();
  Nav nav;
  auto last = std::chrono::steady_clock::now();

  while (appletMainLoop() && !chosen)
  {
    padUpdate(&pad);
    ImGuiOverlay::FeedNav(ReadNav(pad, nav));
    HidTouchScreenState touch{};
    const bool touching = hidGetTouchScreenStates(&touch, 1) && touch.count > 0;
    ImGuiOverlay::FeedTouch({touching, touching ? static_cast<float>(touch.touches[0].x) : 0.0f,
                             touching ? static_cast<float>(touch.touches[0].y) : 0.0f});

    int width = 1280;
    int height = 720;
    SDL_GetRendererOutputSize(s_renderer, &width, &height);
    const auto now = std::chrono::steady_clock::now();
    const float delta = std::chrono::duration<float>(now - last).count();
    last = now;
    ImDrawData* draw_data = ImGuiOverlay::BuildFrame(static_cast<float>(width),
                                                     static_cast<float>(height), delta);

    SDL_SetRenderDrawColor(s_renderer, 0, 0, 0, 255);
    SDL_RenderClear(s_renderer);
    if (draw_data)
      ImGui_ImplSDLRenderer2_RenderDrawData(draw_data, s_renderer);
    SDL_RenderPresent(s_renderer);

    // Exit leaves the list (and the app); the menu always stays open
    const OverlayUI::Action action = ImGuiOverlay::ConsumeAction();
    if (action == OverlayUI::Action::Exit)
      break;
    if (action == OverlayUI::Action::Resume)
      ImGuiOverlay::SetVisible(true);
  }

  ImGuiOverlay::Shutdown();
  s_active = false;
  OverlayUI::SetLibraryMode(false);
  Library::Unregister();
  Close();
  return chosen;
}

bool IsActive()
{
  return s_active;
}

bool RendererInit()
{
  return s_renderer && ImGui_ImplSDLRenderer2_Init(s_renderer);
}

void RendererShutdown()
{
  if (ImGui::GetCurrentContext() && ImGui::GetIO().BackendRendererUserData)
    ImGui_ImplSDLRenderer2_Shutdown();
}

void RendererBeginFrame()
{
  ImGui_ImplSDLRenderer2_NewFrame();
}
}  // namespace DolphinNX::LibraryScreen
