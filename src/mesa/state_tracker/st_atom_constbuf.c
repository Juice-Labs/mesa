/**************************************************************************
 *
 * Copyright 2007 VMware, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL VMWARE AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/

/*
 * Authors:
 *   Keith Whitwell <keithw@vmware.com>
 *   Brian Paul
 */


#include "program/prog_parameter.h"
#include "program/prog_print.h"
#include "main/shaderapi.h"
#include "pipe/p_context.h"
#include "pipe/p_defines.h"
#include "util/u_inlines.h"
#include "util/u_upload_mgr.h"
#include "util/juice_diag_log.h"
#include "util/log.h"
#include "cso_cache/cso_context.h"

#include "main/bufferobj.h"
#include "st_debug.h"
#include "st_context.h"
#include "st_atom.h"
#include "st_atom_constbuf.h"
#include "st_program.h"

/* Unbinds the CB0 if it's not used by the current program to avoid leaving
 * dangling pointers to old (potentially deleted) shaders in the driver.
 */
static void
st_unbind_unused_cb0(struct st_context *st, enum pipe_shader_type shader_type)
{
   if (st->state.constbuf0_enabled_shader_mask & (1 << shader_type)) {
      struct pipe_context *pipe = st->pipe;

      pipe->set_constant_buffer(pipe, shader_type, 0, false, NULL);
      st->state.constbuf0_enabled_shader_mask &= ~(1 << shader_type);
   }
}

/**
 * Pass the given program parameters to the graphics pipe as a
 * constant buffer.
 */
void
st_upload_constants(struct st_context *st, struct gl_program *prog, gl_shader_stage stage)
{
   enum pipe_shader_type shader_type = pipe_shader_type_from_mesa(stage);
   if (!prog) {
      st_unbind_unused_cb0(st, shader_type);
      return;
   }

   struct gl_program_parameter_list *params = prog->Parameters;

   assert(shader_type == PIPE_SHADER_VERTEX ||
          shader_type == PIPE_SHADER_FRAGMENT ||
          shader_type == PIPE_SHADER_GEOMETRY ||
          shader_type == PIPE_SHADER_TESS_CTRL ||
          shader_type == PIPE_SHADER_TESS_EVAL ||
          shader_type == PIPE_SHADER_COMPUTE);

   /* update the ATI constants before rendering */
   if (shader_type == PIPE_SHADER_FRAGMENT && st->fp->ati_fs) {
      struct ati_fragment_shader *ati_fs = st->fp->ati_fs;
      unsigned c;

      for (c = 0; c < MAX_NUM_FRAGMENT_CONSTANTS_ATI; c++) {
         unsigned offset = params->Parameters[c].ValueOffset;
         if (ati_fs->LocalConstDef & (1 << c))
            memcpy(params->ParameterValues + offset,
                   ati_fs->Constants[c], sizeof(GLfloat) * 4);
         else
            memcpy(params->ParameterValues + offset,
                   st->ctx->ATIFragmentShader.GlobalConstants[c],
                   sizeof(GLfloat) * 4);
      }
   }

   /* Make all bindless samplers/images bound texture/image units resident in
    * the context.
    */
   st_make_bound_samplers_resident(st, prog);
   st_make_bound_images_resident(st, prog);

   /* update constants */
   if (params && params->NumParameters) {
      struct pipe_constant_buffer cb;
      const uint paramBytes = params->NumParameterValues * sizeof(GLfloat);

      _mesa_shader_write_subroutine_indices(st->ctx, stage);

      cb.buffer = NULL;
      cb.user_buffer = NULL;
      cb.buffer_offset = 0;
      cb.buffer_size = paramBytes;

      if (st->prefer_real_buffer_in_constbuf0) {
         struct pipe_context *pipe = st->pipe;
         uint32_t *ptr;

         const unsigned alignment = MAX2(
            st->ctx->Const.UniformBufferOffsetAlignment, 64);

         /* fetch_state always stores 4 components (16 bytes) per matrix row,
          * but matrix rows are sometimes allocated partially, so add 12
          * to compensate for the fetch_state defect.
          */
         u_upload_alloc(pipe->const_uploader, 0, paramBytes + 12,
            alignment, &cb.buffer_offset, &cb.buffer, (void**)&ptr);

         int uniform_bytes = params->UniformBytes;
         if (uniform_bytes)
            memcpy(ptr, params->ParameterValues, uniform_bytes);

         /* Upload the constants which come from fixed-function state, such as
          * transformation matrices, fog factors, etc.
          */
         if (params->StateFlags)
            _mesa_upload_state_parameters(st->ctx, params, ptr);

         u_upload_unmap(pipe->const_uploader);
         pipe->set_constant_buffer(pipe, shader_type, 0, true, &cb);

         /* Set inlinable constants. This is more involved because state
          * parameters are uploaded directly above instead of being loaded
          * into gl_program_parameter_list. The easiest way to get their values
          * is to load them.
          */
         unsigned num_inlinable_uniforms = prog->info.num_inlinable_uniforms;
         if (num_inlinable_uniforms) {
            uint32_t values[MAX_INLINABLE_UNIFORMS];
            gl_constant_value *constbuf = params->ParameterValues;
            bool loaded_state_vars = false;

            for (unsigned i = 0; i < num_inlinable_uniforms; i++) {
               unsigned dw_offset = prog->info.inlinable_uniform_dw_offsets[i];

               if (dw_offset * 4 >= uniform_bytes && !loaded_state_vars) {
                  _mesa_load_state_parameters(st->ctx, params);
                  loaded_state_vars = true;
               }

               values[i] = constbuf[prog->info.inlinable_uniform_dw_offsets[i]].u;
            }

            pipe->set_inlinable_constants(pipe, shader_type,
                                          prog->info.num_inlinable_uniforms,
                                          values);
         }
      } else {
         struct pipe_context *pipe = st->pipe;

         cb.user_buffer = params->ParameterValues;

         /* Update the constants which come from fixed-function state, such as
          * transformation matrices, fog factors, etc.
          */
         if (params->StateFlags)
            _mesa_load_state_parameters(st->ctx, params);

         pipe->set_constant_buffer(pipe, shader_type, 0, false, &cb);

         /* Set inlinable constants. */
         unsigned num_inlinable_uniforms = prog->info.num_inlinable_uniforms;
         if (num_inlinable_uniforms) {
            uint32_t values[MAX_INLINABLE_UNIFORMS];
            gl_constant_value *constbuf = params->ParameterValues;

            for (unsigned i = 0; i < num_inlinable_uniforms; i++)
               values[i] = constbuf[prog->info.inlinable_uniform_dw_offsets[i]].u;

            pipe->set_inlinable_constants(pipe, shader_type,
                                          prog->info.num_inlinable_uniforms,
                                          values);
         }
      }

      st->state.constbuf0_enabled_shader_mask |= 1 << shader_type;
   } else {
      st_unbind_unused_cb0(st, shader_type);
   }
}


/**
 * Vertex shader:
 */
void
st_update_vs_constants(struct st_context *st)
{
   st_upload_constants(st, st->vp, MESA_SHADER_VERTEX);
}

/**
 * Fragment shader:
 */
void
st_update_fs_constants(struct st_context *st)
{
   st_upload_constants(st, st->fp, MESA_SHADER_FRAGMENT);
}


/* Geometry shader:
 */
void
st_update_gs_constants(struct st_context *st)
{
   st_upload_constants(st, st->gp, MESA_SHADER_GEOMETRY);
}

/* Tessellation control shader:
 */
void
st_update_tcs_constants(struct st_context *st)
{
   st_upload_constants(st, st->tcp, MESA_SHADER_TESS_CTRL);
}

/* Tessellation evaluation shader:
 */
void
st_update_tes_constants(struct st_context *st)
{
   st_upload_constants(st, st->tep, MESA_SHADER_TESS_EVAL);
}

/* Compute shader:
 */
void
st_update_cs_constants(struct st_context *st)
{
   st_upload_constants(st, st->cp, MESA_SHADER_COMPUTE);
}

static void
st_bind_ubos(struct st_context *st, struct gl_program *prog,
             enum pipe_shader_type shader_type)
{
   static uint32_t juice_vred_material_ubo_mask;
   static bool juice_vred_shadow_handles_logged;
   static unsigned juice_vred_environment_shadow_buffer_name;
   static bool juice_vred_environment_shadow_buffer_initialized;
   unsigned i;
   struct pipe_constant_buffer cb = { 0 };

   if (!prog)
      return;

   struct pipe_context *pipe = st->pipe;

   for (i = 0; i < prog->sh.NumUniformBlocks; i++) {
      struct gl_buffer_binding *binding;
      struct gl_uniform_block *block = prog->sh.UniformBlocks[i];

      binding = &st->ctx->UniformBufferBindings[block->Binding];

      mesa_logi("MESA UBO READ BINDING: shader=%d, block[%d]='%s', block->Binding=%u, binding=%p", 
                shader_type, i,
                block->name.string ? block->name.string : "(null)",
                block->Binding, binding);
      
      mesa_logi("MESA UBO BINDING CONTENTS: BufferObject=%p, BufferObject->Name=%u, BufferObject->Size=%lu, binding->Offset=%ld, binding->Size=%ld, AutomaticSize=%d",
                binding->BufferObject, 
                binding->BufferObject ? binding->BufferObject->Name : 0,
                binding->BufferObject ? (unsigned long)binding->BufferObject->Size : 0,
                (long)binding->Offset, (long)binding->Size, binding->AutomaticSize);

      cb.buffer = _mesa_get_bufferobj_reference(st->ctx, binding->BufferObject);

      if (cb.buffer) {
         cb.buffer_offset = binding->Offset;
         cb.buffer_size = cb.buffer->width0 - binding->Offset;

         /* AutomaticSize is FALSE if the buffer was set with BindBufferRange.
          * Take the minimum just to be sure.
          */
         if (!binding->AutomaticSize)
            cb.buffer_size = MIN2(cb.buffer_size, (unsigned) binding->Size);
      }
      else {
         cb.buffer_offset = 0;
         cb.buffer_size = 0;
      }

      if (shader_type == PIPE_SHADER_FRAGMENT && prog->Id == 120 && i < 32 &&
          !(juice_vred_material_ubo_mask & BITFIELD_BIT(i))) {
         juice_vred_material_ubo_mask |= BITFIELD_BIT(i);
         juice_diag_logf("ZINK_VRED_UBO_BIND",
                         "prog=%u block=%u name=%s binding=%u gallium_index=%u "
                         "buffer_name=%u offset=%lld size=%u",
                         prog->Id, i,
                         block->name.string ? block->name.string : "?",
                         block->Binding, 1 + i,
                         binding->BufferObject ? binding->BufferObject->Name : 0,
                         (long long)cb.buffer_offset, cb.buffer_size);
      }

      if (shader_type == PIPE_SHADER_FRAGMENT && prog->Id == 120 && i == 12 &&
          cb.buffer && !juice_vred_shadow_handles_logged) {
         const char *shadow_names[] = { "OSGShadowMaps", "OSGShadowCubeMaps" };
         const struct gl_shader_program *shader_program =
            prog->shader_program;
         struct pipe_transfer *transfer = NULL;
         uint64_t handles[ARRAY_SIZE(shadow_names)] = { 0 };
         int offsets[ARRAY_SIZE(shadow_names)] = { -1, -1 };
         int block_indices[ARRAY_SIZE(shadow_names)] = { -1, -1 };

         if (shader_program && shader_program->data) {
            for (unsigned uniform_index = 0;
                 uniform_index < shader_program->data->NumUniformStorage;
                 uniform_index++) {
               const struct gl_uniform_storage *uniform =
                  &shader_program->data->UniformStorage[uniform_index];
               if (!uniform->name.string)
                  continue;
               for (unsigned handle_index = 0;
                    handle_index < ARRAY_SIZE(shadow_names); handle_index++) {
                  if (!strcmp(uniform->name.string, shadow_names[handle_index])) {
                     offsets[handle_index] = uniform->offset;
                     block_indices[handle_index] = uniform->block_index;
                  }
               }
            }
         }

         for (unsigned handle_index = 0;
              handle_index < ARRAY_SIZE(shadow_names); handle_index++) {
            if (offsets[handle_index] < 0 ||
                (uint64_t)offsets[handle_index] + sizeof(handles[handle_index]) >
                   cb.buffer_size)
               continue;
            void *mapped = pipe_buffer_map_range(
               pipe, cb.buffer, cb.buffer_offset + offsets[handle_index],
               sizeof(handles[handle_index]), PIPE_MAP_READ | PIPE_MAP_ONCE,
               &transfer);
            if (mapped) {
               memcpy(&handles[handle_index], mapped, sizeof(handles[handle_index]));
               pipe_buffer_unmap(pipe, transfer);
               transfer = NULL;
            }
         }

         juice_diag_logf("ZINK_VRED_SHADOW_HANDLES",
                         "linked_program=%u buffer_name=%u map_offset=%lld "
                         "shadow_maps_block=%d "
                         "shadow_maps_offset=%d shadow_maps=0x%llx "
                         "shadow_cube_maps_block=%d shadow_cube_maps_offset=%d "
                         "shadow_cube_maps=0x%llx",
                         shader_program ? shader_program->Name : 0,
                         binding->BufferObject ? binding->BufferObject->Name : 0,
                         (long long)cb.buffer_offset, block_indices[0], offsets[0],
                         (unsigned long long)handles[0], block_indices[1],
                         offsets[1],
                         (unsigned long long)handles[1]);
         juice_vred_shadow_handles_logged = true;
      }

        if (shader_type == PIPE_SHADER_FRAGMENT &&
           block->name.string && !strcmp(block->name.string, "OSGEnvironment") &&
           cb.buffer && binding->BufferObject &&
           (!juice_vred_environment_shadow_buffer_initialized ||
            binding->BufferObject->Name != juice_vred_environment_shadow_buffer_name)) {
         const struct gl_shader_program *shader_program = prog->shader_program;
         struct pipe_transfer *transfer = NULL;
         unsigned shadow_member_count = 0;
         unsigned nonzero_handle_count = 0;
         int environment_block_index = -1;

         if (shader_program && shader_program->data) {
            for (unsigned block_index = 0;
                 block_index < shader_program->data->NumUniformBlocks;
                 block_index++) {
               const struct gl_uniform_block *linked_block =
                  &shader_program->data->UniformBlocks[block_index];
               if (linked_block->name.string &&
                   !strcmp(linked_block->name.string, block->name.string)) {
                  environment_block_index = block_index;
                  break;
               }
            }

            for (unsigned uniform_index = 0;
                 uniform_index < shader_program->data->NumUniformStorage;
                 uniform_index++) {
               const struct gl_uniform_storage *uniform =
                  &shader_program->data->UniformStorage[uniform_index];
               const char *name = uniform->name.string;
               if (uniform->block_index != environment_block_index || !name ||
                   strncmp(name, "OSGEnvironment.lights[", 22) ||
                   (!strstr(name, ".shadowTexture") &&
                    !strstr(name, ".shadowCubeTexture")))
                  continue;

               shadow_member_count++;
               if (uniform->offset < 0 ||
                   (uint64_t)uniform->offset + sizeof(uint64_t) > cb.buffer_size)
                  continue;

               uint64_t handle = 0;
               void *mapped = pipe_buffer_map_range(
                  pipe, cb.buffer, cb.buffer_offset + uniform->offset,
                  sizeof(handle), PIPE_MAP_READ | PIPE_MAP_ONCE, &transfer);
               if (mapped) {
                  memcpy(&handle, mapped, sizeof(handle));
                  pipe_buffer_unmap(pipe, transfer);
                  transfer = NULL;
               }
               if (!handle)
                  continue;

               nonzero_handle_count++;
               juice_diag_logf("ZINK_VRED_ENV_SHADOW_HANDLE",
                               "linked_program=%u buffer_name=%u name=%s block=%d "
                               "offset=%d handle=0x%llx",
                               shader_program->Name,
                               binding->BufferObject ? binding->BufferObject->Name : 0,
                               name, uniform->block_index, uniform->offset,
                               (unsigned long long)handle);
            }
         }

         juice_diag_logf("ZINK_VRED_ENV_SHADOW_SUMMARY",
                         "linked_program=%u buffer_name=%u block=%d members=%u nonzero=%u",
                         shader_program ? shader_program->Name : 0,
                         binding->BufferObject ? binding->BufferObject->Name : 0,
                         environment_block_index,
                         shadow_member_count, nonzero_handle_count);
         juice_vred_environment_shadow_buffer_name = binding->BufferObject->Name;
         juice_vred_environment_shadow_buffer_initialized = true;
      }

      mesa_logi("MESA UBO BIND: shader=%d, block[%d]='%s', Binding=%u, gallium_index=%u, cb.buffer=%p, cb.buffer_size=%u", 
                shader_type, i, 
                block->name.string ? block->name.string : "(null)",
                block->Binding, 1 + i, cb.buffer, cb.buffer_size);

      pipe->set_constant_buffer(pipe, shader_type, 1 + i, true, &cb);
   }
}

void
st_bind_vs_ubos(struct st_context *st)
{
   struct gl_program *prog =
      st->ctx->_Shader->CurrentProgram[MESA_SHADER_VERTEX];

   st_bind_ubos(st, prog, PIPE_SHADER_VERTEX);
}

void
st_bind_fs_ubos(struct st_context *st)
{
   struct gl_program *prog =
      st->ctx->_Shader->CurrentProgram[MESA_SHADER_FRAGMENT];

   st_bind_ubos(st, prog, PIPE_SHADER_FRAGMENT);
}

void
st_bind_gs_ubos(struct st_context *st)
{
   struct gl_program *prog =
      st->ctx->_Shader->CurrentProgram[MESA_SHADER_GEOMETRY];

   st_bind_ubos(st, prog, PIPE_SHADER_GEOMETRY);
}

void
st_bind_tcs_ubos(struct st_context *st)
{
   struct gl_program *prog =
      st->ctx->_Shader->CurrentProgram[MESA_SHADER_TESS_CTRL];

   st_bind_ubos(st, prog, PIPE_SHADER_TESS_CTRL);
}

void
st_bind_tes_ubos(struct st_context *st)
{
   struct gl_program *prog =
      st->ctx->_Shader->CurrentProgram[MESA_SHADER_TESS_EVAL];

   st_bind_ubos(st, prog, PIPE_SHADER_TESS_EVAL);
}

void
st_bind_cs_ubos(struct st_context *st)
{
   struct gl_program *prog =
      st->ctx->_Shader->CurrentProgram[MESA_SHADER_COMPUTE];

   st_bind_ubos(st, prog, PIPE_SHADER_COMPUTE);
}
