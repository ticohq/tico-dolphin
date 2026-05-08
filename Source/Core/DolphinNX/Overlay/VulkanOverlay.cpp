// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 Dan | ticoverse.com
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinNX/Overlay/VulkanOverlay.h"

#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <imgui.h>
#include <imgui_impl_vulkan.h>
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include "Common/CommonTypes.h"
#include "VideoBackends/Vulkan/CommandBufferManager.h"
#include "VideoBackends/Vulkan/VKGfx.h"
#include "VideoBackends/Vulkan/VKSwapChain.h"
#include "VideoBackends/Vulkan/VKTexture.h"
#include "VideoBackends/Vulkan/VulkanContext.h"
#include "VideoBackends/Vulkan/VulkanLoader.h"

#include "DolphinNX/Overlay/Overlay.h"

namespace DolphinNX::VulkanOverlay
{
namespace
{
constexpr const char* TAG = "[OverlayVK]";

std::atomic_bool s_initialized = false;
std::atomic_bool s_visible = false;
std::atomic_bool s_exit_requested = false;
std::atomic_int s_pending_action = 0;
std::atomic_uint s_pending_nav_mask = 0;
bool s_psm_initialized = false;
bool s_social_data_loaded = false;

VkDescriptorPool s_descriptor_pool = VK_NULL_HANDLE;
VkRenderPass s_render_pass = VK_NULL_HANDLE;
VkDevice s_device = VK_NULL_HANDLE;
VkSampler s_avatar_sampler = VK_NULL_HANDLE;
VkDescriptorSet s_avatar_descriptor_set = VK_NULL_HANDLE;
std::unique_ptr<Vulkan::VKTexture> s_avatar_texture;

bool s_was_combo_down = false;

struct NavPrev
{
  bool up;
  bool down;
  bool left;
  bool right;
  bool a;
  bool b;
};
NavPrev s_nav_prev{};

enum NavBits : unsigned int
{
  NavBit_Up = 1u << 0,
  NavBit_Down = 1u << 1,
  NavBit_Left = 1u << 2,
  NavBit_Right = 1u << 3,
  NavBit_Accept = 1u << 4,
  NavBit_Cancel = 1u << 5,
};

struct AvatarImage
{
  std::vector<unsigned char> pixels;
  int width = 0;
  int height = 0;

  bool IsValid() const { return !pixels.empty() && width > 0 && height > 0; }
};

constexpr std::array<const char*, 2> kCustomAvatarPaths = {{
    "sdmc:/tiicu/assets/avatar.jpg",
    "sdmc:/tico/assets/avatar.jpg",
}};

AvatarImage s_avatar_image;
std::string s_nickname = "Player 1";

FILE* s_log = nullptr;
void LOG(const char* fmt, ...)
{
  // if (!s_log)
  //   s_log = std::fopen("sdmc:/dolphin-nx-overlay.log", "w");
  if (!s_log)
    return;
  va_list args;
  va_start(args, fmt);
  std::vfprintf(s_log, fmt, args);
  va_end(args);
  std::fflush(s_log);
}

bool FileExists(const char* path)
{
  if (FILE* fp = std::fopen(path, "rb"))
  {
    std::fclose(fp);
    return true;
  }
  return false;
}

const char* GetCustomAvatarPath()
{
  for (const char* path : kCustomAvatarPaths)
  {
    if (FileExists(path))
      return path;
  }
  return nullptr;
}

AvatarImage DecodeAvatarFromFile(const char* path)
{
  AvatarImage avatar;
  int channels = 0;
  if (unsigned char* data = stbi_load(path, &avatar.width, &avatar.height, &channels, 4))
  {
    const size_t byte_count =
        static_cast<size_t>(avatar.width) * static_cast<size_t>(avatar.height) * 4;
    avatar.pixels.assign(data, data + byte_count);
    stbi_image_free(data);
  }
  return avatar;
}

AvatarImage DecodeAvatarFromMemory(const void* data, size_t size)
{
  AvatarImage avatar;
  int channels = 0;
  if (unsigned char* rgba = stbi_load_from_memory(static_cast<const stbi_uc*>(data),
                                                  static_cast<int>(size), &avatar.width,
                                                  &avatar.height, &channels, 4))
  {
    const size_t byte_count =
        static_cast<size_t>(avatar.width) * static_cast<size_t>(avatar.height) * 4;
    avatar.pixels.assign(rgba, rgba + byte_count);
    stbi_image_free(rgba);
  }
  return avatar;
}

AvatarImage LoadAvatarFromAccount()
{
  AvatarImage avatar;

  Result rc = accountInitialize(AccountServiceType_Application);
  if (R_FAILED(rc))
    return avatar;

  AccountUid uid = {};
  bool found = false;

  if (R_SUCCEEDED(accountGetPreselectedUser(&uid)) && accountUidIsValid(&uid))
    found = true;
  if (!found && R_SUCCEEDED(accountGetLastOpenedUser(&uid)) && accountUidIsValid(&uid))
    found = true;
  if (!found)
  {
    s32 user_count = 0;
    if (R_SUCCEEDED(accountGetUserCount(&user_count)) && user_count > 0)
    {
      AccountUid uids[ACC_USER_LIST_SIZE];
      s32 actual_total = 0;
      if (R_SUCCEEDED(accountListAllUsers(uids, ACC_USER_LIST_SIZE, &actual_total)) &&
          actual_total > 0)
      {
        uid = uids[0];
        found = true;
      }
    }
  }

  if (found)
  {
    AccountProfile profile;
    if (R_SUCCEEDED(accountGetProfile(&profile, uid)))
    {
      u32 image_size = 0;
      if (R_SUCCEEDED(accountProfileGetImageSize(&profile, &image_size)) && image_size > 0)
      {
        std::vector<unsigned char> jpeg_buffer(image_size);
        u32 actual_size = 0;
        if (R_SUCCEEDED(accountProfileLoadImage(&profile, jpeg_buffer.data(), image_size,
                                                &actual_size)))
        {
          avatar = DecodeAvatarFromMemory(jpeg_buffer.data(), actual_size);
        }
      }
      accountProfileClose(&profile);
    }
  }

  accountExit();
  return avatar;
}

std::string LoadNickname()
{
  if (GetCustomAvatarPath() != nullptr)
    return "Player 1";

  Result rc = accountInitialize(AccountServiceType_Application);
  if (R_FAILED(rc))
    return "Player 1";

  std::string nickname;
  AccountUid uid = {};
  bool found = false;

  if (R_SUCCEEDED(accountGetPreselectedUser(&uid)) && accountUidIsValid(&uid))
    found = true;
  if (!found && R_SUCCEEDED(accountGetLastOpenedUser(&uid)) && accountUidIsValid(&uid))
    found = true;
  if (!found)
  {
    s32 user_count = 0;
    if (R_SUCCEEDED(accountGetUserCount(&user_count)) && user_count > 0)
    {
      AccountUid uids[ACC_USER_LIST_SIZE];
      s32 actual_total = 0;
      if (R_SUCCEEDED(accountListAllUsers(uids, ACC_USER_LIST_SIZE, &actual_total)) &&
          actual_total > 0)
      {
        uid = uids[0];
        found = true;
      }
    }
  }

  if (found)
  {
    AccountProfile profile;
    AccountProfileBase profile_base;
    if (R_SUCCEEDED(accountGetProfile(&profile, uid)))
    {
      if (R_SUCCEEDED(accountProfileGet(&profile, nullptr, &profile_base)))
        nickname = std::string(profile_base.nickname);
      accountProfileClose(&profile);
    }
  }

  accountExit();
  return nickname.empty() ? "Player 1" : nickname;
}

void LoadSocialAreaData()
{
  s_social_data_loaded = true;
  s_nickname = LoadNickname();
  OverlayUI::SetNickname(s_nickname);
  OverlayUI::SetAvatarTextureId(0);

  if (const char* custom_avatar = GetCustomAvatarPath())
  {
    s_avatar_image = DecodeAvatarFromFile(custom_avatar);
    if (s_avatar_image.IsValid())
    {
      LOG("%s loaded custom avatar from %s (%dx%d)\n", TAG, custom_avatar, s_avatar_image.width,
          s_avatar_image.height);
      return;
    }
    LOG("%s failed to decode custom avatar at %s\n", TAG, custom_avatar);
  }

  s_avatar_image = LoadAvatarFromAccount();
  if (s_avatar_image.IsValid())
  {
    LOG("%s loaded account avatar (%dx%d)\n", TAG, s_avatar_image.width, s_avatar_image.height);
  }
  else
  {
    LOG("%s no avatar available, using placeholder social area\n", TAG);
  }
}

bool CreateAvatarSampler()
{
  if (s_avatar_sampler != VK_NULL_HANDLE)
    return true;

  VkSamplerCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  info.magFilter = VK_FILTER_LINEAR;
  info.minFilter = VK_FILTER_LINEAR;
  info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
  info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  info.maxLod = 1.0f;

  const VkResult rc = vkCreateSampler(s_device, &info, nullptr, &s_avatar_sampler);
  if (rc != VK_SUCCESS)
  {
    LOG("%s vkCreateSampler for avatar failed: %d\n", TAG, static_cast<int>(rc));
    return false;
  }
  return true;
}

void UploadAvatarTextureIfNeeded()
{
  if (s_avatar_descriptor_set != VK_NULL_HANDLE || !s_avatar_image.IsValid())
    return;

  if (!CreateAvatarSampler())
    return;

  const TextureConfig config(static_cast<u32>(s_avatar_image.width),
                             static_cast<u32>(s_avatar_image.height), 1, 1, 1,
                             AbstractTextureFormat::RGBA8, 0, AbstractTextureType::Texture_2D);
  auto texture = Vulkan::VKTexture::Create(config, "DolphinNXOverlayAvatar");
  if (!texture)
  {
    LOG("%s failed to create Vulkan texture for avatar\n", TAG);
    s_avatar_image = {};
    return;
  }

  texture->Load(0, static_cast<u32>(s_avatar_image.width), static_cast<u32>(s_avatar_image.height),
                static_cast<u32>(s_avatar_image.width), s_avatar_image.pixels.data(),
                s_avatar_image.pixels.size(), 0);

  s_avatar_descriptor_set =
      ImGui_ImplVulkan_AddTexture(s_avatar_sampler, texture->GetView(), texture->GetLayout());
  s_avatar_texture = std::move(texture);
  OverlayUI::SetAvatarTextureId(reinterpret_cast<unsigned long long>(s_avatar_descriptor_set));
  LOG("%s uploaded social avatar to Vulkan (%dx%d)\n", TAG, s_avatar_image.width,
      s_avatar_image.height);
  s_avatar_image = {};
}

void DestroyAvatarResources()
{
  OverlayUI::SetAvatarTextureId(0);
  if (s_avatar_descriptor_set != VK_NULL_HANDLE)
  {
    ImGui_ImplVulkan_RemoveTexture(s_avatar_descriptor_set);
    s_avatar_descriptor_set = VK_NULL_HANDLE;
  }
  s_avatar_texture.reset();
  if (s_avatar_sampler != VK_NULL_HANDLE)
  {
    vkDestroySampler(s_device, s_avatar_sampler, nullptr);
    s_avatar_sampler = VK_NULL_HANDLE;
  }
  s_avatar_image = {};
}

PFN_vkVoidFunction VulkanLoaderCallback(const char* name, void* user_data)
{
  VkInstance instance = static_cast<VkInstance>(user_data);
  if (!::vkGetInstanceProcAddr)
    return nullptr;
  return ::vkGetInstanceProcAddr(instance, name);
}

void DrawCallback(Vulkan::VKFramebuffer* fb, VkCommandBuffer cmd)
{
  if (!s_initialized.load() || (!s_visible.load() && !OverlayUI::HasTransientContent()))
    return;

  const u32 fb_w = fb->GetWidth();
  const u32 fb_h = fb->GetHeight();

  ImGui_ImplVulkan_NewFrame();

  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2(static_cast<float>(fb_w), static_cast<float>(fb_h));
  io.DeltaTime = 1.0f / 60.0f;

  UploadAvatarTextureIfNeeded();

  const unsigned int nav_mask = s_pending_nav_mask.exchange(0);
  if (nav_mask != 0)
  {
    LOG("%s nav delivered to render (mask=0x%x)\n", TAG, nav_mask);
  }
  OverlayUI::FeedNav({
      .up = (nav_mask & NavBit_Up) != 0,
      .down = (nav_mask & NavBit_Down) != 0,
      .left = (nav_mask & NavBit_Left) != 0,
      .right = (nav_mask & NavBit_Right) != 0,
      .accept = (nav_mask & NavBit_Accept) != 0,
      .cancel = (nav_mask & NavBit_Cancel) != 0,
  });

  ImGui::NewFrame();
  const OverlayUI::Action action =
      OverlayUI::Render(static_cast<int>(fb_w), static_cast<int>(fb_h));
  ImGui::Render();

  if (action != OverlayUI::Action::None)
  {
    s_pending_action.store(static_cast<int>(action));
    if (action == OverlayUI::Action::Exit)
      s_exit_requested.store(true);
    if (action == OverlayUI::Action::Resume || action == OverlayUI::Action::Exit)
    {
      s_visible.store(false);
      s_pending_nav_mask.store(0);
      OverlayUI::SetVisible(false);
    }
  }

  VkRenderPassBeginInfo rp_info{};
  rp_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  rp_info.renderPass = fb->GetLoadRenderPass();
  rp_info.framebuffer = fb->GetFB();
  rp_info.renderArea.offset = {0, 0};
  rp_info.renderArea.extent = {fb_w, fb_h};
  rp_info.clearValueCount = 0;
  rp_info.pClearValues = nullptr;

  vkCmdBeginRenderPass(cmd, &rp_info, VK_SUBPASS_CONTENTS_INLINE);
  ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
  vkCmdEndRenderPass(cmd);
}

bool CreateDescriptorPool(VkDevice device)
{
  VkDescriptorPoolSize pool_size{};
  pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  pool_size.descriptorCount = 64;

  VkDescriptorPoolCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
  info.maxSets = 64;
  info.poolSizeCount = 1;
  info.pPoolSizes = &pool_size;

  const VkResult rc = vkCreateDescriptorPool(device, &info, nullptr, &s_descriptor_pool);
  if (rc != VK_SUCCESS)
  {
    LOG("%s vkCreateDescriptorPool failed: %d\n", TAG, static_cast<int>(rc));
    return false;
  }
  return true;
}
}  // namespace

bool Init()
{
  if (s_initialized.load())
    return true;

  LOG("%s Init begin\n", TAG);

  auto* gfx = Vulkan::VKGfx::GetInstance();
  if (!gfx || !gfx->GetSwapChain() || !Vulkan::g_vulkan_context)
  {
    LOG("%s Vulkan backend not ready\n", TAG);
    return false;
  }

  auto* swap_chain = gfx->GetSwapChain();
  auto* fb0 = swap_chain->GetCurrentFramebuffer();
  if (!fb0)
  {
    LOG("%s swapchain has no current framebuffer yet\n", TAG);
    return false;
  }

  s_device = Vulkan::g_vulkan_context->GetDevice();
  s_render_pass = fb0->GetLoadRenderPass();

  s_visible.store(false);
  s_exit_requested.store(false);
  s_pending_action.store(0);
  s_pending_nav_mask.store(0);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.LogFilename = nullptr;
  io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

  if (ImFont* font = io.Fonts->AddFontFromFileTTF("romfs:/fonts/font.ttf", 32.0f))
  {
    io.FontDefault = font;
    LOG("%s loaded tico font from romfs:/fonts/font.ttf\n", TAG);
  }
  else
  {
    LOG("%s failed to load romfs:/fonts/font.ttf, using default ImGui font\n", TAG);
  }

  ImGui::StyleColorsDark();

  if (!CreateDescriptorPool(s_device))
    return false;

  if (!s_psm_initialized && R_SUCCEEDED(psmInitialize()))
    s_psm_initialized = true;
  OverlayUI::SetNickname(std::string{});

  VkInstance instance = Vulkan::g_vulkan_context->GetVulkanInstance();
  if (!ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_1, VulkanLoaderCallback, instance))
  {
    LOG("%s ImGui_ImplVulkan_LoadFunctions failed\n", TAG);
    return false;
  }

  const u32 image_count = static_cast<u32>(swap_chain->GetSwapChainImageCount());

  ImGui_ImplVulkan_InitInfo init_info{};
  init_info.ApiVersion = VK_API_VERSION_1_1;
  init_info.Instance = instance;
  init_info.PhysicalDevice = Vulkan::g_vulkan_context->GetPhysicalDevice();
  init_info.Device = s_device;
  init_info.QueueFamily = Vulkan::g_vulkan_context->GetGraphicsQueueFamilyIndex();
  init_info.Queue = Vulkan::g_vulkan_context->GetGraphicsQueue();
  init_info.DescriptorPool = s_descriptor_pool;
  init_info.RenderPass = s_render_pass;
  init_info.MinImageCount = image_count >= 2 ? image_count : 2;
  init_info.ImageCount = image_count;
  init_info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
  init_info.PipelineCache = VK_NULL_HANDLE;
  init_info.Subpass = 0;

  if (!ImGui_ImplVulkan_Init(&init_info))
  {
    LOG("%s ImGui_ImplVulkan_Init failed\n", TAG);
    return false;
  }

  Vulkan::VKGfx::SetOverlayCallback(&DrawCallback);

  s_initialized.store(true);
  LOG("%s Init complete (swap_images=%u, rp=%p)\n", TAG, image_count, (void*)s_render_pass);
  return true;
}

void Update(PadState* pad)
{
  if (!s_initialized.load() || !pad)
    return;

  const u64 held = padGetButtons(pad);
  const bool plus = (held & HidNpadButton_Plus) != 0;
  const bool minus = (held & HidNpadButton_Minus) != 0;
  const bool combo_down = plus && minus;

  if (combo_down && !s_was_combo_down)
  {
    const bool new_visible = !s_visible.load();
    s_visible.store(new_visible);
    if (!new_visible)
      s_pending_nav_mask.store(0);
    else if (!s_social_data_loaded)
      LoadSocialAreaData();
    LOG("%s toggle combo detected (held=0x%llx, visible=%d)\n", TAG,
        static_cast<unsigned long long>(held), new_visible);
  }
  s_was_combo_down = combo_down;
  const bool visible = s_visible.load();
  OverlayUI::SetVisible(visible);

  const bool up = (held & (HidNpadButton_Up | HidNpadButton_StickLUp)) != 0;
  const bool down = (held & (HidNpadButton_Down | HidNpadButton_StickLDown)) != 0;
  const bool left = (held & (HidNpadButton_Left | HidNpadButton_StickLLeft)) != 0;
  const bool right = (held & (HidNpadButton_Right | HidNpadButton_StickLRight)) != 0;
  const bool a = (held & HidNpadButton_A) != 0;
  const bool b = (held & HidNpadButton_B) != 0;

  OverlayUI::NavInput nav{
      .up = up && !s_nav_prev.up,
      .down = down && !s_nav_prev.down,
      .left = left && !s_nav_prev.left,
      .right = right && !s_nav_prev.right,
      .accept = a && !s_nav_prev.a,
      .cancel = b && !s_nav_prev.b,
  };
  unsigned int nav_mask = 0;
  if (nav.up)
    nav_mask |= NavBit_Up;
  if (nav.down)
    nav_mask |= NavBit_Down;
  if (nav.left)
    nav_mask |= NavBit_Left;
  if (nav.right)
    nav_mask |= NavBit_Right;
  if (nav.accept)
    nav_mask |= NavBit_Accept;
  if (nav.cancel)
    nav_mask |= NavBit_Cancel;
  if (visible && (nav.up || nav.down || nav.left || nav.right || nav.accept || nav.cancel))
  {
    LOG("%s nav edge (up=%d down=%d left=%d right=%d accept=%d cancel=%d)\n", TAG, nav.up,
        nav.down, nav.left, nav.right, nav.accept, nav.cancel);
    s_pending_nav_mask.fetch_or(nav_mask);
  }

  s_nav_prev = {up, down, left, right, a, b};
}

bool IsVisible()
{
  return s_visible.load();
}

bool ShouldExit()
{
  return s_exit_requested.load();
}

int ConsumeAction()
{
  return s_pending_action.exchange(0);
}

void Shutdown()
{
  if (!s_initialized.load())
    return;

  if (s_device && ::vkDeviceWaitIdle)
    ::vkDeviceWaitIdle(s_device);

  Vulkan::VKGfx::SetOverlayCallback(nullptr);
  DestroyAvatarResources();
  ImGui_ImplVulkan_Shutdown();
  ImGui::DestroyContext();

  if (s_descriptor_pool != VK_NULL_HANDLE)
  {
    ::vkDestroyDescriptorPool(s_device, s_descriptor_pool, nullptr);
    s_descriptor_pool = VK_NULL_HANDLE;
  }

  OverlayUI::SetVisible(false);
  OverlayUI::SetNickname(std::string{});
  OverlayUI::ShowToast(std::string{});
  s_render_pass = VK_NULL_HANDLE;
  s_device = VK_NULL_HANDLE;
  s_visible.store(false);
  s_exit_requested.store(false);
  s_pending_action.store(0);
  s_pending_nav_mask.store(0);
  s_initialized.store(false);
  s_social_data_loaded = false;

  if (s_psm_initialized)
  {
    psmExit();
    s_psm_initialized = false;
  }

  if (s_log)
  {
    std::fclose(s_log);
    s_log = nullptr;
  }
}

}  // namespace DolphinNX::VulkanOverlay
