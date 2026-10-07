// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 Dan | ticoverse.com
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinNX/Overlay/GameOverlay.h"

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <cstdint>

#include <imgui.h>
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include "Common/CommonTypes.h"
#include "Core/System.h"
#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/AbstractPipeline.h"
#include "VideoCommon/AbstractShader.h"
#include "VideoCommon/AbstractStagingTexture.h"
#include "VideoCommon/AbstractTexture.h"
#include "VideoCommon/FramebufferManager.h"
#include "VideoCommon/FramebufferShaderGen.h"
#include "VideoCommon/NativeVertexFormat.h"
#include "VideoCommon/Present.h"
#include "VideoCommon/RenderState.h"
#include "VideoCommon/TextureConfig.h"
#include "VideoCommon/VertexManagerBase.h"
#include "VideoCommon/PerformanceMetrics.h"

#include "DolphinNX/Achievements.h"
#include "TicoLogger.h"
#include "TicoOverlayHost.h"
#include "overlay/imgui_overlay.h"
#include "overlay/overlay_renderer.h"

namespace DolphinNX::GameOverlay
{
namespace
{
using SwitchFrontend::OverlayUI::Action;
namespace ImGuiOverlay = SwitchFrontend::ImGuiOverlay;
namespace OverlayUI = SwitchFrontend::OverlayUI;

// held directions repeat after this many polls, then every few
constexpr int kNavInitialDelay = 18;
constexpr int kNavRepeat = 5;

enum NavBits : unsigned int
{
  NavBit_Up = 1u << 0,
  NavBit_Down = 1u << 1,
  NavBit_Left = 1u << 2,
  NavBit_Right = 1u << 3,
  NavBit_Accept = 1u << 4,
  NavBit_Cancel = 1u << 5,
};

// --- shared between the main loop and the drawing thread ---
std::atomic_bool s_registered = false;
std::atomic_bool s_visible = false;
std::atomic_bool s_cheat_refresh = false;
std::atomic_bool s_resume_prompt = false;
std::atomic_int s_pending_action = 0;
std::atomic_uint s_pending_nav = 0;
std::atomic_bool s_touch_down = false;
std::atomic<float> s_touch_x = 0.0f;
std::atomic<float> s_touch_y = 0.0f;
std::mutex s_notice_mutex;
std::optional<std::pair<std::string, std::vector<std::string>>> s_pending_notice;

// --- main loop only ---
bool s_was_combo_down = false;
u64 s_nav_held_prev = 0;
int s_nav_repeat = 0;
bool s_accept_prev = false;
bool s_cancel_prev = false;

// --- drawing thread only (or the main loop while the core is paused) ---
bool s_ready = false;
bool s_failed = false;
bool s_shown = false;
std::chrono::steady_clock::time_point s_last_frame;
// The overlay's renderer, on Dolphin's AbstractGfx so it draws on any backend
// (Vulkan, deko3d), as Dolphin's own on-screen UI does.
std::unique_ptr<NativeVertexFormat> s_vertex_format;
std::unique_ptr<AbstractPipeline> s_pipeline;
// the overlay's pictures (avatar, icons, covers) and ImGui's font atlas
std::map<ImTextureID, std::unique_ptr<AbstractTexture>> s_textures;

ImTextureID ToTextureID(const AbstractTexture* texture)
{
  return static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(texture));
}

const AbstractTexture* FromTextureID(ImTextureID id)
{
  return reinterpret_cast<const AbstractTexture*>(static_cast<std::uintptr_t>(id));
}

std::unique_ptr<AbstractTexture> CreateTexture(u32 width, u32 height, const u8* rgba)
{
  const TextureConfig config(width, height, 1, 1, 1, AbstractTextureFormat::RGBA8, 0,
                             AbstractTextureType::Texture_2DArray);
  auto texture = g_gfx->CreateTexture(config, "TicoOverlayTexture");
  if (texture && rgba)
    texture->Load(0, width, height, width, rgba, static_cast<size_t>(width) * height * 4);
  return texture;
}

// The overlay's textures (avatar, selection border, icons) as AbstractGfx
// textures ImGui can sample. Called while drawing, where Dolphin records.
class Host final : public IOverlayHost
{
public:
  std::string GetGamePath() override { return {}; }
  bool IsGameLoaded() override { return true; }
  bool StateSlotExists(int) override { return false; }
  void SaveStateSlot(int) override {}
  void LoadStateSlot(int) override {}
  void SwapDisc(const std::string&) override {}
  IOverlayRAHost* RA() override { return DolphinNX::Achievements::Host(); }

  ImTextureID CreateTextureRGBA(const unsigned char* rgba, int width, int height) override
  {
    if (!rgba || width <= 0 || height <= 0 || !g_gfx || !s_pipeline)
      return 0;
    auto texture = CreateTexture(static_cast<u32>(width), static_cast<u32>(height), rgba);
    if (!texture)
      return 0;
    const ImTextureID id = ToTextureID(texture.get());
    s_textures[id] = std::move(texture);
    return id;
  }

  void DestroyTexture(ImTextureID texture) override { s_textures.erase(texture); }
};
Host s_host;

// ImGui 1.92 leaves its font atlas to the renderer: create, update, destroy.
void UpdateImGuiTexture(ImTextureData* tex)
{
  if (tex->Status == ImTextureStatus_WantCreate)
  {
    auto texture = CreateTexture(static_cast<u32>(tex->Width), static_cast<u32>(tex->Height),
                                 static_cast<const u8*>(tex->GetPixels()));
    if (!texture)
      return;
    const ImTextureID id = ToTextureID(texture.get());
    s_textures[id] = std::move(texture);
    tex->SetTexID(id);
    tex->SetStatus(ImTextureStatus_OK);
  }
  else if (tex->Status == ImTextureStatus_WantUpdates)
  {
    auto* texture = const_cast<AbstractTexture*>(FromTextureID(tex->GetTexID()));
    if (!texture)
      return;
    for (const ImTextureRect& r : tex->Updates)
    {
      const TextureConfig config(r.w, r.h, 1, 1, 1, AbstractTextureFormat::RGBA8, 0,
                                 AbstractTextureType::Texture_2DArray);
      auto stage = g_gfx->CreateStagingTexture(StagingTextureType::Upload, config);
      if (!stage)
        continue;
      for (int y = 0; y < r.h; ++y)
      {
        stage->WriteTexels({0, y, r.w, y + 1}, tex->GetPixelsAt(r.x, r.y + y),
                           r.w * tex->BytesPerPixel);
      }
      stage->CopyToTexture({0, 0, r.w, r.h}, texture, {r.x, r.y, r.x + r.w, r.y + r.h}, 0, 0);
    }
    tex->SetStatus(ImTextureStatus_OK);
  }
  else if (tex->Status == ImTextureStatus_WantDestroy && tex->UnusedFrames > 0)
  {
    s_textures.erase(tex->GetTexID());
    tex->SetTexID(ImTextureID_Invalid);
    tex->SetStatus(ImTextureStatus_Destroyed);
  }
}

// Draws the overlay's ImGui frame on the bound backbuffer, as OnScreenUI::DrawImGui.
void RenderDrawData(ImDrawData* draw_data, u32 width, u32 height)
{
  if (draw_data->Textures)
  {
    for (ImTextureData* tex : *draw_data->Textures)
    {
      if (tex->Status != ImTextureStatus_OK)
        UpdateImGuiTexture(tex);
    }
  }

  g_gfx->SetViewport(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f,
                     1.0f);
  struct ImGuiUbo
  {
    float u_rcp_viewport_size_mul2[2];
    float padding[2];
  };
  const ImGuiUbo ubo = {{1.0f / width * 2.0f, 1.0f / height * 2.0f}, {}};
  g_gfx->SetPipeline(s_pipeline.get());
  g_gfx->SetSamplerState(0, RenderState::GetLinearSamplerState());
  g_vertex_manager->UploadUtilityUniforms(&ubo, sizeof(ubo));

  for (int i = 0; i < draw_data->CmdListsCount; ++i)
  {
    const ImDrawList* list = draw_data->CmdLists[i];
    if (list->VtxBuffer.empty() || list->IdxBuffer.empty())
      continue;
    u32 base_vertex = 0;
    u32 base_index = 0;
    g_vertex_manager->UploadUtilityVertices(list->VtxBuffer.Data, sizeof(ImDrawVert),
                                            list->VtxBuffer.Size, list->IdxBuffer.Data,
                                            list->IdxBuffer.Size, &base_vertex, &base_index);
    for (const ImDrawCmd& cmd : list->CmdBuffer)
    {
      if (cmd.UserCallback)
      {
        cmd.UserCallback(list, &cmd);
        continue;
      }
      g_gfx->SetScissorRect(g_gfx->ConvertFramebufferRectangle(
          MathUtil::Rectangle<int>(static_cast<int>(cmd.ClipRect.x),
                                   static_cast<int>(cmd.ClipRect.y),
                                   static_cast<int>(cmd.ClipRect.z),
                                   static_cast<int>(cmd.ClipRect.w)),
          g_gfx->GetCurrentFramebuffer()));
      g_gfx->SetTexture(0, FromTextureID(cmd.GetTexID()));
      g_gfx->DrawIndexed(base_index + cmd.IdxOffset, cmd.ElemCount, base_vertex + cmd.VtxOffset);
    }
  }

  g_gfx->SetScissorRect(g_gfx->ConvertFramebufferRectangle(
      MathUtil::Rectangle<int>(0, 0, static_cast<int>(width), static_cast<int>(height)),
      g_gfx->GetCurrentFramebuffer()));
}

void PublishHudStats()
{
  OverlayUI::HudStats stats;
  stats.fps = static_cast<float>(Core::System::GetInstance().GetPerfMetrics().GetFPS());
  if (g_framebuffer_manager)
  {
    stats.rendered_width = static_cast<int>(g_framebuffer_manager->GetEFBWidth());
    stats.rendered_height = static_cast<int>(g_framebuffer_manager->GetEFBHeight());
  }
  OverlayUI::SetHudStats(stats);
}

void Hide()
{
  s_visible.store(false);
  s_pending_nav.store(0);
  ImGuiOverlay::SetVisible(false);
  s_shown = false;
}

void DrawOverlay(u32 width, u32 height)
{
  if (!s_registered.load() || s_failed)
    return;

  if (!s_ready)
  {
    if (!ImGuiOverlay::Init(&s_host))
    {
      LOG_ERROR("OVERLAY", "the overlay could not start; the game runs without a menu");
      s_failed = true;
      return;
    }
    OverlayUI::ReloadSettings();
    s_last_frame = std::chrono::steady_clock::now();
    s_ready = true;
  }

  {
    std::lock_guard lock(s_notice_mutex);
    if (s_pending_notice)
    {
      s_visible.store(true);
      ImGuiOverlay::SetVisible(true);
      s_shown = true;
      OverlayUI::ShowNotice(std::move(s_pending_notice->first),
                            std::move(s_pending_notice->second));
      s_pending_notice.reset();
    }
  }

  if (s_cheat_refresh.exchange(false))
    OverlayUI::RefreshCheatList();

  if (s_resume_prompt.exchange(false))
  {
    s_visible.store(true);
    ImGuiOverlay::SetVisible(true);
    s_shown = true;
    OverlayUI::ShowResumePrompt();
  }

  const bool visible = s_visible.load();
  if (visible != s_shown)
  {
    ImGuiOverlay::SetVisible(visible);
    s_shown = visible;
  }

  PublishHudStats();
  const auto now = std::chrono::steady_clock::now();
  const float delta = std::chrono::duration<float>(now - s_last_frame).count();
  s_last_frame = now;
  DolphinNX::Achievements::UploadBadges(s_host);
  if (!visible && !OverlayUI::HasTransientContent() && !DolphinNX::Achievements::HasNotifications())
    return;

  if (visible)
  {
    const unsigned int nav = s_pending_nav.exchange(0);
    ImGuiOverlay::FeedNav({
        .up = (nav & NavBit_Up) != 0,
        .down = (nav & NavBit_Down) != 0,
        .left = (nav & NavBit_Left) != 0,
        .right = (nav & NavBit_Right) != 0,
        .accept = (nav & NavBit_Accept) != 0,
        .cancel = (nav & NavBit_Cancel) != 0,
    });
    ImGuiOverlay::FeedTouch({s_touch_down.load(), s_touch_x.load(), s_touch_y.load()});
  }

  ImDrawData* draw_data = ImGuiOverlay::BuildFrame(static_cast<float>(width),
                                                   static_cast<float>(height),
                                                   delta > 0.0f && delta < 0.25f ? delta : 1.0f / 60.0f);

  const Action action = ImGuiOverlay::ConsumeAction();
  if (action != Action::None)
  {
    s_pending_action.store(static_cast<int>(action));
    if (action == Action::Resume)
      Hide();
  }

  if (!draw_data || !s_pipeline)
    return;
  RenderDrawData(draw_data, width, height);
}
}  // namespace

bool Init()
{
  if (s_registered.load())
    return true;

  // once Dolphin presents: its backbuffer's format is what the pipeline draws to
  if (!g_gfx || !g_presenter || !g_vertex_manager ||
      g_presenter->GetBackbufferFormat() == AbstractTextureFormat::Undefined)
  {
    return false;
  }

  s_visible.store(false);
  s_pending_action.store(0);
  s_pending_nav.store(0);
  hidInitializeTouchScreen();

  VideoCommon::SetHostOverlayCallback(&DrawOverlay);
  s_registered.store(true);
  return true;
}

void Shutdown()
{
  if (!s_registered.load())
    return;

  VideoCommon::SetHostOverlayCallback(nullptr);
  if (g_gfx)
    g_gfx->WaitForGPUIdle();
  s_registered.store(false);
  if (s_ready)
    ImGuiOverlay::Shutdown();
  s_ready = false;
  s_failed = false;
  s_shown = false;
  s_visible.store(false);
  s_pending_action.store(0);
  s_pending_nav.store(0);
}

void Update(PadState* pad)
{
  if (!s_registered.load() || !pad)
    return;

  const u64 held = padGetButtons(pad);
  const bool combo_down =
      (held & HidNpadButton_Plus) != 0 && (held & HidNpadButton_Minus) != 0;
  if (combo_down && !s_was_combo_down)
  {
    const bool open = !s_visible.load();
    s_visible.store(open);
    s_pending_nav.store(0);
  }
  s_was_combo_down = combo_down;

  if (!s_visible.load())
  {
    s_nav_held_prev = 0;
    s_accept_prev = (held & HidNpadButton_A) != 0;
    s_cancel_prev = (held & HidNpadButton_B) != 0;
    s_touch_down.store(false);
    return;
  }

  // directions fire on press, then repeat while held
  u64 directions = held & (HidNpadButton_AnyUp | HidNpadButton_AnyDown | HidNpadButton_AnyLeft |
                           HidNpadButton_AnyRight);
  u64 fire = directions & ~s_nav_held_prev;
  if (directions != 0 && directions == s_nav_held_prev)
  {
    if (--s_nav_repeat <= 0)
    {
      fire |= directions;
      s_nav_repeat = kNavRepeat;
    }
  }
  else if (fire != 0)
  {
    s_nav_repeat = kNavInitialDelay;
  }
  s_nav_held_prev = directions;

  const bool accept = (held & HidNpadButton_A) != 0;
  const bool cancel = (held & HidNpadButton_B) != 0;
  unsigned int nav = 0;
  if (fire & HidNpadButton_AnyUp)
    nav |= NavBit_Up;
  if (fire & HidNpadButton_AnyDown)
    nav |= NavBit_Down;
  if (fire & HidNpadButton_AnyLeft)
    nav |= NavBit_Left;
  if (fire & HidNpadButton_AnyRight)
    nav |= NavBit_Right;
  if (accept && !s_accept_prev)
    nav |= NavBit_Accept;
  if (cancel && !s_cancel_prev)
    nav |= NavBit_Cancel;
  s_accept_prev = accept;
  s_cancel_prev = cancel;
  if (nav != 0)
    s_pending_nav.fetch_or(nav);

  HidTouchScreenState touch{};
  if (hidGetTouchScreenStates(&touch, 1) && touch.count > 0)
  {
    s_touch_x.store(static_cast<float>(touch.touches[0].x));
    s_touch_y.store(static_cast<float>(touch.touches[0].y));
    s_touch_down.store(true);
  }
  else
  {
    s_touch_down.store(false);
  }
}

bool IsVisible()
{
  return s_visible.load();
}

void SetVisible(bool visible)
{
  s_visible.store(visible);
  s_pending_nav.store(0);
}

void ShowNotice(std::string message, std::vector<std::string> choices)
{
  std::lock_guard lock(s_notice_mutex);
  s_pending_notice.emplace(std::move(message), std::move(choices));
  s_visible.store(true);
}

void ShowResumePrompt()
{
  s_resume_prompt.store(true);
  s_visible.store(true);
}

unsigned long long LoadPicture(const std::string& path, float* aspect)
{
  int width = 0;
  int height = 0;
  int channels = 0;
  unsigned char* rgba = stbi_load(path.c_str(), &width, &height, &channels, 4);
  if (!rgba)
    return 0;
  const ImTextureID texture = s_host.CreateTextureRGBA(rgba, width, height);
  stbi_image_free(rgba);
  if (aspect && height > 0)
    *aspect = static_cast<float>(width) / static_cast<float>(height);
  return static_cast<unsigned long long>(texture);
}

void FreePicture(unsigned long long texture)
{
  if (texture)
    s_host.DestroyTexture(static_cast<ImTextureID>(texture));
}

void RequestCheatRefresh()
{
  s_cheat_refresh.store(true);
}

Action ConsumeAction()
{
  return static_cast<Action>(s_pending_action.exchange(0));
}
}  // namespace DolphinNX::GameOverlay

// The renderer tico's overlay draws with: Dolphin's AbstractGfx, with the ImGui
// shaders Dolphin's own on-screen UI uses, on whatever backend presents.
namespace DolphinNX::GameOverlay
{
bool RendererInit()
{
  if (!g_gfx || !g_presenter)
    return false;

  PortableVertexDeclaration vdecl = {};
  vdecl.position = {ComponentFormat::Float, 2, offsetof(ImDrawVert, pos), true, false};
  vdecl.texcoords[0] = {ComponentFormat::Float, 2, offsetof(ImDrawVert, uv), true, false};
  vdecl.colors[0] = {ComponentFormat::UByte, 4, offsetof(ImDrawVert, col), true, false};
  vdecl.stride = sizeof(ImDrawVert);
  s_vertex_format = g_gfx->CreateNativeVertexFormat(vdecl);

  const bool linear_space_output =
      g_presenter->GetBackbufferFormat() == AbstractTextureFormat::RGBA16F;
  const auto vertex_shader = g_gfx->CreateShaderFromSource(
      ShaderStage::Vertex, FramebufferShaderGen::GenerateImGuiVertexShader(), nullptr,
      "Tico overlay vertex shader");
  const auto pixel_shader = g_gfx->CreateShaderFromSource(
      ShaderStage::Pixel, FramebufferShaderGen::GenerateImGuiPixelShader(linear_space_output),
      nullptr, "Tico overlay pixel shader");
  if (!s_vertex_format || !vertex_shader || !pixel_shader)
  {
    RendererShutdown();
    return false;
  }

  AbstractPipelineConfig config = {};
  config.vertex_format = s_vertex_format.get();
  config.vertex_shader = vertex_shader.get();
  config.pixel_shader = pixel_shader.get();
  config.rasterization_state = RenderState::GetNoCullRasterizationState(PrimitiveType::Triangles);
  config.depth_state = RenderState::GetNoDepthTestingDepthState();
  config.blending_state = RenderState::GetNoBlendingBlendState();
  config.blending_state.blend_enable = true;
  config.blending_state.src_factor = SrcBlendFactor::SrcAlpha;
  config.blending_state.dst_factor = DstBlendFactor::InvSrcAlpha;
  config.blending_state.src_factor_alpha = SrcBlendFactor::Zero;
  config.blending_state.dst_factor_alpha = DstBlendFactor::One;
  config.framebuffer_state.color_texture_format = g_presenter->GetBackbufferFormat();
  config.framebuffer_state.depth_texture_format = AbstractTextureFormat::Undefined;
  config.framebuffer_state.samples = 1;
  config.framebuffer_state.per_sample_shading = false;
  config.usage = AbstractPipelineUsage::Utility;
  s_pipeline = g_gfx->CreatePipeline(config);
  if (!s_pipeline)
  {
    RendererShutdown();
    return false;
  }

  ImGuiIO& io = ImGui::GetIO();
  io.BackendRendererName = "tico_dolphin_gfx";
  io.BackendRendererUserData = &s_pipeline;
  // the font atlas comes to RenderDrawData; draws use their vertex offsets
  io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset;
  return true;
}

void RendererShutdown()
{
  if (ImGui::GetCurrentContext())
  {
    // the atlas textures ImGui still holds go with the renderer
    for (ImTextureData* tex : ImGui::GetPlatformIO().Textures)
    {
      if (tex->RefCount == 1)
      {
        tex->SetTexID(ImTextureID_Invalid);
        tex->SetStatus(ImTextureStatus_Destroyed);
      }
    }
    ImGuiIO& io = ImGui::GetIO();
    io.BackendRendererName = nullptr;
    io.BackendRendererUserData = nullptr;
    io.BackendFlags &=
        ~(ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset);
  }
  s_textures.clear();
  s_pipeline.reset();
  s_vertex_format.reset();
}

void RendererBeginFrame()
{
}
}  // namespace DolphinNX::GameOverlay
