// LuaLoader GUI theme. SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "imgui.h"
#include <algorithm>
#include <cstring>

namespace lualoader::gui::theme {
inline ImVec4 accent() { return ImVec4(0.35f, 0.85f, 0.73f, 1.0f); }

inline void apply() {
    ImGui::StyleColorsDark();
    auto &s = ImGui::GetStyle();
    s.WindowPadding = ImVec2(20, 18);
    s.FramePadding = ImVec2(14, 10);
    s.ItemSpacing = ImVec2(12, 12);
    s.ItemInnerSpacing = ImVec2(10, 8);
    s.WindowRounding = 16;
    s.ChildRounding = 12;
    s.FrameRounding = 9;
    s.PopupRounding = 12;
    s.ScrollbarRounding = 9;
    s.GrabRounding = 7;
    s.TabRounding = 8;
    s.WindowBorderSize = 1;
    s.FrameBorderSize = 0;
    s.PopupBorderSize = 1;
    s.ScrollbarSize = 12;
    s.GrabMinSize = 16;
    s.WindowTitleAlign = ImVec2(0.06f, 0.5f);
    s.SeparatorTextPadding = ImVec2(0, 8);
    auto *c = s.Colors;
    c[ImGuiCol_Text] = ImVec4(0.91f, 0.94f, 0.95f, 1);
    c[ImGuiCol_TextDisabled] = ImVec4(0.56f, 0.63f, 0.68f, 1);
    c[ImGuiCol_WindowBg] = ImVec4(0.055f, 0.072f, 0.09f, 0.98f);
    c[ImGuiCol_ChildBg] = ImVec4(0.08f, 0.105f, 0.125f, 1);
    c[ImGuiCol_PopupBg] = ImVec4(0.08f, 0.105f, 0.125f, 0.99f);
    c[ImGuiCol_Border] = ImVec4(0.20f, 0.27f, 0.30f, 0.65f);
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TitleBg] = ImVec4(0.08f, 0.11f, 0.13f, 1);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.105f, 0.16f, 0.18f, 1);
    c[ImGuiCol_TitleBgCollapsed] = c[ImGuiCol_TitleBg];
    c[ImGuiCol_FrameBg] = ImVec4(0.11f, 0.15f, 0.18f, 1);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.16f, 0.23f, 0.26f, 1);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.15f, 0.29f, 0.29f, 1);
    c[ImGuiCol_Button] = ImVec4(0.12f, 0.23f, 0.25f, 1);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.17f, 0.35f, 0.35f, 1);
    c[ImGuiCol_ButtonActive] = ImVec4(0.19f, 0.43f, 0.40f, 1);
    c[ImGuiCol_CheckMark] = accent();
    c[ImGuiCol_SliderGrab] = accent();
    c[ImGuiCol_SliderGrabActive] = ImVec4(0.53f, 0.95f, 0.83f, 1);
    c[ImGuiCol_Header] = ImVec4(0.105f, 0.18f, 0.20f, 1);
    c[ImGuiCol_HeaderHovered] = c[ImGuiCol_ButtonHovered];
    c[ImGuiCol_HeaderActive] = c[ImGuiCol_ButtonActive];
    c[ImGuiCol_Separator] = ImVec4(0.20f, 0.27f, 0.30f, 0.65f);
    c[ImGuiCol_SeparatorHovered] = accent();
    c[ImGuiCol_SeparatorActive] = accent();
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(0.24f, 0.32f, 0.35f, 1);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.31f, 0.43f, 0.45f, 1);
    c[ImGuiCol_ScrollbarGrabActive] = accent();
    c[ImGuiCol_ResizeGrip] = ImVec4(0.35f, 0.85f, 0.73f, 0.20f);
    c[ImGuiCol_ResizeGripHovered] = ImVec4(0.35f, 0.85f, 0.73f, 0.6f);
    c[ImGuiCol_ResizeGripActive] = accent();
    c[ImGuiCol_TextSelectedBg] = ImVec4(0.35f, 0.85f, 0.73f, 0.25f);
    c[ImGuiCol_PlotHistogram] = accent();
    c[ImGuiCol_NavHighlight] = accent();
}

inline void heading(const char *version) {
    ImGui::TextColored(accent(), "LUA / MOD CONTROL");
    ImGui::SameLine();
    ImGui::TextDisabled("v%s", version);
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * 0.60f);
}

// Preserve Checkbox's label/## ID rules and boolean API, with a larger touch target.
inline bool toggle(const char *label, bool *value, float scale) {
    const float h = std::max(38.0f * scale, ImGui::GetFrameHeight());
    const float track_w = 46.0f * scale;
    const char *end = std::strstr(label, "##");
    if (!end) end = label + std::strlen(label);
    const ImVec2 text = ImGui::CalcTextSize(label, end);
    const float gap = text.x > 0 ? ImGui::GetStyle().ItemInnerSpacing.x : 0;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool changed = ImGui::InvisibleButton(label, ImVec2(track_w + gap + text.x, h));
    if (changed) *value = !*value;
    auto *dl = ImGui::GetWindowDrawList();
    const float th = 26.0f * scale;
    const float y = p.y + (h - th) * 0.5f;
    const ImU32 bg = ImGui::GetColorU32(*value ? accent() : ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
    dl->AddRectFilled(ImVec2(p.x, y), ImVec2(p.x + track_w, y + th), bg, th * 0.5f);
    if (ImGui::IsItemHovered() || ImGui::IsItemFocused())
        dl->AddRect(ImVec2(p.x - 2 * scale, y - 2 * scale),
                    ImVec2(p.x + track_w + 2 * scale, y + th + 2 * scale),
                    ImGui::GetColorU32(accent()), th * 0.5f, 0, scale);
    const float r = 9.0f * scale;
    const float cx = *value ? p.x + track_w - th * 0.5f : p.x + th * 0.5f;
    dl->AddCircleFilled(ImVec2(cx, y + th * 0.5f), r,
        *value ? IM_COL32(17, 47, 43, 255) : IM_COL32(177, 194, 202, 255));
    dl->AddText(ImVec2(p.x + track_w + gap, p.y + (h - text.y) * 0.5f),
                ImGui::GetColorU32(ImGuiCol_Text), label, end);
    return changed;
}
} // namespace lualoader::gui::theme
