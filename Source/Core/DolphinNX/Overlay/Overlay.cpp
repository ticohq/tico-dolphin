// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 Dan | ticoverse.com
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinNX/Overlay/Overlay.h"
#include "DolphinNX/Overlay/TranslationManager.h"
#include "DolphinNX/TicoCore.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <string_view>

#include <imgui.h>
#include <switch.h>

#include "Core/State.h"

namespace DolphinNX::OverlayUI
{

namespace
{
struct QuickMenuItem
{
  const char* key;
  const char* fallback;
};

constexpr std::array<QuickMenuItem, 4> kQuickMenuItems = {{
    {"emulator_save_state", "Save State"},
    {"emulator_load_state", "Load State"},
    {"emulator_settings", "Settings"},
    {"emulator_exit_game", "Exit Game"},
}};
constexpr int kOverlaySlotCount = 4;
constexpr int kSettingsItemCount = 2;
constexpr int kToastSlotCount = 4;

constexpr float kAnimDuration = 0.4f;
constexpr float kToastDuration = 2.0f;
constexpr float kToastFadeDuration = 0.5f;

enum class MenuScreen
{
  QuickMenu,
  SaveStates,
  LoadStates,
  Settings,
};

enum class ViewportMode
{
  Integer,
  Display,
};

enum class DisplaySize
{
  Stretch,
  FourThree,
  SixteenNine,
  Original,
};

enum class IntegerScale
{
  OneX,
  TwoX,
  Auto,
};

int s_selected = 0;
int s_slot_selected = 0;
int s_settings_selected = 0;
std::string s_title;
std::string s_nickname;
NavInput s_nav{};
bool s_visible = false;
float s_anim_timer = 0.0f;
MenuScreen s_menu = MenuScreen::QuickMenu;
std::array<std::string, kOverlaySlotCount> s_slot_labels{};
unsigned long long s_avatar_texture_id = 0;
ViewportMode s_viewport_mode = ViewportMode::Display;
DisplaySize s_display_size = DisplaySize::FourThree;
IntegerScale s_integer_scale = IntegerScale::Auto;
std::mutex s_toast_mutex;
std::array<std::string, kToastSlotCount> s_toast_messages{};
std::array<float, kToastSlotCount> s_toast_timers{};

float EaseOutCubic(float t)
{
  t = std::clamp(t, 0.0f, 1.0f);
  return 1.0f - std::pow(1.0f - t, 3.0f);
}

std::string TrOr(const char* key, const char* fallback)
{
  const std::string translated = OverlayTranslation::tr(key);
  return translated == key ? std::string(fallback) : translated;
}

void DrawTextWithShadow(ImDrawList* dl, ImFont* font, float font_size, ImVec2 pos, ImU32 color,
                        std::string_view text)
{
  const ImU32 shadow_color = IM_COL32(0, 0, 0, 80);
  dl->AddText(font, font_size, ImVec2(pos.x + 1.5f, pos.y + 1.5f), shadow_color, text.data(),
              text.data() + text.size());
  dl->AddText(font, font_size, pos, color, text.data(), text.data() + text.size());
}

void DrawSwitchButton(ImDrawList* dl, ImFont* font, float font_size, ImVec2 center, float size,
                      std::string_view symbol, float alpha)
{
  const ImU32 fill_color = IM_COL32(220, 220, 220, static_cast<int>(255.0f * alpha));
  const ImU32 text_color = IM_COL32(40, 40, 40, static_cast<int>(255.0f * alpha));
  dl->AddCircleFilled(center, size * 0.5f, fill_color, 16);

  const float symbol_size = font_size * 0.75f;
  const ImVec2 text_size =
      font->CalcTextSizeA(symbol_size, FLT_MAX, 0.0f, symbol.data(), symbol.data() + symbol.size());
  const ImVec2 text_pos(center.x - (text_size.x * 0.5f), center.y - (text_size.y * 0.5f));
  dl->AddText(font, symbol_size, text_pos, text_color, symbol.data(), symbol.data() + symbol.size());
}

std::string BuildTitle()
{
  std::string title;
  switch (s_menu)
  {
  case MenuScreen::SaveStates:
    title = TrOr("emulator_save_state", "Save State");
    break;
  case MenuScreen::LoadStates:
    title = TrOr("emulator_load_state", "Load State");
    break;
  case MenuScreen::Settings:
    title = TrOr("emulator_settings", "Settings");
    break;
  case MenuScreen::QuickMenu:
  default:
    title = s_title.empty() ? "Dolphin" : s_title;
    break;
  }

  constexpr size_t kMaxTitleLength = 46;
  if (title.size() > kMaxTitleLength)
    title = title.substr(0, kMaxTitleLength - 3) + "...";
  return title;
}

Action MakeSaveActionForSlot(int slot_index)
{
  return static_cast<Action>(static_cast<int>(Action::SaveStateSlot1) + slot_index);
}

Action MakeLoadActionForSlot(int slot_index)
{
  return static_cast<Action>(static_cast<int>(Action::LoadStateSlot1) + slot_index);
}

ViewportMode ParseViewportMode(std::string_view value)
{
  return value == "Integer" ? ViewportMode::Integer : ViewportMode::Display;
}

DisplaySize ParseDisplaySize(std::string_view value)
{
  if (value == "Stretch")
    return DisplaySize::Stretch;
  if (value == "16:9")
    return DisplaySize::SixteenNine;
  if (value == "Original")
    return DisplaySize::Original;
  return DisplaySize::FourThree;
}

IntegerScale ParseIntegerScale(std::string_view value)
{
  if (value == "1x")
    return IntegerScale::OneX;
  if (value == "2x")
    return IntegerScale::TwoX;
  return IntegerScale::Auto;
}

const char* ToConfigValue(ViewportMode mode)
{
  return mode == ViewportMode::Integer ? "Integer" : "Display";
}

const char* ToConfigValue(DisplaySize size)
{
  switch (size)
  {
  case DisplaySize::Stretch:
    return "Stretch";
  case DisplaySize::SixteenNine:
    return "16:9";
  case DisplaySize::Original:
    return "Original";
  case DisplaySize::FourThree:
  default:
    return "4:3";
  }
}

const char* ToConfigValue(IntegerScale scale)
{
  switch (scale)
  {
  case IntegerScale::OneX:
    return "1x";
  case IntegerScale::TwoX:
    return "2x";
  case IntegerScale::Auto:
  default:
    return "Auto";
  }
}

void LoadViewportSettings()
{
  s_viewport_mode =
      ParseViewportMode(TicoCore::GetConfigValue("display_mode", ToConfigValue(ViewportMode::Display)));
  s_display_size =
      ParseDisplaySize(TicoCore::GetConfigValue("display_size", ToConfigValue(DisplaySize::FourThree)));
  s_integer_scale = ParseIntegerScale(
      TicoCore::GetConfigValue("integer_scale", ToConfigValue(IntegerScale::Auto)));
}

void SaveViewportSettings()
{
  TicoCore::SetConfigValue("display_mode", ToConfigValue(s_viewport_mode));
  TicoCore::SetConfigValue("display_size", ToConfigValue(s_display_size));
  TicoCore::SetConfigValue("integer_scale", ToConfigValue(s_integer_scale));
  TicoCore::SaveConfig();
}

std::string GetViewportValueLabel()
{
  if (s_viewport_mode == ViewportMode::Integer)
  {
    switch (s_integer_scale)
    {
    case IntegerScale::OneX:
      return "1x";
    case IntegerScale::TwoX:
      return "2x";
    case IntegerScale::Auto:
    default:
      return TrOr("emulator_auto", "Auto");
    }
  }

  switch (s_display_size)
  {
  case DisplaySize::Stretch:
    return TrOr("emulator_stretch", "Stretch");
  case DisplaySize::SixteenNine:
    return "16:9";
  case DisplaySize::Original:
    return TrOr("emulator_original", "Original");
  case DisplaySize::FourThree:
  default:
    return "4:3";
  }
}

void AdvanceSettingsValue(int direction)
{
  if (direction == 0)
    return;

  if (s_settings_selected == 0)
  {
    s_viewport_mode = s_viewport_mode == ViewportMode::Display ? ViewportMode::Integer :
                                                                ViewportMode::Display;
    if (s_viewport_mode == ViewportMode::Integer)
      s_integer_scale = IntegerScale::Auto;
    else
      s_display_size = DisplaySize::FourThree;
  }
  else if (s_viewport_mode == ViewportMode::Integer)
  {
    int scale = static_cast<int>(s_integer_scale) + direction;
    if (scale < static_cast<int>(IntegerScale::OneX))
      scale = static_cast<int>(IntegerScale::Auto);
    else if (scale > static_cast<int>(IntegerScale::Auto))
      scale = static_cast<int>(IntegerScale::OneX);
    s_integer_scale = static_cast<IntegerScale>(scale);
  }
  else
  {
    int size = static_cast<int>(s_display_size) + direction;
    if (size < static_cast<int>(DisplaySize::Stretch))
      size = static_cast<int>(DisplaySize::Original);
    else if (size > static_cast<int>(DisplaySize::Original))
      size = static_cast<int>(DisplaySize::Stretch);
    s_display_size = static_cast<DisplaySize>(size);
  }
  
  SaveViewportSettings();
}

void RefreshSlotLabels()
{
  const std::string slot_format = TrOr("emulator_slot", "Slot %d (%s)");
  const std::string in_use = TrOr("emulator_in_use", "In Use");
  const std::string empty = TrOr("emulator_empty", "Empty");

  for (int i = 0; i < kOverlaySlotCount; ++i)
  {
    const bool exists = State::GetUnixTimeOfSlot(i + 1) != 0;
    char label[128];
    std::snprintf(label, sizeof(label), slot_format.c_str(), i + 1,
                  exists ? in_use.c_str() : empty.c_str());
    s_slot_labels[i] = label;
  }
}

void RenderOverlayBackground(ImDrawList* dl, ImVec2 display_size, float ease)
{
  const int base_alpha = static_cast<int>(200.0f * ease);
  const int max_alpha = static_cast<int>(250.0f * ease);
  if (base_alpha <= 0)
    return;

  const float top_h = display_size.y * 0.20f;
  const float bottom_h = display_size.y * 0.20f;
  const float center_h = display_size.y - top_h - bottom_h;
  const ImU32 col_max = IM_COL32(0, 0, 0, max_alpha);
  const ImU32 col_base = IM_COL32(0, 0, 0, base_alpha);

  dl->AddRectFilledMultiColor(ImVec2(0.0f, 0.0f), ImVec2(display_size.x, top_h), col_max, col_max,
                              col_base, col_base);
  dl->AddRectFilled(ImVec2(0.0f, top_h), ImVec2(display_size.x, top_h + center_h), col_base);
  dl->AddRectFilledMultiColor(ImVec2(0.0f, display_size.y - bottom_h), display_size, col_base,
                              col_base, col_max, col_max);
}

void RenderTitleCard(ImDrawList* dl, ImVec2 display_size, float ease)
{
  const std::string title = BuildTitle();
  const float scale = ImGui::GetIO().FontGlobalScale;
  const float title_height = 72.0f * scale;
  const float available_top_space = 110.0f * scale;
  const float card_width = display_size.x * 0.4f;
  const float card_x = (display_size.x - card_width) * 0.5f;
  const float card_y = (available_top_space - title_height) * 0.5f;
  const float start_y = -150.0f * scale;
  const float current_y = start_y + ((card_y - start_y) * ease);
  const ImU32 text_color = IM_COL32(200, 200, 200, static_cast<int>(255.0f * ease));
  const ImVec2 text_size = ImGui::CalcTextSize(title.c_str());
  const float text_x = card_x + ((card_width - text_size.x) * 0.5f);
  const float text_y = current_y + ((title_height - text_size.y) * 0.5f);

  DrawTextWithShadow(dl, ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(text_x, text_y), text_color,
                     title);
}

void RenderMenu(ImDrawList* dl, ImVec2 display_size, float ease)
{
  if (s_menu == MenuScreen::Settings)
  {
    const float scale = ImGui::GetIO().FontGlobalScale;
    const float menu_width = 400.0f * scale;
    const float item_height = 64.0f * scale;
    const float content_height = static_cast<float>(kSettingsItemCount) * item_height;
    const ImVec2 menu_size(menu_width, content_height);
    const float target_y = (display_size.y - menu_size.y) * 0.5f;
    const float start_y = display_size.y + (100.0f * scale);
    const float current_y = start_y + ((target_y - start_y) * ease);
    const ImVec2 menu_pos((display_size.x - menu_size.x) * 0.5f, current_y);
    const ImVec2 p0 = menu_pos;
    const ImVec2 p1(menu_pos.x + menu_size.x, menu_pos.y + menu_size.y);
    const float corner_radius = 16.0f * scale;

    dl->AddRectFilled(p0, p1, IM_COL32(45, 45, 45, static_cast<int>(255.0f * ease)),
                      corner_radius);

    ImFont* font = ImGui::GetFont();
    const float label_size = ImGui::GetFontSize() * 0.85f;

    for (int i = 0; i < kSettingsItemCount; ++i)
    {
      const bool selected = s_settings_selected == i;
      const float item_y = menu_pos.y + (static_cast<float>(i) * item_height);
      const ImVec2 item_min(menu_pos.x, item_y);
      const ImVec2 item_max(menu_pos.x + menu_size.x, item_y + item_height);

      if (selected)
      {
        ImDrawFlags corners = ImDrawFlags_None;
        float item_radius = 0.0f;
        if (i == 0)
        {
          corners = ImDrawFlags_RoundCornersTop;
          item_radius = corner_radius;
        }
        else if (i == kSettingsItemCount - 1)
        {
          corners = ImDrawFlags_RoundCornersBottom;
          item_radius = corner_radius;
        }

        dl->AddRectFilled(item_min, item_max,
                          IM_COL32(60, 60, 60, static_cast<int>(255.0f * ease)), item_radius,
                          corners);
      }

      const std::string label =
          i == 0 ? TrOr("emulator_display_mode", "Display Mode") : TrOr("emulator_size", "Size");
      const std::string value =
          i == 0 ? (s_viewport_mode == ViewportMode::Integer ? TrOr("emulator_integer", "Integer") :
                                                            TrOr("emulator_display", "Display")) :
                   GetViewportValueLabel();
      const ImU32 text_color =
          selected ? IM_COL32(255, 255, 255, static_cast<int>(255.0f * ease)) :
                     IM_COL32(200, 200, 200, static_cast<int>(255.0f * ease));

      const ImVec2 label_text_size =
          font->CalcTextSizeA(label_size, FLT_MAX, 0.0f, label.c_str());
      const float text_x = item_min.x + (20.0f * scale);
      const float text_y = item_min.y + ((item_height - label_text_size.y) * 0.5f);
      dl->AddText(font, label_size, ImVec2(text_x, text_y), text_color, label.c_str());

      const ImVec2 value_text_size =
          font->CalcTextSizeA(label_size, FLT_MAX, 0.0f, value.c_str());
      const float value_x = item_max.x - value_text_size.x - (40.0f * scale);
      dl->AddText(font, label_size, ImVec2(value_x, text_y), text_color, value.c_str());

      if (selected)
      {
        const float arrow_size = 12.0f * scale;
        const float arrow_y = item_min.y + ((item_height - arrow_size) * 0.5f);

        const float left_arrow_x = value_x - arrow_size - (12.0f * scale);
        const ImVec2 lp1(left_arrow_x, arrow_y + (arrow_size * 0.5f));
        const ImVec2 lp2(left_arrow_x + arrow_size, arrow_y);
        const ImVec2 lp3(left_arrow_x + arrow_size, arrow_y + arrow_size);
        dl->AddTriangleFilled(lp1, lp2, lp3, text_color);

        const float right_arrow_x = value_x + value_text_size.x + (12.0f * scale);
        const ImVec2 rp1(right_arrow_x + arrow_size, arrow_y + (arrow_size * 0.5f));
        const ImVec2 rp2(right_arrow_x, arrow_y);
        const ImVec2 rp3(right_arrow_x, arrow_y + arrow_size);
        dl->AddTriangleFilled(rp1, rp2, rp3, text_color);
      }
    }

    return;
  }

  const bool showing_slots =
      s_menu == MenuScreen::SaveStates || s_menu == MenuScreen::LoadStates;
  const float scale = ImGui::GetIO().FontGlobalScale;
  const float menu_width = 400.0f * scale;
  const float item_height = 64.0f * scale;
  const int item_count =
      showing_slots ? kOverlaySlotCount : static_cast<int>(kQuickMenuItems.size());
  const float content_height = static_cast<float>(item_count) * item_height;
  const ImVec2 menu_size(menu_width, content_height);
  const float target_y = (display_size.y - menu_size.y) * 0.5f;
  const float start_y = display_size.y + (100.0f * scale);
  const float current_y = start_y + ((target_y - start_y) * ease);
  const ImVec2 menu_pos((display_size.x - menu_size.x) * 0.5f, current_y);
  const ImVec2 p0 = menu_pos;
  const ImVec2 p1(menu_pos.x + menu_size.x, menu_pos.y + menu_size.y);
  const float corner_radius = 16.0f * scale;

  dl->AddRectFilled(p0, p1, IM_COL32(45, 45, 45, static_cast<int>(255.0f * ease)), corner_radius);

  ImFont* font = ImGui::GetFont();
  const float label_size = ImGui::GetFontSize() * 0.85f;

  for (int i = 0; i < item_count; ++i)
  {
    const bool selected = showing_slots ? (s_slot_selected == i) : (s_selected == i);
    const float item_y = menu_pos.y + (static_cast<float>(i) * item_height);
    const ImVec2 item_min(menu_pos.x, item_y);
    const ImVec2 item_max(menu_pos.x + menu_size.x, item_y + item_height);

    if (selected)
    {
      ImDrawFlags corners = ImDrawFlags_None;
      float item_radius = 0.0f;
      if (i == 0)
      {
        corners = ImDrawFlags_RoundCornersTop;
        item_radius = corner_radius;
      }
      else if (i == item_count - 1)
      {
        corners = ImDrawFlags_RoundCornersBottom;
        item_radius = corner_radius;
      }

      dl->AddRectFilled(item_min, item_max,
                        IM_COL32(60, 60, 60, static_cast<int>(255.0f * ease)), item_radius,
                        corners);
    }

    std::string translated_label;
    const std::string_view label = showing_slots ?
                                       std::string_view(s_slot_labels[i]) :
                                       std::string_view(translated_label =
                                                            TrOr(kQuickMenuItems[i].key,
                                                                 kQuickMenuItems[i].fallback));
    const ImVec2 text_size =
        font->CalcTextSizeA(label_size, FLT_MAX, 0.0f, label.data(), label.data() + label.size());
    const float text_x = item_min.x + (20.0f * scale);
    const float text_y = item_min.y + ((item_height - text_size.y) * 0.5f);
    const ImU32 text_color = selected ? IM_COL32(255, 255, 255, static_cast<int>(255.0f * ease))
                                      : IM_COL32(200, 200, 200, static_cast<int>(255.0f * ease));
    dl->AddText(font, label_size, ImVec2(text_x, text_y), text_color, label.data(),
                label.data() + label.size());
  }
}

void RenderHelpersBar(ImDrawList* dl, ImVec2 display_size, float ease)
{
  struct Helper
  {
    const char* button;
    std::string_view description;
  };

  const std::string back = TrOr("emulator_back", "Back");
  std::string accept = TrOr("emulator_select", "Select");
  if (s_menu == MenuScreen::SaveStates)
    accept = TrOr("emulator_save_state", "Save State");
  else if (s_menu == MenuScreen::LoadStates)
    accept = TrOr("emulator_load_state", "Load State");
  else if (s_menu == MenuScreen::Settings)
    accept = TrOr("emulator_change", "Change");

  const std::array<Helper, 2> helpers = {{
      {"B", back},
      {"A", accept},
  }};

  const float scale = ImGui::GetIO().FontGlobalScale;
  const float bar_height = 48.0f * scale;
  const float margin_bottom = 24.0f * scale;
  const float padding = 16.0f * scale;
  const float button_size = 22.0f * scale;
  const float item_spacing = 12.0f * scale;
  ImFont* font = ImGui::GetFont();
  const float font_size = ImGui::GetFontSize() * 0.78f;

  float total_width = padding * 2.0f;
  for (size_t i = 0; i < helpers.size(); ++i)
  {
    const char* const text_begin = helpers[i].description.data();
    const char* const text_end = text_begin + helpers[i].description.size();
    total_width += button_size + (8.0f * scale) +
                   font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, text_begin, text_end).x;
    if (i + 1 < helpers.size())
      total_width += item_spacing;
  }

  const float current_offset = (400.0f * scale) * (1.0f - ease);
  const float bar_x = display_size.x - total_width - 20.0f + current_offset;
  const float bar_y = display_size.y - margin_bottom - bar_height;
  float cursor_x = bar_x + padding;
  const float center_y = bar_y + (bar_height * 0.5f);
  const ImU32 text_color = IM_COL32(200, 200, 200, static_cast<int>(255.0f * ease));

  for (const Helper& helper : helpers)
  {
    DrawSwitchButton(dl, font, font_size, ImVec2(cursor_x + (button_size * 0.5f), center_y),
                     button_size, helper.button, ease);

    cursor_x += button_size + (8.0f * scale);
    const char* const text_begin = helper.description.data();
    const char* const text_end = text_begin + helper.description.size();
    const ImVec2 text_size = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, text_begin, text_end);
    dl->AddText(font, font_size, ImVec2(cursor_x, center_y - (text_size.y * 0.5f)), text_color,
                text_begin, text_end);
    cursor_x += text_size.x + item_spacing;
  }
}

void RenderSocialArea(ImDrawList* dl, float ease)
{
  if (ease <= 0.0f)
    return;

  const float scale = ImGui::GetIO().FontGlobalScale;
  const float start_offset = 200.0f * scale;
  const float current_offset = start_offset * (1.0f - ease);
  const float avatar_size = 72.0f * scale;
  const float side_margin = 32.0f * scale;
  const float top_margin = 32.0f * scale;
  const float bar_height = 50.0f * scale;
  const ImVec2 avatar_center(side_margin + (avatar_size * 0.5f) - current_offset,
                             top_margin + (bar_height * 0.5f));
  const float radius = avatar_size * 0.5f;

  dl->AddCircleFilled(avatar_center, radius,
                      IM_COL32(45, 45, 45, static_cast<int>(255.0f * ease)));

  const float image_radius = radius - (4.0f * scale);
  if (s_avatar_texture_id != 0)
  {
    const ImVec2 image_min(avatar_center.x - image_radius, avatar_center.y - image_radius);
    const ImVec2 image_max(avatar_center.x + image_radius, avatar_center.y + image_radius);
    dl->AddImageRounded(ImTextureRef(static_cast<ImTextureID>(s_avatar_texture_id)), image_min,
                        image_max, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), IM_COL32_WHITE,
                        image_radius);
    dl->AddCircle(avatar_center, image_radius, IM_COL32(255, 255, 255, 60), 0, 1.0f);
  }
  else
  {
    dl->AddCircleFilled(avatar_center, image_radius, IM_COL32(200, 200, 210, 255));
  }
}

void RenderStatusBar(ImDrawList* dl, ImVec2 display_size, float ease)
{
  const float scale = ImGui::GetIO().FontGlobalScale;
  const float bar_height = 50.0f * scale;
  const float top_margin = 32.0f * scale;
  const float side_margin = 32.0f * scale;
  const float item_spacing = 20.0f * scale;
  const float padding = 20.0f * scale;
  ImFont* font = ImGui::GetFont();
  const float font_size = ImGui::GetFontSize();

  std::time_t now = std::time(nullptr);
  std::tm local_tm{};
  char time_str[16] = "00:00";
  if (localtime_r(&now, &local_tm))
    std::strftime(time_str, sizeof(time_str), "%H:%M", &local_tm);

  u32 battery_level = 0;
  const bool has_battery = R_SUCCEEDED(psmGetBatteryChargePercentage(&battery_level));
  PsmChargerType charger_type = PsmChargerType_Unconnected;
  const bool charging =
      R_SUCCEEDED(psmGetChargerType(&charger_type)) && charger_type != PsmChargerType_Unconnected;

  float total_width = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, time_str).x;
  if (has_battery)
    total_width += item_spacing + (34.0f * scale);
  total_width += padding * 2.0f;

  const float bar_x = display_size.x - total_width - side_margin;
  const float bar_y = top_margin + ((1.0f - ease) * -20.0f);
  const float center_y = bar_y + (bar_height * 0.5f);
  float cursor_x = bar_x + padding;
  const ImU32 text_color = IM_COL32(200, 200, 200, static_cast<int>(255.0f * ease));

  dl->AddText(font, font_size, ImVec2(cursor_x, center_y - (font_size * 0.5f)), text_color,
              time_str);
  cursor_x += font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, time_str).x;

  if (!has_battery)
    return;

  cursor_x += item_spacing;
  const float body_width = 32.0f * scale;
  const float body_height = 20.0f * scale;
  const float tip_width = 4.0f * scale;
  const float tip_height = 10.0f * scale;
  const ImVec2 body_min(cursor_x, center_y - (body_height * 0.5f));
  const ImVec2 body_max(body_min.x + body_width, body_min.y + body_height);

  dl->AddRect(body_min, body_max, text_color, 3.0f, 0, 2.0f);
  dl->AddRectFilled(
      ImVec2(body_max.x, body_min.y + ((body_height - tip_height) * 0.5f)),
      ImVec2(body_max.x + tip_width, body_min.y + ((body_height + tip_height) * 0.5f)),
      text_color, 2.0f, ImDrawFlags_RoundCornersRight);

  const float pct = std::clamp(static_cast<float>(battery_level) / 100.0f, 0.0f, 1.0f);
  if (pct > 0.0f)
  {
    const float pad = 4.0f * scale;
    const ImVec2 fill_min(body_min.x + pad, body_min.y + pad);
    const ImVec2 fill_max(fill_min.x + ((body_width - (pad * 2.0f)) * pct), body_max.y - pad);
    const ImU32 fill_color =
        charging ? IM_COL32(255, 210, 90, static_cast<int>(255.0f * ease))
                 : IM_COL32(200, 200, 200, static_cast<int>(255.0f * ease));
    dl->AddRectFilled(fill_min, fill_max, fill_color, 1.5f);
  }

  if (charging)
  {
    const ImVec2 center((body_min.x + body_max.x) * 0.5f, (body_min.y + body_max.y) * 0.5f);
    dl->PathLineTo(ImVec2(center.x + (1.0f * scale), center.y - (6.0f * scale)));
    dl->PathLineTo(ImVec2(center.x - (3.0f * scale), center.y));
    dl->PathLineTo(ImVec2(center.x + (0.5f * scale), center.y));
    dl->PathLineTo(ImVec2(center.x - (1.5f * scale), center.y + (6.0f * scale)));
    dl->PathLineTo(ImVec2(center.x + (4.0f * scale), center.y - (1.0f * scale)));
    dl->PathLineTo(ImVec2(center.x, center.y - (1.0f * scale)));
    dl->PathFillConvex(IM_COL32(40, 40, 40, static_cast<int>(255.0f * ease)));
  }
}

int GetToastSlot(ToastCorner corner)
{
  switch (corner)
  {
  case ToastCorner::TopLeft:
    return 0;
  case ToastCorner::TopRight:
    return 1;
  case ToastCorner::BottomLeft:
    return 2;
  case ToastCorner::BottomRight:
    return 3;
  }

  return 0;
}

bool GetAndAdvanceToast(ToastCorner corner, float delta_time, std::string* message, float* timer)
{
  std::lock_guard lock(s_toast_mutex);
  const int slot = GetToastSlot(corner);
  if (s_toast_messages[slot].empty() || s_toast_timers[slot] <= 0.0f)
    return false;

  *message = s_toast_messages[slot];
  *timer = s_toast_timers[slot];
  s_toast_timers[slot] = std::max(0.0f, s_toast_timers[slot] - delta_time);
  if (s_toast_timers[slot] <= 0.0f)
    s_toast_messages[slot].clear();

  return true;
}

void RenderToastSlot(ImDrawList* dl, ToastCorner corner, float delta_time)
{
  std::string message;
  float timer = 0.0f;
  if (!GetAndAdvanceToast(corner, delta_time, &message, &timer))
    return;

  const ImVec2 display_size = ImGui::GetIO().DisplaySize;
  const float scale = ImGui::GetIO().FontGlobalScale;
  const float alpha =
      timer < kToastFadeDuration ? std::clamp(timer / kToastFadeDuration, 0.0f, 1.0f) : 1.0f;
  const float margin_x = 24.0f * scale;
  const float margin_y = 16.0f * scale;
  const float pad_x = 16.0f * scale;
  const float pad_y = 8.0f * scale;
  const float rounding = 14.0f * scale;
  const ImVec2 text_size = ImGui::CalcTextSize(message.c_str());
  const float toast_w = text_size.x + pad_x * 2.0f;
  const float toast_h = text_size.y + pad_y * 2.0f;
  float toast_x = margin_x;
  float toast_y = margin_y;

  if (corner == ToastCorner::TopRight || corner == ToastCorner::BottomRight)
    toast_x = std::max(margin_x, display_size.x - margin_x - toast_w);
  if (corner == ToastCorner::BottomLeft || corner == ToastCorner::BottomRight)
    toast_y = std::max(margin_y, display_size.y - margin_y - toast_h);

  const ImVec2 p0(toast_x, toast_y);
  const ImVec2 p1(p0.x + text_size.x + pad_x * 2.0f, p0.y + text_size.y + pad_y * 2.0f);

  dl->AddRectFilled(p0, p1, IM_COL32(0, 0, 0, static_cast<int>(153.0f * alpha)), rounding);
  dl->AddText(ImVec2(p0.x + pad_x, p0.y + pad_y),
              IM_COL32(255, 255, 255, static_cast<int>(240.0f * alpha)), message.c_str());
}

void RenderToast(ImDrawList* dl, float delta_time)
{
  RenderToastSlot(dl, ToastCorner::TopLeft, delta_time);
  RenderToastSlot(dl, ToastCorner::TopRight, delta_time);
  RenderToastSlot(dl, ToastCorner::BottomLeft, delta_time);
  RenderToastSlot(dl, ToastCorner::BottomRight, delta_time);
}

}  // namespace

void SetVisible(bool visible)
{
  if (visible && !s_visible)
  {
    OverlayTranslation::TranslationManager::Instance().Init();
    LoadViewportSettings();
    s_anim_timer = 0.0f;
    s_selected = 0;
    s_slot_selected = 0;
    s_settings_selected = 0;
    s_menu = MenuScreen::QuickMenu;
  }
  else if (!visible)
  {
    s_anim_timer = 0.0f;
    s_slot_selected = 0;
    s_settings_selected = 0;
    s_menu = MenuScreen::QuickMenu;
  }

  s_visible = visible;
}

void SetGameTitle(std::string title)
{
  s_title = std::move(title);
}

void SetNickname(std::string nickname)
{
  s_nickname = std::move(nickname);
}

void SetAvatarTextureId(unsigned long long texture_id)
{
  s_avatar_texture_id = texture_id;
}

void ShowToast(std::string message, ToastCorner corner)
{
  std::lock_guard lock(s_toast_mutex);
  if (message.empty())
  {
    for (std::string& toast_message : s_toast_messages)
      toast_message.clear();
    s_toast_timers.fill(0.0f);
    return;
  }

  const int slot = GetToastSlot(corner);
  s_toast_messages[slot] = std::move(message);
  s_toast_timers[slot] = kToastDuration;
}

bool HasTransientContent()
{
  std::lock_guard lock(s_toast_mutex);
  for (int i = 0; i < kToastSlotCount; ++i)
  {
    if (!s_toast_messages[i].empty() && s_toast_timers[i] > 0.0f)
      return true;
  }

  return false;
}

void FeedNav(const NavInput& nav)
{
  s_nav = nav;
}

Action Render(int display_w, int display_h)
{
  const float delta_time = ImGui::GetIO().DeltaTime;
  const ImVec2 display_size(static_cast<float>(display_w), static_cast<float>(display_h));
  ImDrawList* dl = ImGui::GetForegroundDrawList();

  if (!s_visible)
  {
    RenderToast(dl, delta_time);
    return Action::None;
  }

  const NavInput nav = s_nav;
  s_nav = {};

  s_anim_timer = std::min(s_anim_timer + delta_time, kAnimDuration);
  const float ease = EaseOutCubic(s_anim_timer / kAnimDuration);

  const bool showing_slots = s_menu == MenuScreen::SaveStates || s_menu == MenuScreen::LoadStates;
  const bool showing_settings = s_menu == MenuScreen::Settings;
  const int item_count =
      showing_slots ? kOverlaySlotCount :
                      (showing_settings ? kSettingsItemCount :
                                          static_cast<int>(kQuickMenuItems.size()));

  if (nav.up)
  {
    if (showing_slots)
      s_slot_selected = (s_slot_selected - 1 + item_count) % item_count;
    else if (showing_settings)
      s_settings_selected = (s_settings_selected - 1 + item_count) % item_count;
    else
      s_selected = (s_selected - 1 + item_count) % item_count;
  }
  if (nav.down)
  {
    if (showing_slots)
      s_slot_selected = (s_slot_selected + 1) % item_count;
    else if (showing_settings)
      s_settings_selected = (s_settings_selected + 1) % item_count;
    else
      s_selected = (s_selected + 1) % item_count;
  }
  if (showing_settings)
  {
    if (nav.left)
      AdvanceSettingsValue(-1);
    if (nav.right)
      AdvanceSettingsValue(1);
  }

  Action result = Action::None;
  if (nav.cancel)
  {
    if (showing_slots || showing_settings)
    {
      s_menu = MenuScreen::QuickMenu;
      s_slot_selected = 0;
      s_settings_selected = 0;
    }
    else
    {
      result = Action::Resume;
    }
  }
  RenderOverlayBackground(dl, display_size, ease);
  RenderTitleCard(dl, display_size, ease);
  RenderMenu(dl, display_size, ease);
  RenderHelpersBar(dl, display_size, ease);
  RenderSocialArea(dl, ease);
  RenderStatusBar(dl, display_size, ease);
  RenderToast(dl, delta_time);

  if (nav.accept)
  {
    if (showing_slots)
    {
      result = s_menu == MenuScreen::SaveStates ? MakeSaveActionForSlot(s_slot_selected) :
                                                  MakeLoadActionForSlot(s_slot_selected);
      s_menu = MenuScreen::QuickMenu;
      s_slot_selected = 0;
    }
    else if (showing_settings)
    {
      AdvanceSettingsValue(1);
    }
    else
    {
      switch (s_selected)
      {
      case 0:
        RefreshSlotLabels();
        s_menu = MenuScreen::SaveStates;
        s_slot_selected = 0;
        break;
      case 1:
        RefreshSlotLabels();
        s_menu = MenuScreen::LoadStates;
        s_slot_selected = 0;
        break;
      case 2:
        s_menu = MenuScreen::Settings;
        s_settings_selected = 0;
        break;
      case 3:
        result = Action::Exit;
        break;
      default:
        break;
      }
    }
  }

  return result;
}

static bool IsActionInRange(Action action, Action first, Action last)
{
  const int value = static_cast<int>(action);
  return value >= static_cast<int>(first) && value <= static_cast<int>(last);
}

}  // namespace DolphinNX::OverlayUI

bool DolphinNX::OverlayUI::IsSaveStateAction(Action action)
{
  return IsActionInRange(action, Action::SaveStateSlot1, Action::SaveStateSlot4);
}

bool DolphinNX::OverlayUI::IsLoadStateAction(Action action)
{
  return IsActionInRange(action, Action::LoadStateSlot1, Action::LoadStateSlot4);
}

int DolphinNX::OverlayUI::GetStateSlotForAction(Action action)
{
  if (IsSaveStateAction(action))
    return static_cast<int>(action) - static_cast<int>(Action::SaveStateSlot1) + 1;
  if (IsLoadStateAction(action))
    return static_cast<int>(action) - static_cast<int>(Action::LoadStateSlot1) + 1;
  return 0;
}
