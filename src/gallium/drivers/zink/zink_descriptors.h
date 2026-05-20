/*
 * Copyright © 2020 Mike Blumenkrantz
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 * 
 * Authors:
 *    Mike Blumenkrantz <michael.blumenkrantz@gmail.com>
 */

#ifndef ZINK_DESCRIPTOR_H
# define  ZINK_DESCRIPTOR_H

#include "zink_types.h"
#ifdef __cplusplus
extern "C" {
#endif

#define ZINK_DESCRIPTOR_COMPACT 2

enum zink_pipeline_idx;


#define ZINK_BINDLESS_IS_BUFFER(HANDLE) (HANDLE >= ZINK_MAX_BINDLESS_HANDLES)

/* JUICE FIX: split the bindless descriptor set across multiple bindings, one
 * per (VkDescriptorType, glsl_sampler_dim, is_array, is_shadow) tuple so that
 * each SPIR-V OpTypeImage has its own dedicated Vulkan binding (no aliasing).
 *
 * The pre-rework layout (4 bindings, all dim/array/shadow aliased at one
 * binding per descriptor type) produces invalid SPIR-V on shaders that mix
 * e.g. sampler1D and sampler2D bindless: aliased descriptors must have
 * matching OpTypeImage Dim/Sampled/Format, which they don't here. RADV faults,
 * NVIDIA renders black/garbage.
 *
 * Encoding (binding number within set ZINK_DESCRIPTOR_BINDLESS):
 *   COMBINED_IMAGE_SAMPLER:  0..23   (6 dims x 2 array x 2 shadow)
 *   UNIFORM_TEXEL_BUFFER:    24
 *   STORAGE_IMAGE:           25..36  (6 dims x 2 array; shadow N/A)
 *   STORAGE_TEXEL_BUFFER:    37
 */

static inline unsigned
zink_bindless_dim_index(enum glsl_sampler_dim dim)
{
   switch (dim) {
   case GLSL_SAMPLER_DIM_1D:   return 0;
   case GLSL_SAMPLER_DIM_2D:   return 1;
   case GLSL_SAMPLER_DIM_3D:   return 2;
   case GLSL_SAMPLER_DIM_CUBE: return 3;
   case GLSL_SAMPLER_DIM_RECT: return 4;
   case GLSL_SAMPLER_DIM_MS:   return 5;
   default:                    return 1; /* SUBPASS/EXTERNAL fall back to 2D */
   }
}

static inline unsigned
zink_bindless_get_binding(VkDescriptorType type, enum glsl_sampler_dim dim,
                          bool is_array, bool is_shadow)
{
   switch (type) {
   case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: {
      unsigned d = zink_bindless_dim_index(dim);
      return ZINK_BINDLESS_SAMPLER_FIRST + d * 4 + (is_array ? 2 : 0) + (is_shadow ? 1 : 0);
   }
   case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
      return ZINK_BINDLESS_UTEX_BINDING;
   case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE: {
      unsigned d = zink_bindless_dim_index(dim);
      return ZINK_BINDLESS_IMAGE_FIRST + d * 2 + (is_array ? 1 : 0);
   }
   case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
      return ZINK_BINDLESS_STEX_BINDING;
   default:
      UNREACHABLE("unknown vk descriptor type for bindless");
   }
}

static inline VkDescriptorType
zink_bindless_binding_type(unsigned binding)
{
   if (binding <= ZINK_BINDLESS_SAMPLER_LAST) return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
   if (binding == ZINK_BINDLESS_UTEX_BINDING) return VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
   if (binding <= ZINK_BINDLESS_IMAGE_LAST)   return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
   if (binding == ZINK_BINDLESS_STEX_BINDING) return VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
   UNREACHABLE("unknown bindless binding");
}

static inline enum zink_descriptor_size_index
zink_vktype_to_size_idx(VkDescriptorType type)
{
   switch (type) {
   case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
   case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
      return ZDS_INDEX_UBO;
   case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
   case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
      return ZDS_INDEX_COMBINED_SAMPLER;
   case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
      return ZDS_INDEX_UNIFORM_TEXELS;
   case VK_DESCRIPTOR_TYPE_SAMPLER:
      return ZDS_INDEX_SAMPLER;
   case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
      return ZDS_INDEX_STORAGE_BUFFER;
   case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
      return ZDS_INDEX_STORAGE_IMAGE;
   case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
      return ZDS_INDEX_STORAGE_TEXELS;
   default: break;
   }
   UNREACHABLE("unknown type");
}

static inline enum zink_descriptor_size_index_compact
zink_vktype_to_size_idx_comp(VkDescriptorType type)
{
   switch (type) {
   case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
   case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
      return ZDS_INDEX_COMP_UBO;
   case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
   case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
      return ZDS_INDEX_COMP_COMBINED_SAMPLER;
   case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
      return ZDS_INDEX_COMP_UNIFORM_TEXELS;
   case VK_DESCRIPTOR_TYPE_SAMPLER:
      return ZDS_INDEX_COMP_SAMPLER;
   case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
      return ZDS_INDEX_COMP_STORAGE_BUFFER;
   case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
      return ZDS_INDEX_COMP_STORAGE_IMAGE;
   case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
      return ZDS_INDEX_COMP_STORAGE_TEXELS;
   default: break;
   }
   UNREACHABLE("unknown type");
}

static inline enum zink_descriptor_size_index
zink_descriptor_type_to_size_idx(enum zink_descriptor_type type)
{
   switch (type) {
   case ZINK_DESCRIPTOR_TYPE_UBO:
      return ZDS_INDEX_UBO;
   case ZINK_DESCRIPTOR_TYPE_SAMPLER_VIEW:
      return ZDS_INDEX_COMBINED_SAMPLER;
   case ZINK_DESCRIPTOR_TYPE_SSBO:
      return ZDS_INDEX_STORAGE_BUFFER;
   case ZINK_DESCRIPTOR_TYPE_IMAGE:
      return ZDS_INDEX_STORAGE_IMAGE;
   default: break;
   }
   UNREACHABLE("unknown type");
}

static inline enum zink_descriptor_size_index_compact
zink_descriptor_type_to_size_idx_comp(enum zink_descriptor_type type)
{
   switch (type) {
   case ZINK_DESCRIPTOR_TYPE_UBO:
      return ZDS_INDEX_COMP_UBO;
   case ZINK_DESCRIPTOR_TYPE_SAMPLER_VIEW:
      return ZDS_INDEX_COMP_COMBINED_SAMPLER;
   case ZINK_DESCRIPTOR_TYPE_SSBO:
   case ZINK_DESCRIPTOR_TYPE_IMAGE:
   default: break;
   }
   UNREACHABLE("unknown type");
}

/* bindless descriptor bindings have their own struct indexing */
ALWAYS_INLINE static VkDescriptorType
zink_descriptor_type_from_bindless_index(unsigned idx)
{
   switch (idx) {
   case 0: return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
   case 1: return VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
   case 2: return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
   case 3: return VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
   default:
      UNREACHABLE("unknown index");
   }
}

ALWAYS_INLINE static unsigned
zink_descriptor_stage_idx(enum mesa_shader_stage stage)
{
   if (stage == MESA_SHADER_TASK || stage == MESA_SHADER_MESH)
      return stage - MESA_SHADER_TASK;
   /* clamp compute bindings for better driver efficiency */
   if (mesa_shader_stage_is_compute(stage))
      return 0;
   return stage;
}

bool
zink_descriptor_layouts_init(struct zink_screen *screen);

void
zink_descriptor_layouts_deinit(struct zink_screen *screen);

bool
zink_descriptor_util_alloc_sets(struct zink_screen *screen, VkDescriptorSetLayout dsl, VkDescriptorPool pool, VkDescriptorSet *sets, unsigned num_sets);
void
zink_descriptor_util_init_fbfetch(struct zink_context *ctx);
VkImageLayout
zink_descriptor_util_image_layout_eval(const struct zink_context *ctx, const struct zink_resource *res, bool is_compute);
void
zink_descriptors_init_bindless(struct zink_context *ctx);
void
zink_descriptors_deinit_bindless(struct zink_context *ctx);
void
zink_descriptors_update_bindless(struct zink_context *ctx);

void
zink_descriptor_shader_get_binding_offsets(const struct zink_shader *shader, unsigned *offsets);
void
zink_descriptor_shader_init(struct zink_screen *screen, struct zink_shader *shader);
void
zink_descriptor_shader_deinit(struct zink_screen *screen, struct zink_shader *shader);

bool
zink_descriptor_program_init(struct zink_context *ctx, struct zink_program *pg);

void
zink_descriptor_program_deinit(struct zink_screen *screen, struct zink_program *pg);

void
zink_descriptors_update(struct zink_context *ctx, enum zink_pipeline_idx pidx);


void
zink_context_invalidate_descriptor_state(struct zink_context *ctx, mesa_shader_stage shader, enum zink_descriptor_type type, unsigned, unsigned);
void
zink_context_invalidate_descriptor_state_compact(struct zink_context *ctx, mesa_shader_stage shader, enum zink_descriptor_type type, unsigned, unsigned);

void
zink_batch_descriptor_deinit(struct zink_screen *screen, struct zink_batch_state *bs);
void
zink_batch_descriptor_reset(struct zink_screen *screen, struct zink_batch_state *bs);
bool
zink_batch_descriptor_init(struct zink_screen *screen, struct zink_batch_state *bs);

bool
zink_descriptors_init(struct zink_context *ctx);

void
zink_descriptors_deinit(struct zink_context *ctx);

#ifdef __cplusplus
}
#endif

#endif
