/**************************************************************************
 *
 * Copyright 2003 VMware, Inc.
 * Copyright 2009 VMware, Inc.
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "arrayobj.h"
#include "util/glheader.h"
#include "c99_alloca.h"
#include "context.h"
#include "state.h"
#include "draw.h"
#include "draw_validate.h"
#include "dispatch.h"
#include "varray.h"
#include "bufferobj.h"
#include "enums.h"
#include "util/bitscan.h"
#include "compiler/nir/nir.h"
#include "compiler/nir/nir_serialize.h"
#include "util/blob.h"
#include "macros.h"
#include "transformfeedback.h"
#include "pipe/p_state.h"
#include "api_exec_decl.h"
#include "glthread_marshal.h"

#include "cso_cache/cso_context.h"
#include "state_tracker/st_context.h"
#include "state_tracker/st_draw.h"
#include "util/u_draw.h"
#include "util/u_threaded_context.h"
#include "state_tracker/st_cb_readpixels.h"

typedef struct {
   GLuint count;
   GLuint primCount;
   GLuint first;
   GLuint baseInstance;
} DrawArraysIndirectCommand;

typedef struct {
   GLuint count;
   GLuint primCount;
   GLuint firstIndex;
   GLint  baseVertex;
   GLuint baseInstance;
} DrawElementsIndirectCommand;

/**
 * Simple counter for unique filenames
 */
static unsigned dump_counter = 0;

/**
 * SPIRV cache for associating with draw calls
 */
static struct {
   char stage_name[64];
   void *spirv_data;
   size_t spirv_size;
} cached_spirv[MESA_SHADER_STAGES] = {0};

/* Cached NIR data for correlation with SPIRV */
static struct {
   char stage_name[64];
   struct nir_shader *nir;
} cached_nir[MESA_SHADER_STAGES] = {0};

/* Forward declarations */
static void dump_spirv_shader(const char *stage_name, unsigned draw_id, const void *spirv_data, size_t spirv_size);
static void dump_nir_shader(struct nir_shader *nir, const char *stage_name, unsigned draw_id);
static void dump_active_texture_contents(struct gl_context *ctx, unsigned draw_id);

/* Initialize the SPIRV dump hook - call this once */
static void
init_spirv_dump_hook(void)
{
   static bool initialized = false;
   if (!initialized) {
      zink_enable_spirv_dumping(true);
      initialized = true;
   }
}

/**
 * Global hook for SPIRV dumping - called from Zink driver
 */
void _mesa_dump_spirv_hook(const char *stage_name, const void *spirv_data, size_t spirv_size)
{
   if (!spirv_data || spirv_size == 0 || !stage_name) return;
   
   /* Find the appropriate slot for this shader stage */
   int slot = -1;
   if (strstr(stage_name, "vertex")) slot = MESA_SHADER_VERTEX;
   else if (strstr(stage_name, "fragment")) slot = MESA_SHADER_FRAGMENT;
   else if (strstr(stage_name, "geometry")) slot = MESA_SHADER_GEOMETRY;
   else if (strstr(stage_name, "tess_ctrl")) slot = MESA_SHADER_TESS_CTRL;
   else if (strstr(stage_name, "tess_eval")) slot = MESA_SHADER_TESS_EVAL;
   else if (strstr(stage_name, "compute")) slot = MESA_SHADER_COMPUTE;
   
   if (slot >= 0 && slot < MESA_SHADER_STAGES) {
      /* Free any existing cached data */
      if (cached_spirv[slot].spirv_data) {
         free(cached_spirv[slot].spirv_data);
      }
      
      /* Cache the new SPIRV data */
      cached_spirv[slot].spirv_data = malloc(spirv_size);
      if (cached_spirv[slot].spirv_data) {
         memcpy(cached_spirv[slot].spirv_data, spirv_data, spirv_size);
         cached_spirv[slot].spirv_size = spirv_size;
         strncpy(cached_spirv[slot].stage_name, stage_name, sizeof(cached_spirv[slot].stage_name) - 1);
         cached_spirv[slot].stage_name[sizeof(cached_spirv[slot].stage_name) - 1] = '\0';
      }
   }
   
   /* Also dump immediately for debugging */
   dump_spirv_shader(stage_name, dump_counter, spirv_data, spirv_size);
}

/**
 * Global hook for NIR dumping - called from Zink driver
 */
void _mesa_dump_nir_hook(const char *stage_name, struct nir_shader *nir)
{
   if (!nir || !stage_name) return;
   
   /* Find the appropriate slot for this shader stage */
   int slot = -1;
   if (strstr(stage_name, "vertex")) slot = MESA_SHADER_VERTEX;
   else if (strstr(stage_name, "fragment")) slot = MESA_SHADER_FRAGMENT;
   else if (strstr(stage_name, "geometry")) slot = MESA_SHADER_GEOMETRY;
   else if (strstr(stage_name, "tess_ctrl")) slot = MESA_SHADER_TESS_CTRL;
   else if (strstr(stage_name, "tess_eval")) slot = MESA_SHADER_TESS_EVAL;
   else if (strstr(stage_name, "compute")) slot = MESA_SHADER_COMPUTE;
   
   if (slot >= 0 && slot < MESA_SHADER_STAGES) {
      /* Cache the NIR reference (don't deep copy, just reference) */
      cached_nir[slot].nir = nir;
      strncpy(cached_nir[slot].stage_name, stage_name, sizeof(cached_nir[slot].stage_name) - 1);
      cached_nir[slot].stage_name[sizeof(cached_nir[slot].stage_name) - 1] = '\0';
   }
   
   /* Immediately dump the NIR since we have it fresh */
   dump_nir_shader(nir, stage_name, dump_counter);
}

/**
 * Dump buffer object data to file
 */
static void
dump_buffer_object(struct gl_context *ctx, struct gl_buffer_object *bufObj, const char *name, unsigned draw_id)
{
   if (!bufObj || !bufObj->Size || bufObj->Size > 10*1024*1024) /* Skip huge buffers */
      return;
      
   char filename[512];
   snprintf(filename, sizeof(filename), "c:\\temp\\draw_%06u_%s_buf%u_%lubytes.raw", 
            draw_id, name, bufObj->Name, (unsigned long)bufObj->Size);
   
   FILE *fp = fopen(filename, "wb");
   if (!fp) return;
   
   /* Map and dump buffer data */
   void *data = _mesa_bufferobj_map_range(ctx, 0, bufObj->Size, GL_MAP_READ_BIT, bufObj, MAP_INTERNAL);
   if (data) {
      fwrite(data, 1, bufObj->Size, fp);
      _mesa_bufferobj_unmap(ctx, bufObj, MAP_INTERNAL);
   }
   fclose(fp);
}

/**
 * Dump vertex array object state
 */
static void
dump_vertex_arrays(struct gl_context *ctx, unsigned draw_id)
{
   struct gl_vertex_array_object *vao = ctx->Array.VAO;
   if (!vao) return;
   
   char filename[512];
   snprintf(filename, sizeof(filename), "c:\\temp\\draw_%06u_vao_state.txt", draw_id);
   
   FILE *fp = fopen(filename, "w");
   if (!fp) return;
   
   fprintf(fp, "VAO Name: %u\n", vao->Name);
   fprintf(fp, "Enabled Attributes: 0x%llx\n", (unsigned long long)vao->Enabled);
   
   GLbitfield mask = vao->Enabled;
   while (mask) {
      const gl_vert_attrib i = u_bit_scan(&mask);
      const struct gl_array_attributes *array = &vao->VertexAttrib[i];
      const struct gl_vertex_buffer_binding *binding = &vao->BufferBinding[array->BufferBindingIndex];
      
      fprintf(fp, "Attr[%d]: Size=%d, Type=0x%x, Stride=%d, Offset=%ld, Buffer=%u\n",
              i, array->Format.Size, array->Format.Type, 
              binding->Stride, (long)array->RelativeOffset, 
              binding->BufferObj ? binding->BufferObj->Name : 0);
              
      /* Dump the vertex buffer data */
      if (binding->BufferObj) {
         char buf_name[64];
         snprintf(buf_name, sizeof(buf_name), "vertex_attr%d", i);
         dump_buffer_object(ctx, binding->BufferObj, buf_name, draw_id);
      }
   }
   
   /* Dump index buffer if present */
   if (vao->IndexBufferObj) {
      fprintf(fp, "Index Buffer: %u\n", vao->IndexBufferObj->Name);
      dump_buffer_object(ctx, vao->IndexBufferObj, "index", draw_id);
   }
   
   fclose(fp);
}

/**
 * Dump uniform buffer bindings and data
 */
static void
dump_uniform_buffers(struct gl_context *ctx, unsigned draw_id)
{
   char filename[512];
   snprintf(filename, sizeof(filename), "c:\\temp\\draw_%06u_uniforms.txt", draw_id);
   
   FILE *fp = fopen(filename, "w");
   if (!fp) return;
   
   fprintf(fp, "=== UNIFORM BUFFER BINDINGS ===\n");
   
   for (GLuint i = 0; i < ctx->Const.MaxUniformBufferBindings; i++) {
      struct gl_buffer_object *bufObj = ctx->UniformBufferBindings[i].BufferObject;
      if (!bufObj) continue;
      
      fprintf(fp, "UBO[%u]: Buffer=%u, Offset=%ld, Size=%ld\n", 
              i, bufObj->Name, 
              (long)ctx->UniformBufferBindings[i].Offset,
              (long)ctx->UniformBufferBindings[i].Size);
              
      /* Dump the uniform buffer data */
      char buf_name[64];
      snprintf(buf_name, sizeof(buf_name), "uniform%u", i);
      dump_buffer_object(ctx, bufObj, buf_name, draw_id);
   }
   
   fclose(fp);
}

/**
 * Dump NIR shader for a specific stage
 */
static void
dump_nir_shader(struct nir_shader *nir, const char *stage_name, unsigned draw_id)
{
   if (!nir) return;
   
   char filename[512];
   snprintf(filename, sizeof(filename), "c:\\temp\\draw_%06u_nir_%s.txt", draw_id, stage_name);
   
   FILE *fp = fopen(filename, "w");
   if (!fp) return;
   
   fprintf(fp, "=== NIR SHADER: %s ===\n", stage_name);
   fprintf(fp, "Draw ID: %u\n", draw_id);
   fprintf(fp, "Stage: %s\n", _mesa_shader_stage_to_string(nir->info.stage));
   fprintf(fp, "Name: %s\n", nir->info.name ? nir->info.name : "unknown");
   fprintf(fp, "Work Group Size: %ux%ux%u\n", 
           nir->info.workgroup_size[0], nir->info.workgroup_size[1], nir->info.workgroup_size[2]);
   fprintf(fp, "Num Inputs: %u\n", nir->num_inputs);
   fprintf(fp, "Num Outputs: %u\n", nir->num_outputs);
   fprintf(fp, "Num Uniforms: %u\n", nir->num_uniforms);
   fprintf(fp, "SPIRV File: draw_%06u_spirv_%s.spv\n", draw_id, stage_name);
   fprintf(fp, "SPIRV Info: draw_%06u_spirv_%s.txt\n", draw_id, stage_name);
   fprintf(fp, "\n=== NIR CODE ===\n");
   
   nir_print_shader(nir, fp);
   fclose(fp);
   
   /* Also dump binary NIR */
   snprintf(filename, sizeof(filename), "c:\\temp\\draw_%06u_nir_%s.bin", draw_id, stage_name);
   FILE *bin_fp = fopen(filename, "wb");
   if (bin_fp) {
      struct blob blob;
      blob_init(&blob);
      nir_serialize(&blob, nir, false);
      fwrite(blob.data, 1, blob.size, bin_fp);
      blob_finish(&blob);
      fclose(bin_fp);
   }
}

/**
 * Dump SPIRV shader data if available (placeholder for Zink integration)
 */
static void
dump_spirv_shader(const char *stage_name, unsigned draw_id, const void *spirv_data, size_t spirv_size)
{
   if (!spirv_data || spirv_size == 0) return;
   
   char filename[512];
   snprintf(filename, sizeof(filename), "c:\\temp\\draw_%06u_spirv_%s.spv", draw_id, stage_name);
   
   FILE *fp = fopen(filename, "wb");
   if (!fp) return;
   
   fwrite(spirv_data, 1, spirv_size, fp);
   fclose(fp);
   
   /* Create a text file with SPIRV info */
   snprintf(filename, sizeof(filename), "c:\\temp\\draw_%06u_spirv_%s.txt", draw_id, stage_name);
   fp = fopen(filename, "w");
   if (fp) {
      fprintf(fp, "=== SPIRV SHADER: %s ===\n", stage_name);
      fprintf(fp, "Draw ID: %u\n", draw_id);
      fprintf(fp, "Binary Size: %zu bytes\n", spirv_size);
      fprintf(fp, "Word Count: %zu\n", spirv_size / 4);
      fprintf(fp, "Magic Number: 0x%08X\n", spirv_size >= 4 ? *((const uint32_t*)spirv_data) : 0);
      fprintf(fp, "NIR Source: draw_%06u_nir_%s.txt\n", draw_id, stage_name);
      fprintf(fp, "NIR Binary: draw_%06u_nir_%s.bin\n", draw_id, stage_name);
      fprintf(fp, "\n=== SPIRV BINARY DUMP ===\n");
      
      /* Dump first 64 words as hex */
      const uint32_t *words = (const uint32_t*)spirv_data;
      size_t word_count = (spirv_size / 4 < 64) ? spirv_size / 4 : 64;
      for (size_t i = 0; i < word_count; i++) {
         if (i % 8 == 0) fprintf(fp, "%04zx: ", i);
         fprintf(fp, "%08x ", words[i]);
         if (i % 8 == 7) fprintf(fp, "\n");
      }
      if (word_count % 8 != 0) fprintf(fp, "\n");
      if (spirv_size / 4 > 64) {
         fprintf(fp, "... (%zu more words)\n", spirv_size / 4 - 64);
      }
      
      fclose(fp);
   }
}

/**
 * Dump comprehensive shader program state including NIR and SPIRV
 */
static void
dump_shader_program(struct gl_context *ctx, unsigned draw_id)
{
   struct gl_program *vp = ctx->VertexProgram._Current;
   struct gl_program *fp = ctx->FragmentProgram._Current;
   
   char filename[512];
   snprintf(filename, sizeof(filename), "c:\\temp\\draw_%06u_shaders.txt", draw_id);
   
   FILE *fp_file = fopen(filename, "w");
   if (!fp_file) return;
   
   fprintf(fp_file, "=== SHADER PROGRAM DUMP %u ===\n", draw_id);
   
   if (ctx->_Shader && ctx->_Shader->ActiveProgram) {
      struct gl_shader_program *prog = ctx->_Shader->ActiveProgram;
      fprintf(fp_file, "Active Shader Program: %u\n", prog->Name);
      fprintf(fp_file, "Link Status: %d\n", prog->data ? prog->data->LinkStatus : -1);
      
      /* Dump linked shaders and their NIR */
      for (unsigned i = 0; i < MESA_SHADER_STAGES; i++) {
         if (prog->_LinkedShaders[i] && prog->_LinkedShaders[i]->Program) {
            struct gl_program *stage_prog = prog->_LinkedShaders[i]->Program;
            const char *stage_names[] = {"vertex", "tess_ctrl", "tess_eval", "geometry", "fragment", "compute"};
            const char *stage_name = (i < 6) ? stage_names[i] : "unknown";
            
            fprintf(fp_file, "Shader[%u]: Stage=%s, Program=%u, NIR=%s\n", 
                    i, stage_name, prog->Name, stage_prog->nir ? "available" : "null");
            
            /* Dump NIR if available */
            if (stage_prog->nir) {
               dump_nir_shader(stage_prog->nir, stage_name, draw_id);
            }
            
            /* Dump cached SPIRV if available */
            if (cached_spirv[i].spirv_data && cached_spirv[i].spirv_size > 0) {
               dump_spirv_shader(stage_name, draw_id, cached_spirv[i].spirv_data, cached_spirv[i].spirv_size);
            }
         }
      }
   }
   
   /* Dump current program NIR directly */
   if (vp) {
      fprintf(fp_file, "Vertex Program: ID=%u, NIR=%s\n", vp->Id, vp->nir ? "available" : "null");
      if (vp->nir) {
         dump_nir_shader(vp->nir, "vertex_current", draw_id);
      }
   }
   
   if (fp) {
      fprintf(fp_file, "Fragment Program: ID=%u, NIR=%s\n", fp->Id, fp->nir ? "available" : "null");
      if (fp->nir) {
         dump_nir_shader(fp->nir, "fragment_current", draw_id);
      }
   }
   
   /* Dump other pipeline stages */
   if (ctx->GeometryProgram._Current) {
      struct gl_program *gp = ctx->GeometryProgram._Current;
      fprintf(fp_file, "Geometry Program: ID=%u, NIR=%s\n", gp->Id, gp->nir ? "available" : "null");
      if (gp->nir) {
         dump_nir_shader(gp->nir, "geometry_current", draw_id);
      }
   }
   
   if (ctx->TessCtrlProgram._Current) {
      struct gl_program *tcp = ctx->TessCtrlProgram._Current;
      fprintf(fp_file, "Tess Control Program: ID=%u, NIR=%s\n", tcp->Id, tcp->nir ? "available" : "null");
      if (tcp->nir) {
         dump_nir_shader(tcp->nir, "tess_ctrl_current", draw_id);
      }
   }
   
   if (ctx->TessEvalProgram._Current) {
      struct gl_program *tep = ctx->TessEvalProgram._Current;
      fprintf(fp_file, "Tess Eval Program: ID=%u, NIR=%s\n", tep->Id, tep->nir ? "available" : "null");
      if (tep->nir) {
         dump_nir_shader(tep->nir, "tess_eval_current", draw_id);
      }
   }
   
   if (ctx->ComputeProgram._Current) {
      struct gl_program *cp = ctx->ComputeProgram._Current;
      fprintf(fp_file, "Compute Program: ID=%u, NIR=%s\n", cp->Id, cp->nir ? "available" : "null");
      if (cp->nir) {
         dump_nir_shader(cp->nir, "compute_current", draw_id);
      }
   }
   
   fclose(fp_file);
}

/**
 * Dump texture bindings and basic info
 */
static void
dump_texture_state(struct gl_context *ctx, unsigned draw_id)
{
   char filename[512];
   snprintf(filename, sizeof(filename), "c:\\temp\\draw_%06u_textures.txt", draw_id);
   
   FILE *fp = fopen(filename, "w");
   if (!fp) return;
   
   fprintf(fp, "=== TEXTURE BINDINGS ===\n");
   
   for (GLuint unit = 0; unit < ctx->Const.MaxCombinedTextureImageUnits; unit++) {
      for (GLuint target = 0; target < NUM_TEXTURE_TARGETS; target++) {
         struct gl_texture_object *texObj = ctx->Texture.Unit[unit].CurrentTex[target];
         if (!texObj || texObj->Name == 0) continue;
         
         fprintf(fp, "Unit[%u] Target[%u]: Tex=%u, Target=0x%x, Size=%ux%u, Format=0x%x\n",
                 unit, target, texObj->Name, texObj->Target,
                 texObj->Image[0][0] ? texObj->Image[0][0]->Width : 0,
                 texObj->Image[0][0] ? texObj->Image[0][0]->Height : 0,
                 texObj->Image[0][0] ? texObj->Image[0][0]->InternalFormat : 0);
      }
   }
   
   fclose(fp);
}

/**
 * Dump active texture contents as PPM images
 */
static void
dump_active_texture_contents(struct gl_context *ctx, unsigned draw_id)
{
   for (GLuint unit = 0; unit < ctx->Const.MaxCombinedTextureImageUnits; unit++) {
      for (GLuint target = 0; target < NUM_TEXTURE_TARGETS; target++) {
         struct gl_texture_object *texObj = ctx->Texture.Unit[unit].CurrentTex[target];
         if (!texObj || texObj->Name == 0) continue;
         
         struct gl_texture_image *texImage = texObj->Image[0][0];
         if (!texImage || texImage->Width == 0 || texImage->Height == 0) continue;
         
         GLint width = texImage->Width;
         GLint height = texImage->Height;
         
         /* Skip depth/stencil textures to avoid complications */
         if (texImage->InternalFormat == GL_DEPTH_COMPONENT ||
             texImage->InternalFormat == GL_DEPTH_STENCIL ||
             texImage->InternalFormat == GL_STENCIL_INDEX) continue;
         
         /* Determine if this is an integer texture */
         bool is_integer = false;
         switch (texImage->InternalFormat) {
            case GL_R8UI: case GL_R8I: case GL_R16UI: case GL_R16I: 
            case GL_R32UI: case GL_R32I:
            case GL_RG8UI: case GL_RG8I: case GL_RG16UI: case GL_RG16I: 
            case GL_RG32UI: case GL_RG32I:
            case GL_RGB8UI: case GL_RGB8I: case GL_RGB16UI: case GL_RGB16I: 
            case GL_RGB32UI: case GL_RGB32I:
            case GL_RGBA8UI: case GL_RGBA8I: case GL_RGBA16UI: case GL_RGBA16I: 
            case GL_RGBA32UI: case GL_RGBA32I:
               is_integer = true;
               break;
            default:
               is_integer = false;
               break;
         }
         
         /* Allocate pixel buffer - use 16 bytes per pixel for integer textures */
         void *pixels = malloc(width * height * (is_integer ? 16 : 4));
         if (!pixels) continue;
         
         /* Save current bindings */
         GLint prev_fbo;
         _mesa_GetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
         
         GLuint temp_fbo, temp_texture;
         _mesa_GenFramebuffers(1, &temp_fbo);
         _mesa_BindFramebuffer(GL_FRAMEBUFFER, temp_fbo);
         
         /* For cube maps and arrays, just read face 0/layer 0 */
         GLenum attachment_target = texObj->Target;
         if (attachment_target == GL_TEXTURE_CUBE_MAP) {
            attachment_target = GL_TEXTURE_CUBE_MAP_POSITIVE_X;
         }
         
         _mesa_FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, 
                                   attachment_target, texObj->Name, 0);
         
         if (_mesa_CheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
            struct gl_pixelstore_attrib pack = ctx->DefaultPacking;
            
            bool read_success = false;
            if (is_integer) {
               /* For integer textures, read as RGBA_INTEGER to avoid blitter assertion */
               st_ReadPixels(ctx, 0, 0, width, height, GL_RGBA_INTEGER, GL_UNSIGNED_INT, &pack, pixels);
               read_success = true;
            } else {
               /* For normalized textures, read as regular RGBA */
               st_ReadPixels(ctx, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, &pack, pixels);
               read_success = true;
            }
            
            if (read_success) {
               /* Write raw data file for all textures */
               char raw_filename[512];
               snprintf(raw_filename, sizeof(raw_filename), 
                       "c:\\temp\\draw_%06u_texture_unit%u_target%u_%ux%u_%s.raw", 
                       draw_id, unit, target, width, height, is_integer ? "int" : "norm");
               
               FILE *raw_fp = fopen(raw_filename, "wb");
               if (raw_fp) {
                  fwrite(pixels, 1, width * height * (is_integer ? 16 : 4), raw_fp);
                  fclose(raw_fp);
               }
               
               /* For normalized textures, also write PPM */
               if (!is_integer) {
                  /* Write PPM file */
                  char ppm_filename[512];
                  snprintf(ppm_filename, sizeof(ppm_filename), 
                        "c:\\temp\\draw_%06u_texture_unit%u_target%u_%ux%u.ppm", 
                        draw_id, unit, target, width, height);
                  
                  FILE *ppm_fp = fopen(ppm_filename, "wb");
                  if (ppm_fp) {
                     fprintf(ppm_fp, "P6\n%d %d\n255\n", width, height);
                     GLubyte *byte_pixels = (GLubyte*)pixels;
                     for (int y = height - 1; y >= 0; y--) {
                        for (int x = 0; x < width; x++) {
                           GLubyte *pixel = &byte_pixels[(y * width + x) * 4];
                           fwrite(pixel, 1, 3, ppm_fp);
                        }
                     }
                     fclose(ppm_fp);
                  }               
               }
            }
         }
         
         /* Restore previous framebuffer */
         _mesa_BindFramebuffer(GL_FRAMEBUFFER, prev_fbo);
         _mesa_DeleteFramebuffers(1, &temp_fbo);
         
         free(pixels);
      }
   }
}

/**
 * Dump framebuffer configuration
 */
static void
dump_framebuffer_config(struct gl_context *ctx, unsigned draw_id)
{
   char filename[512];
   snprintf(filename, sizeof(filename), "c:\\temp\\draw_%06u_framebuffer.txt", draw_id);
   
   FILE *fp = fopen(filename, "w");
   if (!fp) return;
   
   struct gl_framebuffer *fb = ctx->DrawBuffer;
   fprintf(fp, "=== DRAW FRAMEBUFFER ===\n");
   fprintf(fp, "FBO Name: %u\n", fb->Name);
   fprintf(fp, "Size: %ux%u\n", fb->Width, fb->Height);
   fprintf(fp, "Status: 0x%x\n", fb->_Status);
   fprintf(fp, "Has Attachments: %s\n", fb->_HasAttachments ? "true" : "false");
   
   /* Dump color attachments */
   for (int i = 0; i < MAX_DRAW_BUFFERS; i++) {
      struct gl_renderbuffer_attachment *att = &fb->Attachment[BUFFER_COLOR0 + i];
      if (att->Type != GL_NONE) {
         fprintf(fp, "Color[%d]: Type=0x%x", i, att->Type);
         if (att->Type == GL_TEXTURE) {
            fprintf(fp, " Tex=%u Level=%u Face=%u", 
                    att->Texture ? att->Texture->Name : 0, 
                    att->TextureLevel, att->CubeMapFace);
         } else if (att->Type == GL_RENDERBUFFER) {
            fprintf(fp, " RB=%u", att->Renderbuffer ? att->Renderbuffer->Name : 0);
         }
         fprintf(fp, "\n");
      }
   }
   
   /* Dump depth/stencil attachments */
   struct gl_renderbuffer_attachment *depth_att = &fb->Attachment[BUFFER_DEPTH];
   if (depth_att->Type != GL_NONE) {
      fprintf(fp, "Depth: Type=0x%x", depth_att->Type);
      if (depth_att->Type == GL_TEXTURE && depth_att->Texture) {
         fprintf(fp, " Tex=%u Level=%u", depth_att->Texture->Name, depth_att->TextureLevel);
      }
      fprintf(fp, "\n");
   }
   
   fclose(fp);
}

/**
 * Dump additional GL pipeline state
 */
static void
dump_pipeline_state(struct gl_context *ctx, unsigned draw_id)
{
   char filename[512];
   snprintf(filename, sizeof(filename), "c:\\temp\\draw_%06u_pipeline_state.txt", draw_id);
   
   FILE *fp = fopen(filename, "w");
   if (!fp) return;
   
   fprintf(fp, "=== PIPELINE STATE ===\n");
   
   /* Viewport state */
   fprintf(fp, "Viewport[0]: X=%.1f Y=%.1f W=%.1f H=%.1f Near=%.6f Far=%.6f\n",
           ctx->ViewportArray[0].X, ctx->ViewportArray[0].Y,
           ctx->ViewportArray[0].Width, ctx->ViewportArray[0].Height,
           ctx->ViewportArray[0].Near, ctx->ViewportArray[0].Far);
   
   /* Blend state */
   fprintf(fp, "Blend Enabled: %s\n", ctx->Color.BlendEnabled ? "true" : "false");
   if (ctx->Color.BlendEnabled) {
      fprintf(fp, "Blend Src RGB: 0x%x, Dst RGB: 0x%x\n", 
              ctx->Color.Blend[0].SrcRGB, ctx->Color.Blend[0].DstRGB);
      fprintf(fp, "Blend Src Alpha: 0x%x, Dst Alpha: 0x%x\n",
              ctx->Color.Blend[0].SrcA, ctx->Color.Blend[0].DstA);
   }
   
   /* Depth state */
   fprintf(fp, "Depth Test: %s, Depth Func: 0x%x\n", 
           ctx->Depth.Test ? "true" : "false", ctx->Depth.Func);
   fprintf(fp, "Depth Mask: %s\n", ctx->Depth.Mask ? "true" : "false");
   
   /* Stencil state */
   fprintf(fp, "Stencil Test: %s\n", ctx->Stencil.Enabled ? "true" : "false");
   
   /* Rasterizer state */
   fprintf(fp, "Cull Face: %s, Cull Mode: 0x%x, Front Face: 0x%x\n",
           ctx->Polygon.CullFlag ? "true" : "false",
           ctx->Polygon.CullFaceMode, ctx->Polygon.FrontFace);
   
   /* Color mask */
   fprintf(fp, "Color Mask: 0x%x\n", ctx->Color.ColorMask);
   
   fclose(fp);
}

/**
 * Comprehensive RenderDoc-style dump of all GL state and buffers
 */
static void
dump_comprehensive_state(struct gl_context *ctx, GLenum mode, GLint start, GLsizei count, GLuint numInstances, GLuint baseInstance)
{
   /* Only dump if we recently had a uniform buffer update */
   if (!ctx->_UniformBufferDataUpdated)
      return;

   /* Initialize SPIRV dump hook on first use */
   init_spirv_dump_hook();

   unsigned current_dump = ++dump_counter;
   
   /* Create main state file */
   char main_filename[512];
   snprintf(main_filename, sizeof(main_filename), "c:\\temp\\draw_%06u_main_state.txt", current_dump);
   
   FILE *main_fp = fopen(main_filename, "w");
   if (main_fp) {
      fprintf(main_fp, "=== MESA DRAW CALL DUMP %u ===\n", current_dump);
      fprintf(main_fp, "Draw Mode: 0x%x (%s)\n", mode, _mesa_enum_to_string(mode));
      fprintf(main_fp, "First Vertex: %d\n", start);
      fprintf(main_fp, "Vertex Count: %d\n", count);
      fprintf(main_fp, "Instance Count: %u\n", numInstances);
      fprintf(main_fp, "Base Instance: %u\n", baseInstance);
      fprintf(main_fp, "Current Program: %u\n", 
              ctx->_Shader && ctx->_Shader->ActiveProgram ? ctx->_Shader->ActiveProgram->Name : 0);
      fprintf(main_fp, "Framebuffer: %u (%ux%u)\n", 
              ctx->DrawBuffer->Name, ctx->DrawBuffer->Width, ctx->DrawBuffer->Height);
      fclose(main_fp);
   }
   
   /* Dump all the different state categories */
   dump_vertex_arrays(ctx, current_dump);
   dump_uniform_buffers(ctx, current_dump);
   dump_shader_program(ctx, current_dump);
   dump_texture_state(ctx, current_dump);
   dump_active_texture_contents(ctx, current_dump);
   dump_framebuffer_config(ctx, current_dump);
   dump_pipeline_state(ctx, current_dump);
   
   /* Dump framebuffer contents */
   GLint width = (GLint)ctx->DrawBuffer->Width;
   GLint height = (GLint)ctx->DrawBuffer->Height;
   
   if (width > 0 && height > 0) {
      GLubyte *pixels = malloc(width * height * 4);
      if (pixels) {
         struct gl_pixelstore_attrib pack = ctx->DefaultPacking;
         st_ReadPixels(ctx, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, &pack, pixels);
         
         /* Write raw framebuffer data */
         char fb_filename[512];
         snprintf(fb_filename, sizeof(fb_filename), "c:\\temp\\draw_%06u_framebuffer_%dx%d.raw", 
                 current_dump, width, height);
         
         FILE *fb_fp = fopen(fb_filename, "wb");
         if (fb_fp) {
            fwrite(pixels, 1, width * height * 4, fb_fp);
            fclose(fb_fp);
         }
         
         /* Write PPM for easy viewing */
         char ppm_filename[512];
         snprintf(ppm_filename, sizeof(ppm_filename), "c:\\temp\\draw_%06u_framebuffer_%dx%d.ppm", 
                 current_dump, width, height);
         
         FILE *ppm_fp = fopen(ppm_filename, "wb");
         if (ppm_fp) {
            fprintf(ppm_fp, "P6\n%d %d\n255\n", width, height);
            for (int y = height - 1; y >= 0; y--) {
               for (int x = 0; x < width; x++) {
                  GLubyte *pixel = &pixels[(y * width + x) * 4];
                  fwrite(pixel, 1, 3, ppm_fp);
               }
            }
            fclose(ppm_fp);
         }
         
         free(pixels);
      }
   }
}

/**
 * Want to figure out which fragment program inputs are actually
 * constant/current values from ctx->Current.  These should be
 * referenced as a tracked state variable rather than a fragment
 * program input, to save the overhead of putting a constant value in
 * every submitted vertex, transferring it to hardware, interpolating
 * it across the triangle, etc...
 *
 * When there is a VP bound, just use vp->outputs.  But when we're
 * generating vp from fixed function state, basically want to
 * calculate:
 *
 * vp_out_2_fp_in( vp_in_2_vp_out( varying_inputs ) |
 *                 potential_vp_outputs )
 *
 * Where potential_vp_outputs is calculated by looking at enabled
 * texgen, etc.
 *
 * The generated fragment program should then only declare inputs that
 * may vary or otherwise differ from the ctx->Current values.
 * Otherwise, the fp should track them as state values instead.
 */
void
_mesa_set_varying_vp_inputs(struct gl_context *ctx, GLbitfield varying_inputs)
{
   if (ctx->VertexProgram._VPModeOptimizesConstantAttribs &&
       ctx->VertexProgram._VaryingInputs != varying_inputs) {
      ctx->VertexProgram._VaryingInputs = varying_inputs;
      ctx->NewState |= _NEW_FF_VERT_PROGRAM | _NEW_FF_FRAG_PROGRAM;
   }
}


/**
 * Set the _DrawVAO and the net enabled arrays.
 * The vao->_Enabled bitmask is transformed due to position/generic0
 * as stored in vao->_AttributeMapMode. Then the filter bitmask is applied
 * to filter out arrays unwanted for the currently executed draw operation.
 * For example, the generic attributes are masked out form the _DrawVAO's
 * enabled arrays when a fixed function array draw is executed.
 */
void
_mesa_set_draw_vao(struct gl_context *ctx, struct gl_vertex_array_object *vao)
{
   struct gl_vertex_array_object **ptr = &ctx->Array._DrawVAO;

   if (*ptr != vao) {
      _mesa_reference_vao_(ctx, ptr, vao);
      _mesa_update_edgeflag_state_vao(ctx);
      ST_SET_STATE(ctx->NewDriverState, ST_NEW_VERTEX_ARRAYS);
      ctx->Array.NewVertexElements = true;
   }
}

/**
 * Other than setting the new VAO, this returns a VAO reference to
 * the previously-bound VAO and the previous _VPModeInputFilter value through
 * parameters. The caller must call _mesa_restore_draw_vao to ensure
 * reference counting is done properly and the affected states are restored.
 *
 * \param ctx  GL context
 * \param vao  VAO to set.
 * \param vp_input_filter  Mask of enabled vertex attribs.
 *        Possible values that can also be OR'd with each other:
 *        - VERT_BIT_FF_ALL
 *        - VERT_BIT_MAT_ALL
 *        - VERT_BIT_ALL
 *        - VERT_BIT_SELECT_RESULT_OFFSET
 * \param old_vao  Previous bound VAO.
 * \param old_vp_input_filter  Previous value of vp_input_filter.
 */
void
_mesa_save_and_set_draw_vao(struct gl_context *ctx,
                            struct gl_vertex_array_object *vao,
                            GLbitfield vp_input_filter,
                            struct gl_vertex_array_object **old_vao,
                            GLbitfield *old_vp_input_filter)
{
   *old_vao = ctx->Array._DrawVAO;
   *old_vp_input_filter = ctx->VertexProgram._VPModeInputFilter;

   ctx->Array._DrawVAO = NULL;
   ctx->VertexProgram._VPModeInputFilter = vp_input_filter;
   _mesa_set_draw_vao(ctx, vao);
}

void
_mesa_restore_draw_vao(struct gl_context *ctx,
                       struct gl_vertex_array_object *saved,
                       GLbitfield saved_vp_input_filter)
{
   /* Restore states. */
   _mesa_reference_vao(ctx, &ctx->Array._DrawVAO, NULL);
   ctx->Array._DrawVAO = saved;
   ctx->VertexProgram._VPModeInputFilter = saved_vp_input_filter;

   /* Update states. */
   ST_SET_STATE(ctx->NewDriverState, ST_NEW_VERTEX_ARRAYS);
   ctx->Array.NewVertexElements = true;

   /* Restore original states. */
   _mesa_update_edgeflag_state_vao(ctx);
}

/**
 * Is 'mode' a valid value for glBegin(), glDrawArrays(), glDrawElements(),
 * etc?  Also, do additional checking related to transformation feedback.
 * Note: this function cannot be called during glNewList(GL_COMPILE) because
 * this code depends on current transform feedback state.
 * Also, do additional checking related to tessellation shaders.
 */
static GLenum
valid_prim_mode_custom(struct gl_context *ctx, GLenum mode,
                       GLbitfield valid_prim_mask)
{
#if MESA_DEBUG
   ASSERTED unsigned mask = ctx->ValidPrimMask;
   ASSERTED unsigned mask_indexed = ctx->ValidPrimMaskIndexed;
   ASSERTED bool drawpix_valid = ctx->DrawPixValid;
   _mesa_update_valid_to_render_state(ctx);
   assert(mask == ctx->ValidPrimMask &&
          mask_indexed == ctx->ValidPrimMaskIndexed &&
          drawpix_valid == ctx->DrawPixValid);
#endif

   /* All primitive type enums are less than 32, so we can use the shift. */
   if (mode >= 32 || !((1u << mode) & valid_prim_mask)) {
      /* If the primitive type is not in SupportedPrimMask, set GL_INVALID_ENUM,
       * else set DrawGLError (e.g. GL_INVALID_OPERATION).
       */
      return mode >= 32 || !((1u << mode) & ctx->SupportedPrimMask) ?
               GL_INVALID_ENUM : ctx->DrawGLError;
   }

   return GL_NO_ERROR;
}

GLenum
_mesa_valid_prim_mode(struct gl_context *ctx, GLenum mode)
{
   return valid_prim_mode_custom(ctx, mode, ctx->ValidPrimMask);
}

static GLenum
valid_prim_mode_indexed(struct gl_context *ctx, GLenum mode)
{
   return valid_prim_mode_custom(ctx, mode, ctx->ValidPrimMaskIndexed);
}

/**
 * Verify that the element type is valid.
 *
 * Generates \c GL_INVALID_ENUM and returns \c false if it is not.
 */
static GLenum
valid_elements_type(struct gl_context *ctx, GLenum type)
{
   return _mesa_is_index_type_valid(type) ? GL_NO_ERROR : GL_INVALID_ENUM;
}

static inline bool
indices_aligned(unsigned index_size_shift, const GLvoid *indices)
{
   /* Require that indices are aligned to the element size. GL doesn't specify
    * an error for this, but the ES 3.0 spec says:
    *
    *    "Clients must align data elements consistently with the requirements
    *     of the client platform, with an additional base-level requirement
    *     that an offset within a buffer to a datum comprising N basic machine
    *     units be a multiple of N"
    *
    * This is only required by index buffers, not user indices.
    */
   return ((uintptr_t)indices & ((1 << index_size_shift) - 1)) == 0;
}

static GLenum
validate_DrawElements_common(struct gl_context *ctx, GLenum mode,
                             GLsizei count, GLsizei numInstances, GLenum type)
{
   if (count < 0 || numInstances < 0)
      return GL_INVALID_VALUE;

   GLenum error = valid_prim_mode_indexed(ctx, mode);
   if (error)
      return error;

   return valid_elements_type(ctx, type);
}

/**
 * Error checking for glDrawElements().  Includes parameter checking
 * and VBO bounds checking.
 * \return GL_TRUE if OK to render, GL_FALSE if error found
 */
static GLboolean
_mesa_validate_DrawElements(struct gl_context *ctx,
                            GLenum mode, GLsizei count, GLenum type)
{
   GLenum error = validate_DrawElements_common(ctx, mode, count, 1, type);
   if (error)
      _mesa_error(ctx, error, "glDrawElements");

   return !error;
}


/**
 * Error checking for glMultiDrawElements().  Includes parameter checking
 * and VBO bounds checking.
 * \return GL_TRUE if OK to render, GL_FALSE if error found
 */
static GLboolean
_mesa_validate_MultiDrawElements(struct gl_context *ctx,
                                 GLenum mode, const GLsizei *count,
                                 GLenum type, const GLvoid * const *indices,
                                 GLsizei primcount,
                                 struct gl_buffer_object *index_bo)
{
   GLenum error;

   /*
    * Section 2.3.1 (Errors) of the OpenGL 4.5 (Core Profile) spec says:
    *
    *    "If a negative number is provided where an argument of type sizei or
    *     sizeiptr is specified, an INVALID_VALUE error is generated."
    *
    * and in the same section:
    *
    *    "In other cases, there are no side effects unless otherwise noted;
    *     the command which generates the error is ignored so that it has no
    *     effect on GL state or framebuffer contents."
    *
    * Hence, check both primcount and all the count[i].
    */
   if (primcount < 0) {
      error = GL_INVALID_VALUE;
   } else {
      error = valid_prim_mode_indexed(ctx, mode);

      if (!error) {
         error = valid_elements_type(ctx, type);

         if (!error) {
            for (int i = 0; i < primcount; i++) {
               if (count[i] < 0) {
                  error = GL_INVALID_VALUE;
                  break;
               }
            }
         }
      }
   }

   if (error)
      _mesa_error(ctx, error, "glMultiDrawElements");

   /* Not using a VBO for indices, so avoid NULL pointer derefs later.
    */
   if (!index_bo) {
      for (int i = 0; i < primcount; i++) {
         if (!indices[i])
            return GL_FALSE;
      }
   }

   return !error;
}


/**
 * Error checking for glDrawRangeElements().  Includes parameter checking
 * and VBO bounds checking.
 * \return GL_TRUE if OK to render, GL_FALSE if error found
 */
static GLboolean
_mesa_validate_DrawRangeElements(struct gl_context *ctx, GLenum mode,
                                 GLuint start, GLuint end,
                                 GLsizei count, GLenum type)
{
   GLenum error;

   if (end < start) {
      error = GL_INVALID_VALUE;
   } else {
      error = validate_DrawElements_common(ctx, mode, count, 1, type);
   }

   if (error)
      _mesa_error(ctx, error, "glDrawRangeElements");

   return !error;
}


static bool
need_xfb_remaining_prims_check(const struct gl_context *ctx)
{
   /* From the GLES3 specification, section 2.14.2 (Transform Feedback
    * Primitive Capture):
    *
    *   The error INVALID_OPERATION is generated by DrawArrays and
    *   DrawArraysInstanced if recording the vertices of a primitive to the
    *   buffer objects being used for transform feedback purposes would result
    *   in either exceeding the limits of any buffer object’s size, or in
    *   exceeding the end position offset + size − 1, as set by
    *   BindBufferRange.
    *
    * This is in contrast to the behaviour of desktop GL, where the extra
    * primitives are silently dropped from the transform feedback buffer.
    *
    * This text is removed in ES 3.2, presumably because it's not really
    * implementable with geometry and tessellation shaders.  In fact,
    * the OES_geometry_shader spec says:
    *
    *    "(13) Does this extension change how transform feedback operates
    *     compared to unextended OpenGL ES 3.0 or 3.1?
    *
    *     RESOLVED: Yes. Because dynamic geometry amplification in a geometry
    *     shader can make it difficult if not impossible to predict the amount
    *     of geometry that may be generated in advance of executing the shader,
    *     the draw-time error for transform feedback buffer overflow conditions
    *     is removed and replaced with the GL behavior (primitives are not
    *     written and the corresponding counter is not updated)..."
    */
   return _mesa_is_gles3(ctx) && _mesa_is_xfb_active_and_unpaused(ctx) &&
          !_mesa_has_OES_geometry_shader(ctx) &&
          !_mesa_has_OES_tessellation_shader(ctx);
}


/**
 * Figure out the number of transform feedback primitives that will be output
 * considering the drawing mode, number of vertices, and instance count,
 * assuming that no geometry shading is done and primitive restart is not
 * used.
 *
 * This is used by driver back-ends in implementing the PRIMITIVES_GENERATED
 * and TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN queries.  It is also used to
 * pre-validate draw calls in GLES3 (where draw calls only succeed if there is
 * enough room in the transform feedback buffer for the result).
 */
static size_t
count_tessellated_primitives(GLenum mode, GLuint count, GLuint num_instances)
{
   size_t num_primitives;
   switch (mode) {
   case GL_POINTS:
      num_primitives = count;
      break;
   case GL_LINE_STRIP:
      num_primitives = count >= 2 ? count - 1 : 0;
      break;
   case GL_LINE_LOOP:
      num_primitives = count >= 2 ? count : 0;
      break;
   case GL_LINES:
      num_primitives = count / 2;
      break;
   case GL_TRIANGLE_STRIP:
   case GL_TRIANGLE_FAN:
   case GL_POLYGON:
      num_primitives = count >= 3 ? count - 2 : 0;
      break;
   case GL_TRIANGLES:
      num_primitives = count / 3;
      break;
   case GL_QUAD_STRIP:
      num_primitives = count >= 4 ? ((count / 2) - 1) * 2 : 0;
      break;
   case GL_QUADS:
      num_primitives = (count / 4) * 2;
      break;
   case GL_LINES_ADJACENCY:
      num_primitives = count / 4;
      break;
   case GL_LINE_STRIP_ADJACENCY:
      num_primitives = count >= 4 ? count - 3 : 0;
      break;
   case GL_TRIANGLES_ADJACENCY:
      num_primitives = count / 6;
      break;
   case GL_TRIANGLE_STRIP_ADJACENCY:
      num_primitives = count >= 6 ? (count - 4) / 2 : 0;
      break;
   default:
      assert(!"Unexpected primitive type in count_tessellated_primitives");
      num_primitives = 0;
      break;
   }
   return num_primitives * num_instances;
}


static GLenum
validate_draw_arrays(struct gl_context *ctx,
                     GLenum mode, GLsizei count, GLsizei numInstances)
{
   if (count < 0 || numInstances < 0)
      return GL_INVALID_VALUE;

   GLenum error = _mesa_valid_prim_mode(ctx, mode);
   if (error)
      return error;

   if (need_xfb_remaining_prims_check(ctx)) {
      struct gl_transform_feedback_object *xfb_obj
         = ctx->TransformFeedback.CurrentObject;
      size_t prim_count = count_tessellated_primitives(mode, count, numInstances);
      if (xfb_obj->GlesRemainingPrims < prim_count)
         return GL_INVALID_OPERATION;

      xfb_obj->GlesRemainingPrims -= prim_count;
   }

   return GL_NO_ERROR;
}

/**
 * Called from the tnl module to error check the function parameters and
 * verify that we really can draw something.
 * \return GL_TRUE if OK to render, GL_FALSE if error found
 */
static GLboolean
_mesa_validate_DrawArrays(struct gl_context *ctx, GLenum mode, GLsizei count)
{
   GLenum error = validate_draw_arrays(ctx, mode, count, 1);

   if (error)
      _mesa_error(ctx, error, "glDrawArrays");

   return !error;
}


static GLboolean
_mesa_validate_DrawArraysInstanced(struct gl_context *ctx, GLenum mode, GLint first,
                                   GLsizei count, GLsizei numInstances)
{
   GLenum error;

   if (first < 0) {
      error = GL_INVALID_VALUE;
   } else {
      error = validate_draw_arrays(ctx, mode, count, numInstances);
   }

   if (error)
      _mesa_error(ctx, error, "glDrawArraysInstanced");

   return !error;
}


/**
 * Called to error check the function parameters.
 *
 * Note that glMultiDrawArrays is not part of GLES, so there's limited scope
 * for sharing code with the validation of glDrawArrays.
 */
static bool
_mesa_validate_MultiDrawArrays(struct gl_context *ctx, GLenum mode,
                               const GLsizei *count, GLsizei primcount)
{
   GLenum error;

   if (primcount < 0) {
      error = GL_INVALID_VALUE;
   } else {
      error = _mesa_valid_prim_mode(ctx, mode);

      if (!error) {
         for (int i = 0; i < primcount; ++i) {
            if (count[i] < 0) {
               error = GL_INVALID_VALUE;
               break;
            }
         }

         if (!error) {
            if (need_xfb_remaining_prims_check(ctx)) {
               struct gl_transform_feedback_object *xfb_obj
                  = ctx->TransformFeedback.CurrentObject;
               size_t xfb_prim_count = 0;

               for (int i = 0; i < primcount; ++i) {
                  xfb_prim_count +=
                     count_tessellated_primitives(mode, count[i], 1);
               }

               if (xfb_obj->GlesRemainingPrims < xfb_prim_count) {
                  error = GL_INVALID_OPERATION;
               } else {
                  xfb_obj->GlesRemainingPrims -= xfb_prim_count;
               }
            }
         }
      }
   }

   if (error)
      _mesa_error(ctx, error, "glMultiDrawArrays");

   return !error;
}


static GLboolean
_mesa_validate_DrawElementsInstanced(struct gl_context *ctx,
                                     GLenum mode, GLsizei count, GLenum type,
                                     GLsizei numInstances)
{
   GLenum error =
      validate_DrawElements_common(ctx, mode, count, numInstances, type);

   if (error)
      _mesa_error(ctx, error, "glDrawElementsInstanced");

   return !error;
}


static GLboolean
_mesa_validate_DrawTransformFeedback(struct gl_context *ctx,
                                     GLenum mode,
                                     struct gl_transform_feedback_object *obj,
                                     GLuint stream,
                                     GLsizei numInstances)
{
   GLenum error;

   /* From the GL 4.5 specification, page 429:
    * "An INVALID_VALUE error is generated if id is not the name of a
    *  transform feedback object."
    */
   if (!obj || !obj->EverBound || stream >= ctx->Const.MaxVertexStreams ||
       numInstances < 0) {
      error = GL_INVALID_VALUE;
   } else {
      error = _mesa_valid_prim_mode(ctx, mode);

      if (!error) {
         if (!obj->EndedAnytime)
            error = GL_INVALID_OPERATION;
      }
   }

   if (error)
      _mesa_error(ctx, error, "glDrawTransformFeedback*");

   return !error;
}

static GLenum
valid_draw_indirect(struct gl_context *ctx,
                    GLenum mode, const GLvoid *indirect,
                    GLsizei size)
{
   const uint64_t end = (uint64_t) (uintptr_t) indirect + size;

   /* OpenGL ES 3.1 spec. section 10.5:
    *
    *      "DrawArraysIndirect requires that all data sourced for the
    *      command, including the DrawArraysIndirectCommand
    *      structure,  be in buffer objects,  and may not be called when
    *      the default vertex array object is bound."
    */
   if (!_mesa_is_desktop_gl_compat(ctx) &&
       ctx->Array.VAO == ctx->Array.DefaultVAO)
      return GL_INVALID_OPERATION;

   /* From OpenGL ES 3.1 spec. section 10.5:
    *     "An INVALID_OPERATION error is generated if zero is bound to
    *     VERTEX_ARRAY_BINDING, DRAW_INDIRECT_BUFFER or to any enabled
    *     vertex array."
    *
    * Here we check that for each enabled vertex array we have a vertex
    * buffer bound.
    */
   if (_mesa_is_gles31(ctx) &&
       ctx->Array.VAO->Enabled & ~ctx->Array.VAO->VertexAttribBufferMask)
      return GL_INVALID_OPERATION;

   GLenum error = _mesa_valid_prim_mode(ctx, mode);
   if (error)
      return error;

   /* OpenGL ES 3.1 specification, section 10.5:
    *
    *      "An INVALID_OPERATION error is generated if
    *      transform feedback is active and not paused."
    *
    * The OES_geometry_shader spec says:
    *
    *    On p. 250 in the errors section for the DrawArraysIndirect command,
    *    and on p. 254 in the errors section for the DrawElementsIndirect
    *    command, delete the errors which state:
    *
    *    "An INVALID_OPERATION error is generated if transform feedback is
    *    active and not paused."
    *
    *    (thus allowing transform feedback to work with indirect draw commands).
    */
   if (_mesa_is_gles31(ctx) && !ctx->Extensions.OES_geometry_shader &&
       _mesa_is_xfb_active_and_unpaused(ctx))
      return GL_INVALID_OPERATION;

   /* From OpenGL version 4.4. section 10.5
    * and OpenGL ES 3.1, section 10.6:
    *
    *      "An INVALID_VALUE error is generated if indirect is not a
    *       multiple of the size, in basic machine units, of uint."
    */
   if ((GLsizeiptr)indirect & (sizeof(GLuint) - 1))
      return GL_INVALID_VALUE;

   if (!ctx->DrawIndirectBuffer)
      return GL_INVALID_OPERATION;

   if (_mesa_check_disallowed_mapping(ctx->DrawIndirectBuffer))
      return GL_INVALID_OPERATION;

   /* From the ARB_draw_indirect specification:
    * "An INVALID_OPERATION error is generated if the commands source data
    *  beyond the end of the buffer object [...]"
    */
   if (ctx->DrawIndirectBuffer->Size < end)
      return GL_INVALID_OPERATION;

   return GL_NO_ERROR;
}

static inline GLenum
valid_draw_indirect_elements(struct gl_context *ctx,
                             GLenum mode, GLenum type, const GLvoid *indirect,
                             GLsizeiptr size)
{
   GLenum error = valid_elements_type(ctx, type);
   if (error)
      return error;

   /*
    * Unlike regular DrawElementsInstancedBaseVertex commands, the indices
    * may not come from a client array and must come from an index buffer.
    * If no element array buffer is bound, an INVALID_OPERATION error is
    * generated.
    */
   if (!ctx->Array.VAO->IndexBufferObj)
      return GL_INVALID_OPERATION;

   return valid_draw_indirect(ctx, mode, indirect, size);
}

static GLboolean
_mesa_valid_draw_indirect_multi(struct gl_context *ctx,
                                GLsizei primcount, GLsizei stride,
                                const char *name)
{

   /* From the ARB_multi_draw_indirect specification:
    * "INVALID_VALUE is generated by MultiDrawArraysIndirect or
    *  MultiDrawElementsIndirect if <primcount> is negative."
    *
    * "<primcount> must be positive, otherwise an INVALID_VALUE error will
    *  be generated."
    */
   if (primcount < 0) {
      _mesa_error(ctx, GL_INVALID_VALUE, "%s(primcount < 0)", name);
      return GL_FALSE;
   }


   /* From the ARB_multi_draw_indirect specification:
    * "<stride> must be a multiple of four, otherwise an INVALID_VALUE
    *  error is generated."
    */
   if (stride % 4) {
      _mesa_error(ctx, GL_INVALID_VALUE, "%s(stride %% 4)", name);
      return GL_FALSE;
   }

   return GL_TRUE;
}

static GLboolean
_mesa_validate_DrawArraysIndirect(struct gl_context *ctx,
                                  GLenum mode,
                                  const GLvoid *indirect)
{
   const unsigned drawArraysNumParams = 4;
   GLenum error =
      valid_draw_indirect(ctx, mode, indirect,
                          drawArraysNumParams * sizeof(GLuint));

   if (error)
      _mesa_error(ctx, error, "glDrawArraysIndirect");

   return !error;
}

static GLboolean
_mesa_validate_DrawElementsIndirect(struct gl_context *ctx,
                                    GLenum mode, GLenum type,
                                    const GLvoid *indirect)
{
   const unsigned drawElementsNumParams = 5;
   GLenum error = valid_draw_indirect_elements(ctx, mode, type, indirect,
                                               drawElementsNumParams *
                                               sizeof(GLuint));
   if (error)
      _mesa_error(ctx, error, "glDrawElementsIndirect");

   return !error;
}

static GLboolean
_mesa_validate_MultiDrawArraysIndirect(struct gl_context *ctx,
                                       GLenum mode,
                                       const GLvoid *indirect,
                                       GLsizei primcount, GLsizei stride)
{
   GLsizeiptr size = 0;
   const unsigned drawArraysNumParams = 4;

   /* caller has converted stride==0 to drawArraysNumParams * sizeof(GLuint) */
   assert(stride != 0);

   if (!_mesa_valid_draw_indirect_multi(ctx, primcount, stride,
                                        "glMultiDrawArraysIndirect"))
      return GL_FALSE;

   /* number of bytes of the indirect buffer which will be read */
   size = primcount
      ? (primcount - 1) * stride + drawArraysNumParams * sizeof(GLuint)
      : 0;

   GLenum error = valid_draw_indirect(ctx, mode, indirect, size);
   if (error)
      _mesa_error(ctx, error, "glMultiDrawArraysIndirect");

   return !error;
}

static GLboolean
_mesa_validate_MultiDrawElementsIndirect(struct gl_context *ctx,
                                         GLenum mode, GLenum type,
                                         const GLvoid *indirect,
                                         GLsizei primcount, GLsizei stride)
{
   GLsizeiptr size = 0;
   const unsigned drawElementsNumParams = 5;

   /* caller has converted stride==0 to drawElementsNumParams * sizeof(GLuint) */
   assert(stride != 0);

   if (!_mesa_valid_draw_indirect_multi(ctx, primcount, stride,
                                        "glMultiDrawElementsIndirect"))
      return GL_FALSE;

   /* number of bytes of the indirect buffer which will be read */
   size = primcount
      ? (primcount - 1) * stride + drawElementsNumParams * sizeof(GLuint)
      : 0;

   GLenum error = valid_draw_indirect_elements(ctx, mode, type, indirect,
                                               size);
   if (error)
      _mesa_error(ctx, error, "glMultiDrawElementsIndirect");

   return !error;
}

static GLenum
valid_draw_indirect_parameters(struct gl_context *ctx,
                               GLintptr drawcount)
{
   /* From the ARB_indirect_parameters specification:
    * "INVALID_VALUE is generated by MultiDrawArraysIndirectCountARB or
    *  MultiDrawElementsIndirectCountARB if <drawcount> is not a multiple of
    *  four."
    */
   if (drawcount & 3)
      return GL_INVALID_VALUE;

   /* From the ARB_indirect_parameters specification:
    * "INVALID_OPERATION is generated by MultiDrawArraysIndirectCountARB or
    *  MultiDrawElementsIndirectCountARB if no buffer is bound to the
    *  PARAMETER_BUFFER_ARB binding point."
    */
   if (!ctx->ParameterBuffer)
      return GL_INVALID_OPERATION;

   if (_mesa_check_disallowed_mapping(ctx->ParameterBuffer))
      return GL_INVALID_OPERATION;

   /* From the ARB_indirect_parameters specification:
    * "INVALID_OPERATION is generated by MultiDrawArraysIndirectCountARB or
    *  MultiDrawElementsIndirectCountARB if reading a <sizei> typed value
    *  from the buffer bound to the PARAMETER_BUFFER_ARB target at the offset
    *  specified by <drawcount> would result in an out-of-bounds access."
    */
   if (ctx->ParameterBuffer->Size < drawcount + sizeof(GLsizei))
      return GL_INVALID_OPERATION;

   return GL_NO_ERROR;
}

static GLboolean
_mesa_validate_MultiDrawArraysIndirectCount(struct gl_context *ctx,
                                            GLenum mode,
                                            GLintptr indirect,
                                            GLintptr drawcount,
                                            GLsizei maxdrawcount,
                                            GLsizei stride)
{
   GLsizeiptr size = 0;
   const unsigned drawArraysNumParams = 4;

   /* caller has converted stride==0 to drawArraysNumParams * sizeof(GLuint) */
   assert(stride != 0);

   if (!_mesa_valid_draw_indirect_multi(ctx, maxdrawcount, stride,
                                        "glMultiDrawArraysIndirectCountARB"))
      return GL_FALSE;

   /* number of bytes of the indirect buffer which will be read */
   size = maxdrawcount
      ? (maxdrawcount - 1) * stride + drawArraysNumParams * sizeof(GLuint)
      : 0;

   GLenum error = valid_draw_indirect(ctx, mode, (void *)indirect, size);
   if (!error)
      error = valid_draw_indirect_parameters(ctx, drawcount);

   if (error)
      _mesa_error(ctx, error, "glMultiDrawArraysIndirectCountARB");

   return !error;
}

static GLboolean
_mesa_validate_MultiDrawElementsIndirectCount(struct gl_context *ctx,
                                              GLenum mode, GLenum type,
                                              GLintptr indirect,
                                              GLintptr drawcount,
                                              GLsizei maxdrawcount,
                                              GLsizei stride)
{
   GLsizeiptr size = 0;
   const unsigned drawElementsNumParams = 5;

   /* caller has converted stride==0 to drawElementsNumParams * sizeof(GLuint) */
   assert(stride != 0);

   if (!_mesa_valid_draw_indirect_multi(ctx, maxdrawcount, stride,
                                        "glMultiDrawElementsIndirectCountARB"))
      return GL_FALSE;

   /* number of bytes of the indirect buffer which will be read */
   size = maxdrawcount
      ? (maxdrawcount - 1) * stride + drawElementsNumParams * sizeof(GLuint)
      : 0;

   GLenum error = valid_draw_indirect_elements(ctx, mode, type,
                                               (void *)indirect, size);
   if (!error)
      error = valid_draw_indirect_parameters(ctx, drawcount);

   if (error)
      _mesa_error(ctx, error, "glMultiDrawElementsIndirectCountARB");

   return !error;
}

static inline struct pipe_draw_start_count_bias *
get_temp_draws(struct gl_context *ctx, unsigned primcount)
{
   if (primcount > ctx->num_tmp_draws) {
      struct pipe_draw_start_count_bias *tmp =
         realloc(ctx->tmp_draws, primcount * sizeof(ctx->tmp_draws[0]));

      if (tmp) {
         ctx->tmp_draws = tmp;
         ctx->num_tmp_draws = primcount;
      } else {
         _mesa_error(ctx, GL_OUT_OF_MEMORY, "can't alloc tmp_draws");
         free(ctx->tmp_draws); /* realloc doesn't free on failure */
         ctx->tmp_draws = NULL;
         ctx->num_tmp_draws = 0;
      }
   }
   return ctx->tmp_draws;
}

/**
 * Check that element 'j' of the array has reasonable data.
 * Map VBO if needed.
 * For debugging purposes; not normally used.
 */
static void
check_array_data(struct gl_context *ctx, struct gl_vertex_array_object *vao,
                 GLuint attrib, GLuint j)
{
   const struct gl_array_attributes *array = &vao->VertexAttrib[attrib];
   if (vao->Enabled & VERT_BIT(attrib)) {
      const struct gl_vertex_buffer_binding *binding =
         &vao->BufferBinding[array->BufferBindingIndex];
      struct gl_buffer_object *bo = binding->BufferObj;
      const void *data = array->Ptr;
      if (bo) {
         data = ADD_POINTERS(_mesa_vertex_attrib_address(array, binding),
                             bo->Mappings[MAP_INTERNAL].Pointer);
      }
      switch (array->Format.User.Type) {
      case GL_FLOAT:
         {
            GLfloat *f = (GLfloat *) ((GLubyte *) data + binding->Stride * j);
            GLint k;
            for (k = 0; k < array->Format.User.Size; k++) {
               if (util_is_inf_or_nan(f[k]) || f[k] >= 1.0e20F || f[k] <= -1.0e10F) {
                  printf("Bad array data:\n");
                  printf("  Element[%u].%u = %f\n", j, k, f[k]);
                  printf("  Array %u at %p\n", attrib, (void *) array);
                  printf("  Type 0x%x, Size %d, Stride %d\n",
                         array->Format.User.Type, array->Format.User.Size,
                         binding->Stride);
                  printf("  Address/offset %p in Buffer Object %u\n",
                         array->Ptr, bo ? bo->Name : 0);
                  f[k] = 1.0F;  /* XXX replace the bad value! */
               }
               /*assert(!util_is_inf_or_nan(f[k])); */
            }
         }
         break;
      default:
         ;
      }
   }
}


/**
 * Examine the array's data for NaNs, etc.
 * For debug purposes; not normally used.
 */
static void
check_draw_elements_data(struct gl_context *ctx, GLsizei count,
                         GLenum elemType, const void *elements,
                         GLint basevertex)
{
   struct gl_vertex_array_object *vao = ctx->Array.VAO;
   GLint i;
   GLuint k;

   _mesa_vao_map(ctx, vao, GL_MAP_READ_BIT);

   if (vao->IndexBufferObj)
       elements =
          ADD_POINTERS(vao->IndexBufferObj->Mappings[MAP_INTERNAL].Pointer, elements);

   for (i = 0; i < count; i++) {
      GLuint j;

      /* j = element[i] */
      switch (elemType) {
      case GL_UNSIGNED_BYTE:
         j = ((const GLubyte *) elements)[i];
         break;
      case GL_UNSIGNED_SHORT:
         j = ((const GLushort *) elements)[i];
         break;
      case GL_UNSIGNED_INT:
         j = ((const GLuint *) elements)[i];
         break;
      default:
         UNREACHABLE("Unexpected index buffer type");
      }

      /* check element j of each enabled array */
      for (k = 0; k < VERT_ATTRIB_MAX; k++) {
         check_array_data(ctx, vao, k, j);
      }
   }

   _mesa_vao_unmap(ctx, vao);
}


/**
 * Check array data, looking for NaNs, etc.
 */
static void
check_draw_arrays_data(struct gl_context *ctx, GLint start, GLsizei count)
{
   /* TO DO */
}


/**
 * Print info/data for glDrawArrays(), for debugging.
 */
static void
print_draw_arrays(struct gl_context *ctx,
                  GLenum mode, GLint start, GLsizei count)
{
   struct gl_vertex_array_object *vao = ctx->Array.VAO;

   printf("_mesa_DrawArrays(mode 0x%x, start %d, count %d):\n",
          mode, start, count);

   _mesa_vao_map_arrays(ctx, vao, GL_MAP_READ_BIT);

   GLbitfield mask = vao->Enabled;
   while (mask) {
      const gl_vert_attrib i = u_bit_scan(&mask);
      const struct gl_array_attributes *array = &vao->VertexAttrib[i];

      const struct gl_vertex_buffer_binding *binding =
         &vao->BufferBinding[array->BufferBindingIndex];
      struct gl_buffer_object *bufObj = binding->BufferObj;

      printf("attr %s: size %d stride %d  "
             "ptr %p  Bufobj %u\n",
             gl_vert_attrib_name((gl_vert_attrib) i),
             array->Format.User.Size, binding->Stride,
             array->Ptr, bufObj ? bufObj->Name : 0);

      if (bufObj) {
         GLubyte *p = bufObj->Mappings[MAP_INTERNAL].Pointer;
         int offset = (int) (GLintptr)
            _mesa_vertex_attrib_address(array, binding);

         unsigned multiplier;
         switch (array->Format.User.Type) {
         case GL_DOUBLE:
         case GL_INT64_ARB:
         case GL_UNSIGNED_INT64_ARB:
            multiplier = 2;
            break;
         default:
            multiplier = 1;
         }

         float *f = (float *) (p + offset);
         int *k = (int *) f;
         int i = 0;
         int n = (count - 1) * (binding->Stride / (4 * multiplier))
            + array->Format.User.Size;
         if (n > 32)
            n = 32;
         printf("  Data at offset %d:\n", offset);
         do {
            if (multiplier == 2)
               printf("    double[%d] = 0x%016llx %lf\n", i,
                      ((unsigned long long *) k)[i], ((double *) f)[i]);
            else
               printf("    float[%d] = 0x%08x %f\n", i, k[i], f[i]);
            i++;
         } while (i < n);
      }
   }

   _mesa_vao_unmap_arrays(ctx, vao);
}


/**
 * Helper function called by the other DrawArrays() functions below.
 * This is where we handle primitive restart for drawing non-indexed
 * arrays.  If primitive restart is enabled, it typically means
 * splitting one DrawArrays() into two.
 */
static void
_mesa_draw_arrays(struct gl_context *ctx, GLenum mode, GLint start,
                  GLsizei count, GLuint numInstances, GLuint baseInstance)
{
   /* Viewperf has many draws with count=0. Discarding them is faster than
    * processing them.
    */
   if (!count || !numInstances)
      return;

   /* OpenGL 4.5 says that primitive restart is ignored with non-indexed
    * draws.
    */
   struct pipe_draw_info info;
   struct pipe_draw_start_count_bias draw;

   info.mode = mode;
   info.index_size = 0;
   /* Packed section begin. */
   info.primitive_restart = false;
   info.has_user_indices = false;
   info.index_bounds_valid = true;
   info.increment_draw_id = false;
   info.was_line_loop = false;
   info.index_bias_varies = false;
   /* Packed section end. */
   info.start_instance = baseInstance;
   info.instance_count = numInstances;
   info.min_index = start;
   info.max_index = start + count - 1;

   draw.start = start;
   draw.count = count;

   ST_PIPELINE_RENDER_STATE_MASK(mask);
   st_prepare_draw(ctx, mask);

   ctx->Driver.DrawGallium(ctx, &info, ctx->DrawID, NULL, &draw, 1);

   /* Dump comprehensive state after draw if enabled */
   dump_comprehensive_state(ctx, mode, start, count, numInstances, baseInstance);
   
   /* Reset the uniform buffer update flag */
   ctx->_UniformBufferDataUpdated = false;

   if (MESA_DEBUG_FLAGS & DEBUG_ALWAYS_FLUSH) {
      _mesa_flush(ctx);
   }
}


/**
 * Execute a glRectf() function.
 */
void GLAPIENTRY
_mesa_Rectf(GLfloat x1, GLfloat y1, GLfloat x2, GLfloat y2)
{
   GET_CURRENT_CONTEXT(ctx);
   ASSERT_OUTSIDE_BEGIN_END(ctx);

   CALL_Begin(ctx->Dispatch.Current, (GL_QUADS));
   /* Begin can change Dispatch.Current. */
   struct _glapi_table *dispatch = ctx->Dispatch.Current;
   CALL_Vertex2f(dispatch, (x1, y1));
   CALL_Vertex2f(dispatch, (x2, y1));
   CALL_Vertex2f(dispatch, (x2, y2));
   CALL_Vertex2f(dispatch, (x1, y2));
   CALL_End(dispatch, ());
}


void GLAPIENTRY
_mesa_Rectd(GLdouble x1, GLdouble y1, GLdouble x2, GLdouble y2)
{
   _mesa_Rectf((GLfloat) x1, (GLfloat) y1, (GLfloat) x2, (GLfloat) y2);
}

void GLAPIENTRY
_mesa_Rectdv(const GLdouble *v1, const GLdouble *v2)
{
   _mesa_Rectf((GLfloat) v1[0], (GLfloat) v1[1], (GLfloat) v2[0], (GLfloat) v2[1]);
}

void GLAPIENTRY
_mesa_Rectfv(const GLfloat *v1, const GLfloat *v2)
{
   _mesa_Rectf(v1[0], v1[1], v2[0], v2[1]);
}

void GLAPIENTRY
_mesa_Recti(GLint x1, GLint y1, GLint x2, GLint y2)
{
   _mesa_Rectf((GLfloat) x1, (GLfloat) y1, (GLfloat) x2, (GLfloat) y2);
}

void GLAPIENTRY
_mesa_Rectiv(const GLint *v1, const GLint *v2)
{
   _mesa_Rectf((GLfloat) v1[0], (GLfloat) v1[1], (GLfloat) v2[0], (GLfloat) v2[1]);
}

void GLAPIENTRY
_mesa_Rects(GLshort x1, GLshort y1, GLshort x2, GLshort y2)
{
   _mesa_Rectf((GLfloat) x1, (GLfloat) y1, (GLfloat) x2, (GLfloat) y2);
}

void GLAPIENTRY
_mesa_Rectsv(const GLshort *v1, const GLshort *v2)
{
   _mesa_Rectf((GLfloat) v1[0], (GLfloat) v1[1], (GLfloat) v2[0], (GLfloat) v2[1]);
}


void GLAPIENTRY
_mesa_EvalMesh1(GLenum mode, GLint i1, GLint i2)
{
   GET_CURRENT_CONTEXT(ctx);
   GLint i;
   GLfloat u, du;
   GLenum prim;

   switch (mode) {
   case GL_POINT:
      prim = GL_POINTS;
      break;
   case GL_LINE:
      prim = GL_LINE_STRIP;
      break;
   default:
      _mesa_error(ctx, GL_INVALID_ENUM, "glEvalMesh1(mode)");
      return;
   }

   /* No effect if vertex maps disabled.
    */
   if (!ctx->Eval.Map1Vertex4 && !ctx->Eval.Map1Vertex3)
      return;

   du = ctx->Eval.MapGrid1du;
   u = ctx->Eval.MapGrid1u1 + i1 * du;


   CALL_Begin(ctx->Dispatch.Current, (prim));
   /* Begin can change Dispatch.Current. */
   struct _glapi_table *dispatch = ctx->Dispatch.Current;
   for (i = i1; i <= i2; i++, u += du) {
      CALL_EvalCoord1f(dispatch, (u));
   }
   CALL_End(dispatch, ());
}


void GLAPIENTRY
_mesa_EvalMesh2(GLenum mode, GLint i1, GLint i2, GLint j1, GLint j2)
{
   GET_CURRENT_CONTEXT(ctx);
   GLfloat u, du, v, dv, v1, u1;
   GLint i, j;

   switch (mode) {
   case GL_POINT:
   case GL_LINE:
   case GL_FILL:
      break;
   default:
      _mesa_error(ctx, GL_INVALID_ENUM, "glEvalMesh2(mode)");
      return;
   }

   /* No effect if vertex maps disabled.
    */
   if (!ctx->Eval.Map2Vertex4 && !ctx->Eval.Map2Vertex3)
      return;

   du = ctx->Eval.MapGrid2du;
   dv = ctx->Eval.MapGrid2dv;
   v1 = ctx->Eval.MapGrid2v1 + j1 * dv;
   u1 = ctx->Eval.MapGrid2u1 + i1 * du;

   struct _glapi_table *dispatch;

   switch (mode) {
   case GL_POINT:
      CALL_Begin(ctx->Dispatch.Current, (GL_POINTS));
      /* Begin can change Dispatch.Current. */
      dispatch = ctx->Dispatch.Current;
      for (v = v1, j = j1; j <= j2; j++, v += dv) {
         for (u = u1, i = i1; i <= i2; i++, u += du) {
            CALL_EvalCoord2f(dispatch, (u, v));
         }
      }
      CALL_End(dispatch, ());
      break;
   case GL_LINE:
      for (v = v1, j = j1; j <= j2; j++, v += dv) {
         CALL_Begin(ctx->Dispatch.Current, (GL_LINE_STRIP));
         /* Begin can change Dispatch.Current. */
         dispatch = ctx->Dispatch.Current;
         for (u = u1, i = i1; i <= i2; i++, u += du) {
            CALL_EvalCoord2f(dispatch, (u, v));
         }
         CALL_End(dispatch, ());
      }
      for (u = u1, i = i1; i <= i2; i++, u += du) {
         CALL_Begin(ctx->Dispatch.Current, (GL_LINE_STRIP));
         /* Begin can change Dispatch.Current. */
         dispatch = ctx->Dispatch.Current;
         for (v = v1, j = j1; j <= j2; j++, v += dv) {
            CALL_EvalCoord2f(dispatch, (u, v));
         }
         CALL_End(dispatch, ());
      }
      break;
   case GL_FILL:
      for (v = v1, j = j1; j < j2; j++, v += dv) {
         CALL_Begin(ctx->Dispatch.Current, (GL_TRIANGLE_STRIP));
         /* Begin can change Dispatch.Current. */
         dispatch = ctx->Dispatch.Current;
         for (u = u1, i = i1; i <= i2; i++, u += du) {
            CALL_EvalCoord2f(dispatch, (u, v));
            CALL_EvalCoord2f(dispatch, (u, v + dv));
         }
         CALL_End(dispatch, ());
      }
      break;
   }
}


/**
 * Called from glDrawArrays when in immediate mode (not display list mode).
 */
void GLAPIENTRY
_mesa_DrawArrays(GLenum mode, GLint start, GLsizei count)
{
   GET_CURRENT_CONTEXT(ctx);
   FLUSH_FOR_DRAW(ctx);

   _mesa_set_varying_vp_inputs(ctx, ctx->VertexProgram._VPModeInputFilter &
                               ctx->Array._DrawVAO->_EnabledWithMapMode);
   if (ctx->NewState)
      _mesa_update_state(ctx);

   if (!_mesa_is_no_error_enabled(ctx) &&
       !_mesa_validate_DrawArrays(ctx, mode, count))
      return;

   if (0)
      check_draw_arrays_data(ctx, start, count);

   _mesa_draw_arrays(ctx, mode, start, count, 1, 0);

   if (0)
      print_draw_arrays(ctx, mode, start, count);
}


/**
 * Called from glDrawArraysInstanced when in immediate mode (not
 * display list mode).
 */
void GLAPIENTRY
_mesa_DrawArraysInstanced(GLenum mode, GLint start, GLsizei count,
                          GLsizei numInstances)
{
   _mesa_DrawArraysInstancedBaseInstance(mode, start, count, numInstances, 0);
}


/**
 * Called from glDrawArraysInstancedBaseInstance when in immediate mode.
 */
void GLAPIENTRY
_mesa_DrawArraysInstancedBaseInstance(GLenum mode, GLint first,
                                      GLsizei count, GLsizei numInstances,
                                      GLuint baseInstance)
{
   GET_CURRENT_CONTEXT(ctx);
   FLUSH_FOR_DRAW(ctx);

   _mesa_set_varying_vp_inputs(ctx, ctx->VertexProgram._VPModeInputFilter &
                               ctx->Array._DrawVAO->_EnabledWithMapMode);
   if (ctx->NewState)
      _mesa_update_state(ctx);

   if (!_mesa_is_no_error_enabled(ctx) &&
       !_mesa_validate_DrawArraysInstanced(ctx, mode, first, count,
                                           numInstances))
      return;

   if (0)
      check_draw_arrays_data(ctx, first, count);

   _mesa_draw_arrays(ctx, mode, first, count, numInstances, baseInstance);

   if (0)
      print_draw_arrays(ctx, mode, first, count);
}


/**
 * Called from glMultiDrawArrays when in immediate mode.
 */
void GLAPIENTRY
_mesa_MultiDrawArrays(GLenum mode, const GLint *first,
                      const GLsizei *count, GLsizei primcount)
{
   GET_CURRENT_CONTEXT(ctx);
   FLUSH_FOR_DRAW(ctx);

   _mesa_set_varying_vp_inputs(ctx, ctx->VertexProgram._VPModeInputFilter &
                               ctx->Array._DrawVAO->_EnabledWithMapMode);
   if (ctx->NewState)
      _mesa_update_state(ctx);

   if (!_mesa_is_no_error_enabled(ctx) &&
       !_mesa_validate_MultiDrawArrays(ctx, mode, count, primcount))
      return;

   if (primcount == 0)
      return;

   struct pipe_draw_info info;
   struct pipe_draw_start_count_bias *draw = get_temp_draws(ctx, primcount);
   if (!draw)
      return;

   info.mode = mode;
   info.index_size = 0;
   /* Packed section begin. */
   info.primitive_restart = false;
   info.has_user_indices = false;
   info.index_bounds_valid = false;
   info.increment_draw_id = primcount > 1;
   info.was_line_loop = false;
   info.index_bias_varies = false;
   /* Packed section end. */
   info.start_instance = 0;
   info.instance_count = 1;

   for (int i = 0; i < primcount; i++) {
      draw[i].start = first[i];
      draw[i].count = count[i];
   }

   ST_PIPELINE_RENDER_STATE_MASK(mask);
   st_prepare_draw(ctx, mask);

   ctx->Driver.DrawGallium(ctx, &info, 0, NULL, draw, primcount);

   if (MESA_DEBUG_FLAGS & DEBUG_ALWAYS_FLUSH)
      _mesa_flush(ctx);
}



/**
 * Map GL_ELEMENT_ARRAY_BUFFER and print contents.
 * For debugging.
 */
#if 0
static void
dump_element_buffer(struct gl_context *ctx, GLenum type)
{
   const GLvoid *map =
      ctx->Driver.MapBufferRange(ctx, 0,
                                 ctx->Array.VAO->IndexBufferObj->Size,
                                 GL_MAP_READ_BIT,
                                 ctx->Array.VAO->IndexBufferObj,
                                 MAP_INTERNAL);
   switch (type) {
   case GL_UNSIGNED_BYTE:
      {
         const GLubyte *us = (const GLubyte *) map;
         GLint i;
         for (i = 0; i < ctx->Array.VAO->IndexBufferObj->Size; i++) {
            printf("%02x ", us[i]);
            if (i % 32 == 31)
               printf("\n");
         }
         printf("\n");
      }
      break;
   case GL_UNSIGNED_SHORT:
      {
         const GLushort *us = (const GLushort *) map;
         GLint i;
         for (i = 0; i < ctx->Array.VAO->IndexBufferObj->Size / 2; i++) {
            printf("%04x ", us[i]);
            if (i % 16 == 15)
               printf("\n");
         }
         printf("\n");
      }
      break;
   case GL_UNSIGNED_INT:
      {
         const GLuint *us = (const GLuint *) map;
         GLint i;
         for (i = 0; i < ctx->Array.VAO->IndexBufferObj->Size / 4; i++) {
            printf("%08x ", us[i]);
            if (i % 8 == 7)
               printf("\n");
         }
         printf("\n");
      }
      break;
   default:
      ;
   }

   ctx->Driver.UnmapBuffer(ctx, ctx->Array.VAO->IndexBufferObj, MAP_INTERNAL);
}
#endif

static bool
validate_index_bounds(struct gl_context *ctx, struct pipe_draw_info *info,
                      const struct pipe_draw_start_count_bias *draws,
                      unsigned num_draws)
{
   assert(info->index_size);

   /* Get index bounds for user buffers. */
   if (!info->index_bounds_valid && ctx->st->draw_needs_minmax_index) {
      /* Return if this fails, which means all draws have count == 0. */
      if (!vbo_get_minmax_indices_gallium(ctx, info, draws, num_draws))
         return false;

      info->index_bounds_valid = true;
   }
   return true;
}

/**
 * Inner support for both _mesa_DrawElements and _mesa_DrawRangeElements.
 * Do the rendering for a glDrawElements or glDrawRangeElements call after
 * we've validated buffer bounds, etc.
 */
static void
_mesa_validated_drawrangeelements(struct gl_context *ctx,
                                  struct gl_buffer_object *index_bo,
                                  GLenum mode,
                                  bool index_bounds_valid,
                                  GLuint start, GLuint end,
                                  GLsizei count, GLenum type,
                                  const GLvoid * indices,
                                  GLint basevertex, GLuint numInstances,
                                  GLuint baseInstance)
{
   /* Viewperf has many draws with count=0. Discarding them is faster than
    * processing them.
    */
   if (!count || !numInstances)
      return;

   if (!index_bounds_valid) {
      assert(start == 0u);
      assert(end == ~0u);
   }

   unsigned index_size_shift = _mesa_get_index_size_shift(type);

   if (index_bo) {
      if (!indices_aligned(index_size_shift, indices))
         return;

      if (unlikely(index_bo->Size < (uintptr_t)indices || !index_bo->buffer)) {
#ifndef NDEBUG
         _mesa_warning(ctx, "Invalid indices offset 0x%" PRIxPTR
                            " (indices buffer size is %ld bytes)"
                            " or unallocated buffer (%u). Draw skipped.",
                            (uintptr_t)indices, (long)index_bo->Size,
                       !!index_bo->buffer);
#endif
         return;
      }
   }

   ST_PIPELINE_RENDER_STATE_MASK(mask);
   st_prepare_draw(ctx, mask);

   /* Fast path for a very common DrawElements case:
    * - there are no user indices here (always true with glthread)
    * - DrawGallium is st_draw_gallium (regular render mode, almost always
    *   true), which only calls cso_context::draw_vbo
    * - the threaded context is enabled while u_vbuf is bypassed (cso_context
    *   always calls tc_draw_vbo, which is always true with glthread if all
    *   vertex formats are also supported by the driver)
    * - DrawID is 0 (true if glthread isn't unrolling an indirect multi draw,
    *   which is almost always true)
    */
   struct st_context *st = st_context(ctx);
   if (index_bo && ctx->Driver.DrawGallium == st_draw_gallium &&
       st->cso_context->draw_vbo == tc_draw_vbo && ctx->DrawID == 0) {
      assert(!st->draw_needs_minmax_index);
      struct pipe_resource *index_buffer = index_bo->buffer;
      struct tc_draw_single *draw =
         tc_add_draw_single_call(st->pipe, index_buffer);
      bool primitive_restart = ctx->Array._PrimitiveRestart[index_size_shift];

      /* This must be set exactly like u_threaded_context sets it, not like
       * it would be set for draw_vbo.
       */
      draw->info.mode = mode;
      draw->info.index_size = 1 << index_size_shift;
      /* Packed section begin. */
      draw->info.primitive_restart = primitive_restart;
      draw->info.has_user_indices = false;
      draw->info.index_bounds_valid = false;
      draw->info.increment_draw_id = false;
      draw->info.index_bias_varies = false;
      draw->info.was_line_loop = false;
      draw->info._pad = 0;
      /* Packed section end. */
      draw->info.start_instance = baseInstance;
      draw->info.instance_count = numInstances;
      draw->info.restart_index =
         primitive_restart ? ctx->Array._RestartIndex[index_size_shift] : 0;
      draw->info.index.resource = index_buffer;

      /* u_threaded_context stores start/count in min/max_index for single draws. */
      draw->info.min_index = (uintptr_t)indices >> index_size_shift;
      draw->info.max_index = count;
      draw->index_bias = basevertex;
      return;
   }

   struct pipe_draw_info info;
   struct pipe_draw_start_count_bias draw;

   info.mode = mode;
   info.index_size = 1 << index_size_shift;
   /* Packed section begin. */
   info.primitive_restart = ctx->Array._PrimitiveRestart[index_size_shift];
   info.has_user_indices = index_bo == NULL;
   info.index_bounds_valid = index_bounds_valid;
   info.increment_draw_id = false;
   info.was_line_loop = false;
   info.index_bias_varies = false;
   /* Packed section end. */
   info.start_instance = baseInstance;
   info.instance_count = numInstances;
   info.restart_index = ctx->Array._RestartIndex[index_size_shift];

   if (info.has_user_indices) {
      info.index.user = indices;
      draw.start = 0;
   } else {
      draw.start = (uintptr_t)indices >> index_size_shift;
      info.index.resource = index_bo->buffer;
   }
   draw.index_bias = basevertex;

   info.min_index = start;
   info.max_index = end;
   draw.count = count;

   if (!validate_index_bounds(ctx, &info, &draw, 1))
      return;

   ctx->Driver.DrawGallium(ctx, &info, ctx->DrawID, NULL, &draw, 1);

   if (MESA_DEBUG_FLAGS & DEBUG_ALWAYS_FLUSH) {
      _mesa_flush(ctx);
   }
}


/**
 * Called by glDrawRangeElementsBaseVertex() in immediate mode.
 */
void GLAPIENTRY
_mesa_DrawRangeElementsBaseVertex(GLenum mode, GLuint start, GLuint end,
                                  GLsizei count, GLenum type,
                                  const GLvoid * indices, GLint basevertex)
{
   static GLuint warnCount = 0;
   bool index_bounds_valid = true;

   /* This is only useful to catch invalid values in the "end" parameter
    * like ~0.
    */
   GLuint max_element = 2 * 1000 * 1000 * 1000; /* just a big number */

   GET_CURRENT_CONTEXT(ctx);
   FLUSH_FOR_DRAW(ctx);

   _mesa_set_varying_vp_inputs(ctx, ctx->VertexProgram._VPModeInputFilter &
                               ctx->Array._DrawVAO->_EnabledWithMapMode);
   if (ctx->NewState)
      _mesa_update_state(ctx);

   if (!_mesa_is_no_error_enabled(ctx) &&
       !_mesa_validate_DrawRangeElements(ctx, mode, start, end, count,
                                         type))
      return;

   if ((int) end + basevertex < 0 || start + basevertex >= max_element) {
      /* The application requested we draw using a range of indices that's
       * outside the bounds of the current VBO.  This is invalid and appears
       * to give undefined results.  The safest thing to do is to simply
       * ignore the range, in case the application botched their range tracking
       * but did provide valid indices.  Also issue a warning indicating that
       * the application is broken.
       */
      if (warnCount++ < 10) {
         _mesa_warning(ctx, "glDrawRangeElements(start %u, end %u, "
                       "basevertex %d, count %d, type 0x%x, indices=%p):\n"
                       "\trange is outside VBO bounds (max=%u); ignoring.\n"
                       "\tThis should be fixed in the application.",
                       start, end, basevertex, count, type, indices,
                       max_element - 1);
      }
      index_bounds_valid = false;
   }

   /* NOTE: It's important that 'end' is a reasonable value.
    * in _tnl_draw_prims(), we use end to determine how many vertices
    * to transform.  If it's too large, we can unnecessarily split prims
    * or we can read/write out of memory in several different places!
    */

   /* Catch/fix some potential user errors */
   if (type == GL_UNSIGNED_BYTE) {
      start = MIN2(start, 0xff);
      end = MIN2(end, 0xff);
   }
   else if (type == GL_UNSIGNED_SHORT) {
      start = MIN2(start, 0xffff);
      end = MIN2(end, 0xffff);
   }

   if (0) {
      printf("glDraw[Range]Elements{,BaseVertex}"
             "(start %u, end %u, type 0x%x, count %d) ElemBuf %u, "
             "base %d\n",
             start, end, type, count,
             ctx->Array.VAO->IndexBufferObj ?
                ctx->Array.VAO->IndexBufferObj->Name : 0, basevertex);
   }

   if ((int) start + basevertex < 0 || end + basevertex >= max_element)
      index_bounds_valid = false;

#if 0
   check_draw_elements_data(ctx, count, type, indices, basevertex);
#else
   (void) check_draw_elements_data;
#endif

   if (!index_bounds_valid) {
      start = 0;
      end = ~0;
   }

   _mesa_validated_drawrangeelements(ctx, ctx->Array.VAO->IndexBufferObj,
                                     mode, index_bounds_valid, start, end,
                                     count, type, indices, basevertex, 1, 0);
}


/**
 * Called by glDrawRangeElements() in immediate mode.
 */
void GLAPIENTRY
_mesa_DrawRangeElements(GLenum mode, GLuint start, GLuint end,
                        GLsizei count, GLenum type, const GLvoid * indices)
{
   _mesa_DrawRangeElementsBaseVertex(mode, start, end, count, type,
                                     indices, 0);
}


/**
 * Called by glDrawElements() in immediate mode.
 */
void GLAPIENTRY
_mesa_DrawElements(GLenum mode, GLsizei count, GLenum type,
                   const GLvoid * indices)
{
   _mesa_DrawElementsBaseVertex(mode, count, type, indices, 0);
}


/**
 * Called by glDrawElementsBaseVertex() in immediate mode.
 */
void GLAPIENTRY
_mesa_DrawElementsBaseVertex(GLenum mode, GLsizei count, GLenum type,
                             const GLvoid * indices, GLint basevertex)
{
   GET_CURRENT_CONTEXT(ctx);
   FLUSH_FOR_DRAW(ctx);

   _mesa_set_varying_vp_inputs(ctx, ctx->VertexProgram._VPModeInputFilter &
                               ctx->Array._DrawVAO->_EnabledWithMapMode);
   if (ctx->NewState)
      _mesa_update_state(ctx);

   if (!_mesa_is_no_error_enabled(ctx) &&
       !_mesa_validate_DrawElements(ctx, mode, count, type))
      return;

   _mesa_validated_drawrangeelements(ctx, ctx->Array.VAO->IndexBufferObj,
                                     mode, false, 0, ~0,
                                     count, type, indices, basevertex, 1, 0);
}


/**
 * Called by glDrawElementsInstanced() in immediate mode.
 */
void GLAPIENTRY
_mesa_DrawElementsInstanced(GLenum mode, GLsizei count, GLenum type,
                            const GLvoid * indices, GLsizei numInstances)
{
   _mesa_DrawElementsInstancedBaseVertexBaseInstance(mode, count, type,
                                                     indices, numInstances,
                                                     0, 0);
}


/**
 * Called by glDrawElementsInstancedBaseVertex() in immediate mode.
 */
void GLAPIENTRY
_mesa_DrawElementsInstancedBaseVertex(GLenum mode, GLsizei count,
                                      GLenum type, const GLvoid * indices,
                                      GLsizei numInstances,
                                      GLint basevertex)
{
   _mesa_DrawElementsInstancedBaseVertexBaseInstance(mode, count, type,
                                                     indices, numInstances,
                                                     basevertex, 0);
}


/**
 * Called by glDrawElementsInstancedBaseInstance() in immediate mode.
 */
void GLAPIENTRY
_mesa_DrawElementsInstancedBaseInstance(GLenum mode, GLsizei count,
                                        GLenum type,
                                        const GLvoid *indices,
                                        GLsizei numInstances,
                                        GLuint baseInstance)
{
   _mesa_DrawElementsInstancedBaseVertexBaseInstance(mode, count, type,
                                                     indices, numInstances,
                                                     0, baseInstance);
}


/**
 * Called by glDrawElementsInstancedBaseVertexBaseInstance() in immediate mode.
 */
void GLAPIENTRY
_mesa_DrawElementsInstancedBaseVertexBaseInstance(GLenum mode,
                                                  GLsizei count,
                                                  GLenum type,
                                                  const GLvoid *indices,
                                                  GLsizei numInstances,
                                                  GLint basevertex,
                                                  GLuint baseInstance)
{
   GET_CURRENT_CONTEXT(ctx);
   FLUSH_FOR_DRAW(ctx);

   _mesa_set_varying_vp_inputs(ctx, ctx->VertexProgram._VPModeInputFilter &
                               ctx->Array._DrawVAO->_EnabledWithMapMode);
   if (ctx->NewState)
      _mesa_update_state(ctx);

   if (!_mesa_is_no_error_enabled(ctx) &&
       !_mesa_validate_DrawElementsInstanced(ctx, mode, count, type,
                                             numInstances))
      return;

   _mesa_validated_drawrangeelements(ctx, ctx->Array.VAO->IndexBufferObj,
                                     mode, false, 0, ~0,
                                     count, type, indices, basevertex,
                                     numInstances, baseInstance);
}

/**
 * Same as glDrawElementsInstancedBaseVertexBaseInstance, but the index
 * buffer is set by the indexBuf parameter instead of using the bound
 * GL_ELEMENT_ARRAY_BUFFER if indexBuf != NULL.
 */
void GLAPIENTRY
_mesa_DrawElementsUserBuf(const GLvoid *ptr)
{
   GET_CURRENT_CONTEXT(ctx);
   FLUSH_FOR_DRAW(ctx);

   _mesa_set_varying_vp_inputs(ctx, ctx->VertexProgram._VPModeInputFilter &
                               ctx->Array._DrawVAO->_EnabledWithMapMode);
   if (ctx->NewState)
      _mesa_update_state(ctx);

   const struct marshal_cmd_DrawElementsUserBuf *cmd =
      (const struct marshal_cmd_DrawElementsUserBuf *)ptr;
   const GLenum mode = cmd->mode;
   const GLsizei count = cmd->count;
   const GLenum type = _mesa_decode_index_type(cmd->type);
   const GLsizei instance_count = cmd->instance_count;

   if (!_mesa_is_no_error_enabled(ctx) &&
       !_mesa_validate_DrawElementsInstanced(ctx, mode, count, type,
                                             instance_count))
      return;

   struct gl_buffer_object *index_bo =
      cmd->index_buffer ? cmd->index_buffer : ctx->Array.VAO->IndexBufferObj;

   const GLvoid *indices = cmd->indices;
   const GLint basevertex = cmd->basevertex;
   const GLuint baseinstance = cmd->baseinstance;

   ctx->DrawID = cmd->drawid;
   _mesa_validated_drawrangeelements(ctx, index_bo,
                                     mode, false, 0, ~0,
                                     count, type, indices, basevertex,
                                     instance_count, baseinstance);
   ctx->DrawID = 0;
}

/**
 * Same as glDrawElementsInstancedBaseVertexBaseInstance, but the index
 * buffer is set by the indexBuf parameter instead of using the bound
 * GL_ELEMENT_ARRAY_BUFFER if indexBuf != NULL.
 */
void GLAPIENTRY
_mesa_DrawElementsUserBufPacked(const GLvoid *ptr)
{
   GET_CURRENT_CONTEXT(ctx);
   FLUSH_FOR_DRAW(ctx);

   _mesa_set_varying_vp_inputs(ctx, ctx->VertexProgram._VPModeInputFilter &
                               ctx->Array._DrawVAO->_EnabledWithMapMode);
   if (ctx->NewState)
      _mesa_update_state(ctx);

   const struct marshal_cmd_DrawElementsUserBufPacked *cmd =
      (const struct marshal_cmd_DrawElementsUserBufPacked *)ptr;
   const GLenum mode = cmd->mode;
   const GLsizei count = cmd->count;
   const GLenum type = _mesa_decode_index_type(cmd->type);

   if (!_mesa_is_no_error_enabled(ctx) &&
       !_mesa_validate_DrawElements(ctx, mode, count, type))
      return;

   struct gl_buffer_object *index_bo =
      cmd->index_buffer ? cmd->index_buffer : ctx->Array.VAO->IndexBufferObj;

   const GLvoid *indices = (void*)(uintptr_t)cmd->indices;

   _mesa_validated_drawrangeelements(ctx, index_bo, mode, false, 0, ~0,
                                     count, type, indices, 0, 1, 0);
}

/**
 * Inner support for both _mesa_MultiDrawElements() and
 * _mesa_MultiDrawRangeElements().
 * This does the actual rendering after we've checked array indexes, etc.
 */
static void
_mesa_validated_multidrawelements(struct gl_context *ctx,
                                  struct gl_buffer_object *index_bo,
                                  GLenum mode, const GLsizei *count,
                                  GLenum type, const GLvoid * const *indices,
                                  GLsizei primcount, const GLint *basevertex)
{
   uintptr_t min_index_ptr, max_index_ptr;
   bool fallback = false;
   int i;

   if (primcount == 0)
      return;

   unsigned index_size_shift = _mesa_get_index_size_shift(type);

   min_index_ptr = (uintptr_t) indices[0];
   max_index_ptr = 0;
   for (i = 0; i < primcount; i++) {
      if (count[i]) {
         min_index_ptr = MIN2(min_index_ptr, (uintptr_t) indices[i]);
         max_index_ptr = MAX2(max_index_ptr, (uintptr_t) indices[i] +
                              (count[i] << index_size_shift));
      }
   }

   /* Check if we can handle this thing as a bunch of index offsets from the
    * same index pointer.  If we can't, then we have to fall back to doing
    * a draw_prims per primitive.
    * Check that the difference between each prim's indexes is a multiple of
    * the index/element size.
    */
   if (index_size_shift) {
      for (i = 0; i < primcount; i++) {
         if (count[i] &&
             (((uintptr_t)indices[i] - min_index_ptr) &
              ((1 << index_size_shift) - 1)) != 0) {
            fallback = true;
            break;
         }
      }
   }

   struct pipe_draw_info info;

   info.mode = mode;
   info.index_size = 1 << index_size_shift;
   /* Packed section begin. */
   info.primitive_restart = ctx->Array._PrimitiveRestart[index_size_shift];
   info.has_user_indices = index_bo == NULL;
   info.index_bounds_valid = false;
   info.increment_draw_id = primcount > 1;
   info.was_line_loop = false;
   info.index_bias_varies = !!basevertex;
   /* Packed section end. */
   info.start_instance = 0;
   info.instance_count = 1;
   info.restart_index = ctx->Array._RestartIndex[index_size_shift];

   if (info.has_user_indices) {
      info.index.user = (void*)min_index_ptr;
   } else {
      info.index.resource = index_bo->buffer;

      /* No index buffer storage allocated - nothing to do. */
      if (!info.index.resource)
         return;
   }

   if (!fallback &&
       (!info.has_user_indices ||
        /* "max_index_ptr - min_index_ptr >> index_size_shift" is stored
         * in draw[i].start. The driver will multiply it later by index_size
         * so make sure the final value won't overflow.
         *
         * For real index buffers, gallium doesn't support index buffer offsets
         * greater than UINT32_MAX bytes.
         */
        max_index_ptr - min_index_ptr <= UINT32_MAX)) {
      struct pipe_draw_start_count_bias *draw = get_temp_draws(ctx, primcount);
      if (!draw)
         return;

      if (info.has_user_indices) {
         for (int i = 0; i < primcount; i++) {
            draw[i].start =
               ((uintptr_t)indices[i] - min_index_ptr) >> index_size_shift;
            draw[i].count = count[i];
            draw[i].index_bias = basevertex ? basevertex[i] : 0;
         }
      } else {
         for (int i = 0; i < primcount; i++) {
            draw[i].start = (uintptr_t)indices[i] >> index_size_shift;
            draw[i].count =
               indices_aligned(index_size_shift, indices[i]) ? count[i] : 0;
            draw[i].index_bias = basevertex ? basevertex[i] : 0;
         }
      }

      ST_PIPELINE_RENDER_STATE_MASK(mask);
      st_prepare_draw(ctx, mask);
      if (!validate_index_bounds(ctx, &info, draw, primcount))
         return;

      ctx->Driver.DrawGallium(ctx, &info, 0, NULL, draw, primcount);
   } else {
      /* draw[i].start would overflow. Draw one at a time. */
      assert(info.has_user_indices);
      info.increment_draw_id = false;

      ST_PIPELINE_RENDER_STATE_MASK(mask);
      st_prepare_draw(ctx, mask);

      for (int i = 0; i < primcount; i++) {
         struct pipe_draw_start_count_bias draw;

         if (!count[i])
            continue;

         /* Reset these, because the callee can change them. */
         info.index_bounds_valid = false;
         info.index.user = indices[i];
         draw.start = 0;
         draw.index_bias = basevertex ? basevertex[i] : 0;
         draw.count = count[i];

         if (!draw.count || !validate_index_bounds(ctx, &info, &draw, 1))
            continue;

         ctx->Driver.DrawGallium(ctx, &info, i, NULL, &draw, 1);
      }
   }

   if (MESA_DEBUG_FLAGS & DEBUG_ALWAYS_FLUSH) {
      _mesa_flush(ctx);
   }
}


void GLAPIENTRY
_mesa_MultiDrawElements(GLenum mode, const GLsizei *count, GLenum type,
                        const GLvoid * const *indices, GLsizei primcount)
{
   GET_CURRENT_CONTEXT(ctx);
   FLUSH_FOR_DRAW(ctx);

   _mesa_set_varying_vp_inputs(ctx, ctx->VertexProgram._VPModeInputFilter &
                               ctx->Array._DrawVAO->_EnabledWithMapMode);
   if (ctx->NewState)
      _mesa_update_state(ctx);

   struct gl_buffer_object *index_bo = ctx->Array.VAO->IndexBufferObj;

   if (!_mesa_is_no_error_enabled(ctx) &&
       !_mesa_validate_MultiDrawElements(ctx, mode, count, type, indices,
                                         primcount, index_bo))
      return;

   _mesa_validated_multidrawelements(ctx, index_bo, mode, count, type,
                                     indices, primcount, NULL);
}


void GLAPIENTRY
_mesa_MultiDrawElementsBaseVertex(GLenum mode,
                                  const GLsizei *count, GLenum type,
                                  const GLvoid * const *indices,
                                  GLsizei primcount,
                                  const GLsizei *basevertex)
{
   GET_CURRENT_CONTEXT(ctx);
   FLUSH_FOR_DRAW(ctx);

   _mesa_set_varying_vp_inputs(ctx, ctx->VertexProgram._VPModeInputFilter &
                               ctx->Array._DrawVAO->_EnabledWithMapMode);
   if (ctx->NewState)
      _mesa_update_state(ctx);

   struct gl_buffer_object *index_bo = ctx->Array.VAO->IndexBufferObj;

   if (!_mesa_is_no_error_enabled(ctx) &&
       !_mesa_validate_MultiDrawElements(ctx, mode, count, type, indices,
                                         primcount, index_bo))
      return;

   _mesa_validated_multidrawelements(ctx, index_bo, mode, count, type,
                                     indices, primcount, basevertex);
}


/**
 * Same as glMultiDrawElementsBaseVertex, but the index buffer is set by
 * the indexBuf parameter instead of using the bound GL_ELEMENT_ARRAY_BUFFER
 * if indexBuf != NULL.
 */
void GLAPIENTRY
_mesa_MultiDrawElementsUserBuf(GLintptr indexBuf, GLenum mode,
                               const GLsizei *count, GLenum type,
                               const GLvoid * const * indices,
                               GLsizei primcount, const GLint * basevertex)
{
   GET_CURRENT_CONTEXT(ctx);
   FLUSH_FOR_DRAW(ctx);

   _mesa_set_varying_vp_inputs(ctx, ctx->VertexProgram._VPModeInputFilter &
                               ctx->Array._DrawVAO->_EnabledWithMapMode);
   if (ctx->NewState)
      _mesa_update_state(ctx);

   struct gl_buffer_object *index_bo =
      indexBuf ? (struct gl_buffer_object*)indexBuf :
                 ctx->Array.VAO->IndexBufferObj;

   if (!_mesa_is_no_error_enabled(ctx) &&
       !_mesa_validate_MultiDrawElements(ctx, mode, count, type, indices,
                                         primcount, index_bo))
      return;

   _mesa_validated_multidrawelements(ctx, index_bo, mode, count, type,
                                     indices, primcount, basevertex);
}


/**
 * Like DrawArrays, but take the count from a transform feedback object.
 * \param mode  GL_POINTS, GL_LINES, GL_TRIANGLE_STRIP, etc.
 * \param name  the transform feedback object
 * User still has to setup of the vertex attribute info with
 * glVertexPointer, glColorPointer, etc.
 * Part of GL_ARB_transform_feedback2.
 */
void GLAPIENTRY
_mesa_DrawTransformFeedback(GLenum mode, GLuint name)
{
   _mesa_DrawTransformFeedbackStreamInstanced(mode, name, 0, 1);
}


void GLAPIENTRY
_mesa_DrawTransformFeedbackStream(GLenum mode, GLuint name, GLuint stream)
{
   _mesa_DrawTransformFeedbackStreamInstanced(mode, name, stream, 1);
}


void GLAPIENTRY
_mesa_DrawTransformFeedbackInstanced(GLenum mode, GLuint name,
                                     GLsizei primcount)
{
   _mesa_DrawTransformFeedbackStreamInstanced(mode, name, 0, primcount);
}


void GLAPIENTRY
_mesa_DrawTransformFeedbackStreamInstanced(GLenum mode, GLuint name,
                                           GLuint stream,
                                           GLsizei primcount)
{
   GET_CURRENT_CONTEXT(ctx);
   struct gl_transform_feedback_object *obj =
      _mesa_lookup_transform_feedback_object(ctx, name);

   FLUSH_FOR_DRAW(ctx);

   _mesa_set_varying_vp_inputs(ctx, ctx->VertexProgram._VPModeInputFilter &
                               ctx->Array._DrawVAO->_EnabledWithMapMode);
   if (ctx->NewState)
      _mesa_update_state(ctx);

   if (!_mesa_is_no_error_enabled(ctx) &&
       !_mesa_validate_DrawTransformFeedback(ctx, mode, obj, stream,
                                             primcount))
      return;

   ST_PIPELINE_RENDER_STATE_MASK(mask);
   st_prepare_draw(ctx, mask);

   struct pipe_draw_indirect_info indirect;
   memset(&indirect, 0, sizeof(indirect));
   indirect.count_from_stream_output = obj->draw_count[stream];
   if (indirect.count_from_stream_output == NULL)
      return;

   struct pipe_draw_start_count_bias draw = {0};
   struct pipe_draw_info info;
   util_draw_init_info(&info);
   info.max_index = ~0u; /* so that u_vbuf can tell that it's unknown */
   info.mode = mode;
   info.instance_count = primcount;

   ctx->Driver.DrawGallium(ctx, &info, 0, &indirect, &draw, 1);

   if (MESA_DEBUG_FLAGS & DEBUG_ALWAYS_FLUSH) {
      _mesa_flush(ctx);
   }
}


/**
 * Like [Multi]DrawArrays/Elements, but they take most arguments from
 * a buffer object.
 */
void GLAPIENTRY
_mesa_DrawArraysIndirect(GLenum mode, const GLvoid *indirect)
{
   GET_CURRENT_CONTEXT(ctx);

   /* From the ARB_draw_indirect spec:
    *
    *    "Initially zero is bound to DRAW_INDIRECT_BUFFER. In the
    *    compatibility profile, this indicates that DrawArraysIndirect and
    *    DrawElementsIndirect are to source their arguments directly from the
    *    pointer passed as their <indirect> parameters."
    */
   if (_mesa_is_desktop_gl_compat(ctx) &&
       !ctx->DrawIndirectBuffer) {
      DrawArraysIndirectCommand *cmd = (DrawArraysIndirectCommand *) indirect;

      _mesa_DrawArraysInstancedBaseInstance(mode, cmd->first, cmd->count,
                                            cmd->primCount,
                                            cmd->baseInstance);
      return;
   }

   FLUSH_FOR_DRAW(ctx);

   _mesa_set_varying_vp_inputs(ctx, ctx->VertexProgram._VPModeInputFilter &
                               ctx->Array._DrawVAO->_EnabledWithMapMode);
   if (ctx->NewState)
      _mesa_update_state(ctx);

   if (!_mesa_is_no_error_enabled(ctx) &&
       !_mesa_validate_DrawArraysIndirect(ctx, mode, indirect))
      return;

   st_indirect_draw_vbo(ctx, mode, 0, (GLintptr)indirect, 0, 1, 16);
}


void GLAPIENTRY
_mesa_DrawElementsIndirect(GLenum mode, GLenum type, const GLvoid *indirect)
{
   GET_CURRENT_CONTEXT(ctx);

   /* From the ARB_draw_indirect spec:
    *
    *    "Initially zero is bound to DRAW_INDIRECT_BUFFER. In the
    *    compatibility profile, this indicates that DrawArraysIndirect and
    *    DrawElementsIndirect are to source their arguments directly from the
    *    pointer passed as their <indirect> parameters."
    */
   if (_mesa_is_desktop_gl_compat(ctx) &&
       !ctx->DrawIndirectBuffer) {
      /*
       * Unlike regular DrawElementsInstancedBaseVertex commands, the indices
       * may not come from a client array and must come from an index buffer.
       * If no element array buffer is bound, an INVALID_OPERATION error is
       * generated.
       */
      if (!ctx->Array.VAO->IndexBufferObj) {
         _mesa_error(ctx, GL_INVALID_OPERATION,
                     "glDrawElementsIndirect(no buffer bound "
                     "to GL_ELEMENT_ARRAY_BUFFER)");
      } else {
         DrawElementsIndirectCommand *cmd =
            (DrawElementsIndirectCommand *) indirect;

         /* Convert offset to pointer */
         void *offset = (void *)
            (uintptr_t)((cmd->firstIndex * _mesa_sizeof_type(type)) & 0xffffffffUL);

         _mesa_DrawElementsInstancedBaseVertexBaseInstance(mode, cmd->count,
                                                           type, offset,
                                                           cmd->primCount,
                                                           cmd->baseVertex,
                                                           cmd->baseInstance);
      }

      return;
   }

   FLUSH_FOR_DRAW(ctx);

   _mesa_set_varying_vp_inputs(ctx, ctx->VertexProgram._VPModeInputFilter &
                               ctx->Array._DrawVAO->_EnabledWithMapMode);
   if (ctx->NewState)
      _mesa_update_state(ctx);

   if (!_mesa_is_no_error_enabled(ctx) &&
       !_mesa_validate_DrawElementsIndirect(ctx, mode, type, indirect))
      return;

   st_indirect_draw_vbo(ctx, mode, type, (GLintptr)indirect, 0, 1, 20);
}


void GLAPIENTRY
_mesa_MultiDrawArraysIndirect(GLenum mode, const GLvoid *indirect,
                              GLsizei primcount, GLsizei stride)
{
   GET_CURRENT_CONTEXT(ctx);

   /* If <stride> is zero, the array elements are treated as tightly packed. */
   if (stride == 0)
      stride = sizeof(DrawArraysIndirectCommand);

   FLUSH_FOR_DRAW(ctx);

   _mesa_set_varying_vp_inputs(ctx, ctx->VertexProgram._VPModeInputFilter &
                               ctx->Array._DrawVAO->_EnabledWithMapMode);
   if (ctx->NewState)
      _mesa_update_state(ctx);

   /* From the ARB_draw_indirect spec:
    *
    *    "Initially zero is bound to DRAW_INDIRECT_BUFFER. In the
    *    compatibility profile, this indicates that DrawArraysIndirect and
    *    DrawElementsIndirect are to source their arguments directly from the
    *    pointer passed as their <indirect> parameters."
    */
   if (_mesa_is_desktop_gl_compat(ctx) &&
       !ctx->DrawIndirectBuffer) {

      if (!_mesa_is_no_error_enabled(ctx) &&
          (!_mesa_valid_draw_indirect_multi(ctx, primcount, stride,
                                           "glMultiDrawArraysIndirect") ||
           !_mesa_validate_DrawArrays(ctx, mode, 1)))
         return;

      struct pipe_draw_info info;
      info.mode = mode;
      info.index_size = 0;
      /* Packed section begin. */
      info.primitive_restart = false;
      info.has_user_indices = false;
      info.index_bounds_valid = false;
      info.increment_draw_id = primcount > 1;
      info.was_line_loop = false;
      info.index_bias_varies = false;
      /* Packed section end. */

      ST_PIPELINE_RENDER_STATE_MASK(mask);
      st_prepare_draw(ctx, mask);

      const uint8_t *ptr = (const uint8_t *) indirect;
      for (unsigned i = 0; i < primcount; i++) {
         DrawArraysIndirectCommand *cmd = (DrawArraysIndirectCommand *) ptr;

         info.start_instance = cmd->baseInstance;
         info.instance_count = cmd->primCount;

         struct pipe_draw_start_count_bias draw;
         draw.start = cmd->first;
         draw.count = cmd->count;

         if (!draw.count)
            continue;

         ctx->Driver.DrawGallium(ctx, &info, i, NULL, &draw, 1);
         ptr += stride;
      }

      return;
   }

   if (!_mesa_is_no_error_enabled(ctx) &&
       !_mesa_validate_MultiDrawArraysIndirect(ctx, mode, indirect,
                                               primcount, stride))
      return;

   st_indirect_draw_vbo(ctx, mode, 0, (GLintptr)indirect, 0, primcount, stride);
}


void GLAPIENTRY
_mesa_MultiDrawElementsIndirect(GLenum mode, GLenum type,
                                const GLvoid *indirect,
                                GLsizei primcount, GLsizei stride)
{
   GET_CURRENT_CONTEXT(ctx);

   FLUSH_FOR_DRAW(ctx);

   _mesa_set_varying_vp_inputs(ctx, ctx->VertexProgram._VPModeInputFilter &
                               ctx->Array._DrawVAO->_EnabledWithMapMode);
   if (ctx->NewState)
      _mesa_update_state(ctx);

   /* If <stride> is zero, the array elements are treated as tightly packed. */
   if (stride == 0)
      stride = sizeof(DrawElementsIndirectCommand);

   /* From the ARB_draw_indirect spec:
    *
    *    "Initially zero is bound to DRAW_INDIRECT_BUFFER. In the
    *    compatibility profile, this indicates that DrawArraysIndirect and
    *    DrawElementsIndirect are to source their arguments directly from the
    *    pointer passed as their <indirect> parameters."
    */
   if (_mesa_is_desktop_gl_compat(ctx) &&
       !ctx->DrawIndirectBuffer) {
      /*
       * Unlike regular DrawElementsInstancedBaseVertex commands, the indices
       * may not come from a client array and must come from an index buffer.
       * If no element array buffer is bound, an INVALID_OPERATION error is
       * generated.
       */
      if (!ctx->Array.VAO->IndexBufferObj) {
         _mesa_error(ctx, GL_INVALID_OPERATION,
                     "glMultiDrawElementsIndirect(no buffer bound "
                     "to GL_ELEMENT_ARRAY_BUFFER)");

         return;
      }

      if (!_mesa_is_no_error_enabled(ctx) &&
          (!_mesa_valid_draw_indirect_multi(ctx, primcount, stride,
                                           "glMultiDrawArraysIndirect") ||
           !_mesa_validate_DrawElements(ctx, mode, 1, type)))
         return;

      unsigned index_size_shift = _mesa_get_index_size_shift(type);

      struct pipe_draw_info info;
      info.mode = mode;
      info.index_size = 1 << index_size_shift;
      /* Packed section begin. */
      info.primitive_restart = ctx->Array._PrimitiveRestart[index_size_shift];
      info.has_user_indices = false;
      info.index_bounds_valid = false;
      info.increment_draw_id = primcount > 1;
      info.was_line_loop = false;
      info.index_bias_varies = false;
      /* Packed section end. */
      info.restart_index = ctx->Array._RestartIndex[index_size_shift];

      struct gl_buffer_object *index_bo = ctx->Array.VAO->IndexBufferObj;

      info.index.resource = index_bo->buffer;

      /* No index buffer storage allocated - nothing to do. */
      if (!info.index.resource)
         return;

      ST_PIPELINE_RENDER_STATE_MASK(mask);
      st_prepare_draw(ctx, mask);

      const uint8_t *ptr = (const uint8_t *) indirect;
      for (unsigned i = 0; i < primcount; i++) {
         DrawElementsIndirectCommand *cmd = (DrawElementsIndirectCommand*)ptr;

         info.start_instance = cmd->baseInstance;
         info.instance_count = cmd->primCount;

         struct pipe_draw_start_count_bias draw;
         draw.start = cmd->firstIndex;
         draw.count = cmd->count;
         draw.index_bias = cmd->baseVertex;

         if (!draw.count || !validate_index_bounds(ctx, &info, &draw, 1))
            continue;

         ctx->Driver.DrawGallium(ctx, &info, i, NULL, &draw, 1);
         ptr += stride;
      }

      return;
   }

   if (!_mesa_is_no_error_enabled(ctx) &&
       !_mesa_validate_MultiDrawElementsIndirect(ctx, mode, type, indirect,
                                                 primcount, stride))
      return;

   st_indirect_draw_vbo(ctx, mode, type, (GLintptr)indirect, 0, primcount, stride);
}


void GLAPIENTRY
_mesa_MultiDrawArraysIndirectCountARB(GLenum mode, GLintptr indirect,
                                      GLintptr drawcount_offset,
                                      GLsizei maxdrawcount, GLsizei stride)
{
   GET_CURRENT_CONTEXT(ctx);
   FLUSH_FOR_DRAW(ctx);

   /* If <stride> is zero, the array elements are treated as tightly packed. */
   if (stride == 0)
      stride = 4 * sizeof(GLuint);      /* sizeof(DrawArraysIndirectCommand) */

   _mesa_set_varying_vp_inputs(ctx, ctx->VertexProgram._VPModeInputFilter &
                               ctx->Array._DrawVAO->_EnabledWithMapMode);
   if (ctx->NewState)
      _mesa_update_state(ctx);

   if (!_mesa_is_no_error_enabled(ctx) &&
       !_mesa_validate_MultiDrawArraysIndirectCount(ctx, mode, indirect,
                                                    drawcount_offset,
                                                    maxdrawcount, stride))
      return;

   st_indirect_draw_vbo(ctx, mode, 0, (GLintptr)indirect, drawcount_offset,
                        maxdrawcount, stride);
}


void GLAPIENTRY
_mesa_MultiDrawElementsIndirectCountARB(GLenum mode, GLenum type,
                                        GLintptr indirect,
                                        GLintptr drawcount_offset,
                                        GLsizei maxdrawcount, GLsizei stride)
{
   GET_CURRENT_CONTEXT(ctx);
   FLUSH_FOR_DRAW(ctx);

   /* If <stride> is zero, the array elements are treated as tightly packed. */
   if (stride == 0)
      stride = 5 * sizeof(GLuint);      /* sizeof(DrawElementsIndirectCommand) */

   _mesa_set_varying_vp_inputs(ctx, ctx->VertexProgram._VPModeInputFilter &
                               ctx->Array._DrawVAO->_EnabledWithMapMode);
   if (ctx->NewState)
      _mesa_update_state(ctx);

   if (!_mesa_is_no_error_enabled(ctx) &&
       !_mesa_validate_MultiDrawElementsIndirectCount(ctx, mode, type,
                                                      indirect,
                                                      drawcount_offset,
                                                      maxdrawcount, stride))
      return;

   st_indirect_draw_vbo(ctx, mode, type, (GLintptr)indirect, drawcount_offset,
                        maxdrawcount, stride);
}


/* GL_IBM_multimode_draw_arrays */
void GLAPIENTRY
_mesa_MultiModeDrawArraysIBM( const GLenum * mode, const GLint * first,
                              const GLsizei * count,
                              GLsizei primcount, GLint modestride )
{
   GET_CURRENT_CONTEXT(ctx);
   GLint i;

   for ( i = 0 ; i < primcount ; i++ ) {
      if ( count[i] > 0 ) {
         GLenum m = *((GLenum *) ((GLubyte *) mode + i * modestride));
         CALL_DrawArrays(ctx->Dispatch.Current, ( m, first[i], count[i] ));
      }
   }
}


/* GL_IBM_multimode_draw_arrays */
void GLAPIENTRY
_mesa_MultiModeDrawElementsIBM( const GLenum * mode, const GLsizei * count,
                                GLenum type, const GLvoid * const * indices,
                                GLsizei primcount, GLint modestride )
{
   GET_CURRENT_CONTEXT(ctx);
   GLint i;

   for ( i = 0 ; i < primcount ; i++ ) {
      if ( count[i] > 0 ) {
         GLenum m = *((GLenum *) ((GLubyte *) mode + i * modestride));
         CALL_DrawElements(ctx->Dispatch.Current, ( m, count[i], type,
                                                         indices[i] ));
      }
   }
}
