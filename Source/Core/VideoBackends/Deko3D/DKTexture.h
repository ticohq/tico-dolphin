// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <string_view>
#include <vector>

#include <deko3d.hpp>

#include "Common/CommonTypes.h"

#include "VideoCommon/AbstractFramebuffer.h"
#include "VideoCommon/AbstractStagingTexture.h"
#include "VideoCommon/AbstractTexture.h"

namespace Deko3D
{
class DKTexture final : public AbstractTexture
{
public:
  DKTexture(const TextureConfig& config, dk::UniqueMemBlock memblock, const dk::ImageLayout& layout,
            const dk::Image& image, const DkImageDescriptor& descriptor);
  ~DKTexture() override;

  static std::unique_ptr<DKTexture> Create(const TextureConfig& config, std::string_view name);

  // Wraps an image whose memory belongs to someone else.
  static std::unique_ptr<DKTexture>
  CreateAdopted(const TextureConfig& config, const dk::ImageLayout& layout, const dk::Image& image);

  static DkImageFormat GetDkFormatForHostTextureFormat(AbstractTextureFormat format);

  static DkImageDescriptor MakeBufferDescriptor(DkMemBlock memblock, DkImageFormat format,
                                                u32 element_size);

  void CopyRectangleFromTexture(const AbstractTexture* src,
                                const MathUtil::Rectangle<int>& src_rect, u32 src_layer,
                                u32 src_level, const MathUtil::Rectangle<int>& dst_rect,
                                u32 dst_layer, u32 dst_level) override;
  void ResolveFromTexture(const AbstractTexture* src, const MathUtil::Rectangle<int>& rect,
                          u32 layer, u32 level) override;
  void Load(u32 level, u32 width, u32 height, u32 row_length, const u8* buffer, size_t buffer_size,
            u32 layer) override;

  const dk::Image& GetImage() const { return m_image; }
  const DkImageDescriptor& GetDescriptor() const { return m_descriptor; }

  // Builds a view over a single mip/layer.
  // Used for transfers and render-target binding.
  DkImageView MakeView(u32 level, u32 layer, u32 layer_count) const;

private:
  dk::UniqueMemBlock m_memblock;
  dk::ImageLayout m_layout;
  dk::Image m_image;

  // Baked sampling descriptor. The state tracker copies this into the frame's descriptor set.
  DkImageDescriptor m_descriptor;
};

class DKStagingTexture final : public AbstractStagingTexture
{
public:
  DKStagingTexture(StagingTextureType type, const TextureConfig& config,
                   dk::UniqueMemBlock memblock);
  ~DKStagingTexture() override;

  static std::unique_ptr<DKStagingTexture> Create(StagingTextureType type,
                                                  const TextureConfig& config);

  void CopyFromTexture(const AbstractTexture* src, const MathUtil::Rectangle<int>& src_rect,
                       u32 src_layer, u32 src_level,
                       const MathUtil::Rectangle<int>& dst_rect) override;
  void CopyToTexture(const MathUtil::Rectangle<int>& src_rect, AbstractTexture* dst,
                     const MathUtil::Rectangle<int>& dst_rect, u32 dst_layer,
                     u32 dst_level) override;

  bool Map() override;
  void Unmap() override;
  void Flush() override;

private:
  dk::UniqueMemBlock m_memblock;
  u64 m_flush_fence_counter = 0;
};

class DKFramebuffer final : public AbstractFramebuffer
{
public:
  DKFramebuffer(AbstractTexture* color_attachment, AbstractTexture* depth_attachment,
                std::vector<AbstractTexture*> additional_color_attachments,
                AbstractTextureFormat color_format, AbstractTextureFormat depth_format, u32 width,
                u32 height, u32 layers, u32 samples);
  ~DKFramebuffer() override;

  static std::unique_ptr<DKFramebuffer>
  Create(DKTexture* color_attachment, DKTexture* depth_attachment,
         std::vector<AbstractTexture*> additional_color_attachments);

  u32 GetNumberOfAdditionalAttachments() const
  {
    return static_cast<u32>(m_additional_color_attachments.size());
  }

  // Binds this framebuffer's attachments as the queue's render targets.
  void Bind(DkCmdBuf cmdbuf) const;

  // Drops every attachment from the sampler bindings,
  // since nothing may be sampled while it is being rendered into.
  void Unbind() const;
};
}  // namespace Deko3D
