/*
 * Copyright (c) 2022-2025, NVIDIA CORPORATION.  All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0.
 *
 * SPDX-FileCopyrightText: Copyright (c) 2022-2025, NVIDIA CORPORATION.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ImguiAxis.h"

#include "../common/Common.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#include <glm/ext/scalar_constants.hpp>

namespace demo::ui {
namespace {

struct AxisGeom
{
  AxisGeom()
  {
    constexpr float asize   = 1.0f;
    constexpr float aradius = 0.11f;
    constexpr float abase   = 0.66f;
    constexpr int   asubdiv = 8;

    red.emplace_back(asize, 0.0f, 0.0f);
    for(int i = 0; i <= asubdiv; ++i)
    {
      const float angle = 2.0f * glm::pi<float>() * static_cast<float>(i) / static_cast<float>(asubdiv);
      red.emplace_back(abase, std::cos(angle) * aradius, std::sin(angle) * aradius);
    }
    for(int i = 0; i <= asubdiv - 1; ++i)
    {
      indices.push_back(0);
      indices.push_back(i + 1);
      indices.push_back(i + 2);
    }

    const int center = static_cast<int>(red.size());
    red.emplace_back(abase, 0.0f, 0.0f);
    for(int i = 0; i <= asubdiv; ++i)
    {
      const float angle = -2.0f * glm::pi<float>() * static_cast<float>(i) / static_cast<float>(asubdiv);
      red.emplace_back(abase, std::cos(angle) * aradius, std::sin(angle) * aradius);
    }
    for(int i = 0; i <= asubdiv - 1; ++i)
    {
      indices.push_back(center);
      indices.push_back(center + i + 1);
      indices.push_back(center + i + 2);
    }

    red.emplace_back(0.0f, 0.0f, 0.0f);

    for(const glm::vec3& v : red)
    {
      green.emplace_back(v.z, v.x, v.y);
      blue.emplace_back(v.y, v.z, v.x);
    }
  }

  [[nodiscard]] std::vector<glm::vec3> transform(const std::vector<glm::vec3>& input,
                                                 const ImVec2&                 pos,
                                                 const glm::mat4&              modelView,
                                                 float                         size) const
  {
    std::vector<glm::vec3> output(input.size());
    for(size_t i = 0; i < input.size(); ++i)
    {
      output[i] = glm::vec3(modelView * glm::vec4(input[i], 0.0f));
      output[i].x = output[i].x * size + pos.x;
      output[i].y = output[i].y * -size + pos.y;
    }
    return output;
  }

  static void drawTriangle(ImDrawList& drawList, ImVec2 v0, ImVec2 v1, ImVec2 v2, const ImVec2& uv, ImU32 color)
  {
    const ImVec2 d0(v1.x - v0.x, v1.y - v0.y);
    const ImVec2 d1(v2.x - v0.x, v2.y - v0.y);
    const float  cross = d0.x * d1.y - d0.y * d1.x;
    if(cross > 0.0f)
    {
      v1 = v0;
      v2 = v0;
    }

    drawList.PrimVtx(v0, uv, color);
    drawList.PrimVtx(v1, uv, color);
    drawList.PrimVtx(v2, uv, color);
  }

  void draw(ImDrawList& drawList, const std::vector<glm::vec3>& vertices, ImU32 color, float lineWidth) const
  {
    const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
    drawList.PrimReserve(static_cast<int>(indices.size()), static_cast<int>(indices.size()));

    for(size_t i = 0; i < indices.size(); i += 3)
    {
      const glm::vec3& p0 = vertices[static_cast<size_t>(indices[i + 0])];
      const glm::vec3& p1 = vertices[static_cast<size_t>(indices[i + 1])];
      const glm::vec3& p2 = vertices[static_cast<size_t>(indices[i + 2])];
      drawTriangle(drawList, ImVec2(p0.x, p0.y), ImVec2(p1.x, p1.y), ImVec2(p2.x, p2.y), uv, color);
    }

    drawList.AddLine(ImVec2(vertices.front().x, vertices.front().y),
                     ImVec2(vertices.back().x, vertices.back().y),
                     color,
                     lineWidth);
  }

  std::vector<glm::vec3> red;
  std::vector<glm::vec3> green;
  std::vector<glm::vec3> blue;
  std::vector<int>       indices;
};

void DrawAxis(ImDrawList& drawList, ImVec2 pos, const glm::mat4& modelView, float size, float dpiScale)
{
  static AxisGeom axisGeom;

  struct Arrow
  {
    std::vector<glm::vec3> vertices;
    ImU32                  color{0};
  };

  const float scaledSize = size * dpiScale;
  std::array<Arrow, 3> arrows{{
      {axisGeom.transform(axisGeom.red, pos, modelView, scaledSize), IM_COL32(220, 40, 35, 255)},
      {axisGeom.transform(axisGeom.green, pos, modelView, scaledSize), IM_COL32(40, 210, 65, 255)},
      {axisGeom.transform(axisGeom.blue, pos, modelView, scaledSize), IM_COL32(60, 115, 235, 255)},
  }};

  std::sort(arrows.begin(), arrows.end(), [](const Arrow& lhs, const Arrow& rhs) {
    return lhs.vertices.front().z < rhs.vertices.front().z;
  });

  const float lineWidth = std::max(1.0f, dpiScale);
  axisGeom.draw(drawList, arrows[0].vertices, arrows[0].color, lineWidth);
  axisGeom.draw(drawList, arrows[1].vertices, arrows[1].color, lineWidth);
  axisGeom.draw(drawList, arrows[2].vertices, arrows[2].color, lineWidth);
}

}  // namespace

void DrawAxisInRect(const glm::vec4& rect, const glm::mat4& modelView, float size)
{
  if(rect.z <= 1.0f || rect.w <= 1.0f)
  {
    return;
  }

  const ImGuiViewport* viewport = ImGui::GetMainViewport();
  const float          dpiScale = viewport != nullptr ? viewport->DpiScale : 1.0f;
  const float          margin   = size * 1.35f * dpiScale;
  const ImVec2         pos(rect.x + margin, rect.y + rect.w - margin);

  DrawAxis(*ImGui::GetForegroundDrawList(), pos, modelView, size, dpiScale);
}

}  // namespace demo::ui
