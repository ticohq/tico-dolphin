// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 Dan | ticoverse.com
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinNX/Overlay/VulkanOverlay.h"

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <imgui.h>
#include <imgui_impl_vulkan.h>
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include "Common/CommonTypes.h"
#include "Core/System.h"
#include "VideoBackends/Vulkan/VKGfx.h"
#include "VideoBackends/Vulkan/VKSwapChain.h"
#include "VideoBackends/Vulkan/VKTexture.h"
#include "VideoBackends/Vulkan/VulkanContext.h"
#include "VideoBackends/Vulkan/VulkanLoader.h"
#include "VideoCommon/FramebufferManager.h"
#include "VideoCommon/PerformanceMetrics.h"

#include "DolphinNX/Achievements.h"
#include "TicoLogger.h"
#include "TicoOverlayHost.h"
#include "overlay/imgui_overlay.h"
#include "overlay/overlay_renderer.h"

namespace DolphinNX::VulkanOverlay
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
VkDevice s_device = VK_NULL_HANDLE;
VkRenderPass s_render_pass = VK_NULL_HANDLE;
u32 s_image_count = 2;
VkDescriptorPool s_descriptor_pool = VK_NULL_HANDLE;
VkSampler s_sampler = VK_NULL_HANDLE;
std::map<ImTextureID, std::unique_ptr<Vulkan::VKTexture>> s_textures;

// The overlay's textures (avatar, selection border, icons) as Vulkan
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
    if (!rgba || width <= 0 || height <= 0 || s_sampler == VK_NULL_HANDLE)
      return 0;
    const TextureConfig config(static_cast<u32>(width), static_cast<u32>(height), 1, 1, 1,
                               AbstractTextureFormat::RGBA8, 0, AbstractTextureType::Texture_2D);
    auto texture = Vulkan::VKTexture::Create(config, "TicoOverlayTexture");
    if (!texture)
      return 0;
    const std::size_t size = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4;
    texture->Load(0, static_cast<u32>(width), static_cast<u32>(height), static_cast<u32>(width),
                  rgba, size, 0);
    const VkDescriptorSet set =
        ImGui_ImplVulkan_AddTexture(s_sampler, texture->GetView(), texture->GetLayout());
    const ImTextureID id = reinterpret_cast<ImTextureID>(set);
    s_textures[id] = std::move(texture);
    return id;
  }

  void DestroyTexture(ImTextureID texture) override
  {
    const auto it = s_textures.find(texture);
    if (it == s_textures.end())
      return;
    ImGui_ImplVulkan_RemoveTexture(reinterpret_cast<VkDescriptorSet>(texture));
    s_textures.erase(it);
  }
};
Host s_host;

PFN_vkVoidFunction LoadVulkanFunction(const char* name, void* user_data)
{
  if (!::vkGetInstanceProcAddr)
    return nullptr;
  return ::vkGetInstanceProcAddr(static_cast<VkInstance>(user_data), name);
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

void DrawCallback(Vulkan::VKFramebuffer* fb, VkCommandBuffer cmd)
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

  const u32 width = fb->GetWidth();
  const u32 height = fb->GetHeight();
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

  if (!draw_data)
    return;

  VkRenderPassBeginInfo rp_info{};
  rp_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  rp_info.renderPass = fb->GetLoadRenderPass();
  rp_info.framebuffer = fb->GetFB();
  rp_info.renderArea.extent = {width, height};
  vkCmdBeginRenderPass(cmd, &rp_info, VK_SUBPASS_CONTENTS_INLINE);
  ImGui_ImplVulkan_RenderDrawData(draw_data, cmd);
  vkCmdEndRenderPass(cmd);
}
}  // namespace

bool Init()
{
  if (s_registered.load())
    return true;

  auto* gfx = Vulkan::VKGfx::GetInstance();
  if (!gfx || !gfx->GetSwapChain() || !Vulkan::g_vulkan_context)
    return false;
  auto* swap_chain = gfx->GetSwapChain();
  auto* framebuffer = swap_chain->GetCurrentFramebuffer();
  if (!framebuffer)
    return false;

  s_device = Vulkan::g_vulkan_context->GetDevice();
  s_render_pass = framebuffer->GetLoadRenderPass();
  s_image_count = static_cast<u32>(swap_chain->GetSwapChainImageCount());
  s_visible.store(false);
  s_pending_action.store(0);
  s_pending_nav.store(0);
  hidInitializeTouchScreen();

  Vulkan::VKGfx::SetOverlayCallback(&DrawCallback);
  s_registered.store(true);
  return true;
}

void Shutdown()
{
  if (!s_registered.load())
    return;

  if (s_device && ::vkDeviceWaitIdle)
    ::vkDeviceWaitIdle(s_device);
  Vulkan::VKGfx::SetOverlayCallback(nullptr);
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
}  // namespace DolphinNX::VulkanOverlay

// The renderer tico's overlay draws with: ImGui's Vulkan backend on the
// swapchain's load render pass, set up from the first frame drawn.
namespace DolphinNX::VulkanOverlay
{
bool RendererInit()
{
  if (s_device == VK_NULL_HANDLE || !Vulkan::g_vulkan_context)
    return false;

  VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 128};
  VkDescriptorPoolCreateInfo pool_info{};
  pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
  pool_info.maxSets = 128;
  pool_info.poolSizeCount = 1;
  pool_info.pPoolSizes = &pool_size;
  if (vkCreateDescriptorPool(s_device, &pool_info, nullptr, &s_descriptor_pool) != VK_SUCCESS)
    return false;

  VkSamplerCreateInfo sampler_info{};
  sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  sampler_info.magFilter = VK_FILTER_LINEAR;
  sampler_info.minFilter = VK_FILTER_LINEAR;
  sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
  sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler_info.maxLod = 1.0f;
  if (vkCreateSampler(s_device, &sampler_info, nullptr, &s_sampler) != VK_SUCCESS)
  {
    RendererShutdown();
    return false;
  }

  const VkInstance instance = Vulkan::g_vulkan_context->GetVulkanInstance();
  if (!ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_1, LoadVulkanFunction, instance))
  {
    RendererShutdown();
    return false;
  }

  ImGui_ImplVulkan_InitInfo init_info{};
  init_info.ApiVersion = VK_API_VERSION_1_1;
  init_info.Instance = instance;
  init_info.PhysicalDevice = Vulkan::g_vulkan_context->GetPhysicalDevice();
  init_info.Device = s_device;
  init_info.QueueFamily = Vulkan::g_vulkan_context->GetGraphicsQueueFamilyIndex();
  init_info.Queue = Vulkan::g_vulkan_context->GetGraphicsQueue();
  init_info.DescriptorPool = s_descriptor_pool;
  init_info.RenderPass = s_render_pass;
  init_info.MinImageCount = s_image_count >= 2 ? s_image_count : 2;
  init_info.ImageCount = s_image_count >= 2 ? s_image_count : 2;
  init_info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
  if (!ImGui_ImplVulkan_Init(&init_info))
  {
    RendererShutdown();
    return false;
  }
  return true;
}

void RendererShutdown()
{
  if (ImGui::GetCurrentContext() && ImGui::GetIO().BackendRendererUserData)
  {
    for (auto& [id, texture] : s_textures)
      ImGui_ImplVulkan_RemoveTexture(reinterpret_cast<VkDescriptorSet>(id));
    s_textures.clear();
    ImGui_ImplVulkan_Shutdown();
  }
  s_textures.clear();
  if (s_sampler != VK_NULL_HANDLE)
  {
    vkDestroySampler(s_device, s_sampler, nullptr);
    s_sampler = VK_NULL_HANDLE;
  }
  if (s_descriptor_pool != VK_NULL_HANDLE)
  {
    vkDestroyDescriptorPool(s_device, s_descriptor_pool, nullptr);
    s_descriptor_pool = VK_NULL_HANDLE;
  }
}

void RendererBeginFrame()
{
  ImGui_ImplVulkan_NewFrame();
}
}  // namespace DolphinNX::VulkanOverlay
