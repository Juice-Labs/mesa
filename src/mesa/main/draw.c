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
#include <math.h>
#include "arrayobj.h"
#include "glheader.h"
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
#include "texgetimage.h"
#include "pipe/p_state.h"
#include "api_exec_decl.h"

#include "state_tracker/st_context.h"
#include "state_tracker/st_draw.h"
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
 * JUICE: decode the two environment/viewport uniform blocks into named,
 * human-readable values so the background multipliers can be inspected
 * directly (no raw-offset math). VRED binds OSGViewport at binding 0 and
 * OSGEnvironment at binding 6 (std140). The background sky color in the
 * environment shader (prog 54) is:
 *   texture(envMap,dir) * envColorMatrix * exposure * physicalScaleFactor
 * so a zero exposure / physicalScaleFactor / envColorMatrix blacks the
 * backdrop even when envMap itself is fine.
 */
static float
juice_ubo_f32(const uint8_t *base, unsigned off)
{
   float v;
   memcpy(&v, base + off, sizeof(v));
   return v;
}

static int
juice_ubo_i32(const uint8_t *base, unsigned off)
{
   int v;
   memcpy(&v, base + off, sizeof(v));
   return v;
}

static void
dump_env_decode(struct gl_context *ctx, unsigned draw_id)
{
   char filename[512];
   snprintf(filename, sizeof(filename), "c:\\temp\\draw_%06u_envdecode.txt", draw_id);
   FILE *fp = fopen(filename, "w");
   if (!fp)
      return;

   GLuint active_prog = (ctx->_Shader && ctx->_Shader->ActiveProgram) ?
                        ctx->_Shader->ActiveProgram->Name : 0;
   fprintf(fp, "=== ENV DECODE (program %u) ===\n", active_prog);

   const struct { unsigned binding; const char *name; } blocks[2] = {
      { 0, "OSGViewport" }, { 6, "OSGEnvironment" }
   };

   for (int b = 0; b < 2; b++) {
      unsigned bind = blocks[b].binding;
      if (bind >= ctx->Const.MaxUniformBufferBindings)
         continue;
      struct gl_buffer_object *bufObj = ctx->UniformBufferBindings[bind].BufferObject;
      GLintptr boff = ctx->UniformBufferBindings[bind].Offset;
      GLsizeiptr bsize = ctx->UniformBufferBindings[bind].Size;

      fprintf(fp, "\n[binding %u %s] buffer=%u bind_offset=%ld bind_size=%ld buf_total=%ld\n",
              bind, blocks[b].name, bufObj ? bufObj->Name : 0,
              (long)boff, (long)bsize, (long)(bufObj ? bufObj->Size : 0));
      if (!bufObj) {
         fprintf(fp, "  <no buffer bound at this binding>\n");
         continue;
      }
      if (boff < 0)
         boff = 0;

      GLsizeiptr avail = bufObj->Size - boff;
      if (avail <= 0) {
         fprintf(fp, "  <bind offset beyond buffer>\n");
         continue;
      }
      GLsizeiptr map_len = avail < 512 ? avail : 512;
      const uint8_t *p = (const uint8_t *)
         _mesa_bufferobj_map_range(ctx, boff, map_len, GL_MAP_READ_BIT, bufObj, MAP_INTERNAL);
      if (!p) {
         fprintf(fp, "  <map failed>\n");
         continue;
      }

      if (bind == 0) {
         /* OSGViewport std140 offsets */
         fprintf(fp, "  rec709toRCS diag   = %f %f %f %f\n",
                 juice_ubo_f32(p, 0), juice_ubo_f32(p, 20),
                 juice_ubo_f32(p, 40), juice_ubo_f32(p, 60));
         if (map_len > 124) {
            fprintf(fp, "  superSamplingScale = %f\n", juice_ubo_f32(p, 112));
            fprintf(fp, "  displayLuminance   = %f\n", juice_ubo_f32(p, 116));
            fprintf(fp, "  physicalScaleFactor= %f\n", juice_ubo_f32(p, 120));
         }
         if (map_len > 156) {
            fprintf(fp, "  sceneUnitScale     = %f\n", juice_ubo_f32(p, 144));
            fprintf(fp, "  colorspace(int)    = %d\n", juice_ubo_i32(p, 152));
         }
      } else {
         /* OSGEnvironment std140 offsets */
         fprintf(fp, "  envColorMatrix diag= %f %f %f %f\n",
                 juice_ubo_f32(p, 128), juice_ubo_f32(p, 148),
                 juice_ubo_f32(p, 168), juice_ubo_f32(p, 188));
         if (map_len > 208) {
            fprintf(fp, "  averageEnvColor    = %f %f %f %f\n",
                    juice_ubo_f32(p, 192), juice_ubo_f32(p, 196),
                    juice_ubo_f32(p, 200), juice_ubo_f32(p, 204));
            fprintf(fp, "  exposure           = %f\n", juice_ubo_f32(p, 208));
         }
         if (map_len > 224) {
            fprintf(fp, "  saturation         = %f\n", juice_ubo_f32(p, 212));
            fprintf(fp, "  whitepoint         = %f\n", juice_ubo_f32(p, 216));
            fprintf(fp, "  environmentSize    = %f\n", juice_ubo_f32(p, 220));
         }
      }
      _mesa_bufferobj_unmap(ctx, bufObj, MAP_INTERNAL);
   }

   fclose(fp);
}

/**
 * JUICE: dump every bindless sampler of the active program with its current
 * resolved 64-bit handle (the exact value the shader loads from the FS default
 * uniform block, i.e. uniform_0@64.base[N]), plus the texture target and
 * bound flag. This is the one datum missing from every prior dump: the default
 * uniform block is zink-internal and never appears as a GL buffer object, so
 * the handle the environment dome actually indexes the bindless sampler2D
 * container with (base[0]) can only be read here, straight out of
 * gl_bindless_sampler::data. The low 32 bits (u2u32) are what the shader uses
 * as the bindless array index; index 0 is zink's reserved null slot (black).
 */
static void
dump_bindless_handles(struct gl_context *ctx, unsigned draw_id)
{
   char filename[512];
   snprintf(filename, sizeof(filename), "c:\\temp\\draw_%06u_bindless.txt", draw_id);
   FILE *fp = fopen(filename, "w");
   if (!fp)
      return;

   GLuint active_prog = (ctx->_Shader && ctx->_Shader->ActiveProgram) ?
                        ctx->_Shader->ActiveProgram->Name : 0;
   fprintf(fp, "=== BINDLESS SAMPLER HANDLES (program %u) ===\n", active_prog);
   fprintf(fp, "(handle = value shader loads; u2u32 = bindless array index; "
               "index 0 == zink reserved null slot == black)\n");

   const struct { struct gl_program *prog; const char *name; } stages[] = {
      { ctx->VertexProgram._Current,   "vertex" },
      { ctx->FragmentProgram._Current, "fragment" },
   };

   for (unsigned s = 0; s < 2; s++) {
      struct gl_program *prog = stages[s].prog;
      if (!prog)
         continue;
      fprintf(fp, "\n[%s stage] prog_id=%u NumBindlessSamplers=%u "
                  "HasBoundBindlessSampler=%d\n",
              stages[s].name, prog->Id, prog->sh.NumBindlessSamplers,
              (int)prog->sh.HasBoundBindlessSampler);
      for (unsigned i = 0; i < prog->sh.NumBindlessSamplers; i++) {
         struct gl_bindless_sampler *bs = &prog->sh.BindlessSamplers[i];
         uint64_t h = 0;
         if (bs->data)
            memcpy(&h, bs->data, sizeof(h));
         fprintf(fp, "  idx=%u unit=%u bound=%d target=%d data=%p "
                     "handle=0x%llx u2u32=%u\n",
                 i, (unsigned)bs->unit, (int)bs->bound, (int)bs->target,
                 (void *)bs->data, (unsigned long long)h,
                 (unsigned)(h & 0xffffffffu));
      }
   }
   fclose(fp);
}

/**
 * JUICE: dump every bound shader-storage buffer (SSBO). The environment
 * light-loop path in the VRED background shader reads the light list out of
 * an SSBO (ssbos@32) as well as ubos@32[8/11]; if that SSBO is never bound
 * the loop count / light data is garbage and the backdrop renders black.
 * dump_uniform_buffers() only covers UBOs, so the SSBO bytes never appeared
 * in any prior capture. Here we walk ctx->ShaderStorageBufferBindings and
 * write the raw contents next to the UBO dumps.
 */
static void
dump_ssbos(struct gl_context *ctx, unsigned draw_id)
{
   char filename[512];
   snprintf(filename, sizeof(filename), "c:\\temp\\draw_%06u_ssbos.txt", draw_id);
   FILE *fp = fopen(filename, "w");
   if (!fp)
      return;

   fprintf(fp, "=== SHADER STORAGE BUFFER BINDINGS ===\n");
   bool any = false;
   for (GLuint i = 0; i < MAX_COMBINED_SHADER_STORAGE_BUFFERS; i++) {
      struct gl_buffer_object *bufObj =
         ctx->ShaderStorageBufferBindings[i].BufferObject;
      if (!bufObj)
         continue;
      any = true;
      fprintf(fp, "SSBO[%u]: Buffer=%u Offset=%ld Size=%ld (buffer total %lu bytes)\n",
              i, bufObj->Name,
              (long)ctx->ShaderStorageBufferBindings[i].Offset,
              (long)ctx->ShaderStorageBufferBindings[i].Size,
              (unsigned long)bufObj->Size);
      char buf_name[64];
      snprintf(buf_name, sizeof(buf_name), "ssbo%u", i);
      dump_buffer_object(ctx, bufObj, buf_name, draw_id);
   }
   if (!any)
      fprintf(fp, "(no shader storage buffers bound on this draw)\n");
   fclose(fp);
}

/**
 * JUICE: dump the active program's default-block (non-UBO) uniforms. These
 * scalars/vectors (shadowColor, occlusionColor, shadowIntensity, opacityMode,
 * gl_FbWposYTransform, ...) live in zink's internal default uniform block and
 * never appear as a GL buffer object, so they were invisible to every prior
 * capture. We read them straight out of gl_uniform_storage::storage and print
 * both float and int interpretations (the background loop count is a field
 * that is bit-pattern 1.0f but consumed as an integer).
 */
static void
dump_default_uniforms(struct gl_context *ctx, unsigned draw_id)
{
   struct gl_shader_program *shProg =
      ctx->_Shader ? ctx->_Shader->ActiveProgram : NULL;
   if (!shProg || !shProg->data)
      return;

   char filename[512];
   snprintf(filename, sizeof(filename), "c:\\temp\\draw_%06u_default_uniforms.txt", draw_id);
   FILE *fp = fopen(filename, "w");
   if (!fp)
      return;

   fprintf(fp, "=== DEFAULT-BLOCK UNIFORMS (program %u) ===\n", shProg->Name);
   fprintf(fp, "(block_index==-1 => default uniform block; values shown as float/int/hex)\n");

   for (unsigned u = 0; u < shProg->data->NumUniformStorage; u++) {
      struct gl_uniform_storage *uni = &shProg->data->UniformStorage[u];
      if (uni->block_index != -1)    /* only the default uniform block */
         continue;
      if (uni->is_shader_storage)
         continue;
      if (!uni->type || !uni->storage)
         continue;

      enum glsl_base_type bt = glsl_get_base_type(uni->type);
      /* opaque handles (samplers/images) are covered by dump_bindless_handles */
      if (bt == GLSL_TYPE_SAMPLER || bt == GLSL_TYPE_TEXTURE ||
          bt == GLSL_TYPE_IMAGE)
         continue;

      unsigned comps = glsl_get_components(uni->type);
      unsigned elems = uni->array_elements ? uni->array_elements : 1u;
      const char *btname =
         bt == GLSL_TYPE_FLOAT ? "float" :
         bt == GLSL_TYPE_INT   ? "int"   :
         bt == GLSL_TYPE_UINT  ? "uint"  :
         bt == GLSL_TYPE_BOOL  ? "bool"  : "other";

      fprintf(fp, "\n%s  type=%s comps=%u elems=%u loc=%d\n",
              uni->name.string ? uni->name.string : "?",
              btname, comps, elems, uni->remap_location);

      const union gl_constant_value *s = uni->storage;
      unsigned total = comps * elems;
      if (total > 64)               /* cap noisy arrays */
         total = 64;
      for (unsigned k = 0; k < total; k++) {
         fprintf(fp, "  [%u] f=%.6g  i=%d  u=0x%08x\n",
                 k, s[k].f, s[k].i, s[k].u);
      }
   }
   fclose(fp);
}

/**
 * JUICE: dump one texture image (a single cube face / 3D slice / 2D level 0)
 * to RGBA32F using the software GetTexImage path. Unlike the FBO+ReadPixels
 * path in dump_active_texture_contents(), this reads formats that are NOT
 * color-renderable -- crucially RGB9_E5 (the shared-exponent env cube) and
 * cube-map faces -- which is exactly the environment/IBL data that previously
 * came back empty. Also writes a tonemapped PPM (reinhard + gamma) so HDR
 * content is actually visible.
 */
static void
dump_teximage_float(struct gl_context *ctx, struct gl_texture_image *texImage,
                    unsigned draw_id, GLuint unit, GLuint target,
                    const char *facetag)
{
   if (!texImage || texImage->Width == 0 || texImage->Height == 0)
      return;
   if (texImage->_BaseFormat == GL_DEPTH_COMPONENT ||
       texImage->_BaseFormat == GL_DEPTH_STENCIL ||
       texImage->_BaseFormat == GL_STENCIL_INDEX)
      return;

   GLint w = texImage->Width;
   GLint h = texImage->Height;
   size_t n = (size_t)w * (size_t)h * 4u;
   float *pixels = malloc(n * sizeof(float));
   if (!pixels)
      return;
   memset(pixels, 0, n * sizeof(float));

   _mesa_GetTexSubImage_sw(ctx, 0, 0, 0, w, h, 1, GL_RGBA, GL_FLOAT,
                           pixels, texImage);

   char raw_filename[512];
   snprintf(raw_filename, sizeof(raw_filename),
            "c:\\temp\\draw_%06u_tex_unit%u_target%u%s_%dx%d_rgba32f.raw",
            draw_id, unit, target, facetag, w, h);
   FILE *rf = fopen(raw_filename, "wb");
   if (rf) {
      fwrite(pixels, sizeof(float), n, rf);
      fclose(rf);
   }

   char ppm_filename[512];
   snprintf(ppm_filename, sizeof(ppm_filename),
            "c:\\temp\\draw_%06u_tex_unit%u_target%u%s_%dx%d_tonemap.ppm",
            draw_id, unit, target, facetag, w, h);
   FILE *pf = fopen(ppm_filename, "wb");
   if (pf) {
      fprintf(pf, "P6\n%d %d\n255\n", w, h);
      for (int y = h - 1; y >= 0; y--) {
         for (int x = 0; x < w; x++) {
            const float *px = &pixels[((size_t)y * w + x) * 4];
            for (int c = 0; c < 3; c++) {
               float v = px[c];
               if (v < 0.0f)
                  v = 0.0f;
               v = v / (v + 1.0f);              /* reinhard tonemap */
               v = powf(v, 1.0f / 2.2f);        /* approx sRGB gamma */
               int iv = (int)(v * 255.0f + 0.5f);
               if (iv < 0) iv = 0;
               if (iv > 255) iv = 255;
               fputc(iv, pf);
            }
         }
      }
      fclose(pf);
   }
   free(pixels);
}

static void
dump_textures_float(struct gl_context *ctx, unsigned draw_id)
{
   for (GLuint unit = 0; unit < ctx->Const.MaxCombinedTextureImageUnits; unit++) {
      for (GLuint target = 0; target < NUM_TEXTURE_TARGETS; target++) {
         struct gl_texture_object *texObj =
            ctx->Texture.Unit[unit].CurrentTex[target];
         if (!texObj || texObj->Name == 0)
            continue;

         if (texObj->Target == GL_TEXTURE_CUBE_MAP) {
            for (unsigned face = 0; face < 6; face++) {
               char facetag[16];
               snprintf(facetag, sizeof(facetag), "_face%u", face);
               dump_teximage_float(ctx, texObj->Image[face][0],
                                    draw_id, unit, target, facetag);
            }
         } else {
            dump_teximage_float(ctx, texObj->Image[0][0],
                                draw_id, unit, target, "");
         }
      }
   }
}

/**
 * Comprehensive RenderDoc-style dump of all GL state and buffers
 */
static void
dump_comprehensive_state(struct gl_context *ctx, GLenum mode, GLint start, GLsizei count, GLuint numInstances, GLuint baseInstance)
{
   /* JUICE: also capture the first draw of each distinct GL program, even if
    * no UBO was updated that draw. The environment/background program (VRED
    * prog 54) reuses the OSGViewport/OSGEnvironment UBOs uploaded on an
    * earlier draw, so the _UniformBufferDataUpdated gate alone never captures
    * it. Recording one dump per program guarantees those UBO bytes
    * (physicalScaleFactor @ OSGViewport+120, exposure @ OSGEnvironment+208,
    * envColorMatrix @ OSGEnvironment+128) get written for offline inspection. */
   static GLuint seen_programs[512];
   static unsigned num_seen_programs = 0;
   GLuint active_prog = (ctx->_Shader && ctx->_Shader->ActiveProgram) ?
                        ctx->_Shader->ActiveProgram->Name : 0;
   bool new_program = true;
   for (unsigned i = 0; i < num_seen_programs; i++) {
      if (seen_programs[i] == active_prog) { new_program = false; break; }
   }
   if (new_program && num_seen_programs < 512)
      seen_programs[num_seen_programs++] = active_prog;

   /* NO GATE: dump full state + framebuffer + textures for EVERY draw call.
    * (Previously gated to new-program / UBO-update, which limited us to ~12
    * images total.) Disk and speed are explicitly not a concern; capturing
    * the black-background bug is paramount. */
   (void) new_program;

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
   dump_ssbos(ctx, current_dump);
   dump_default_uniforms(ctx, current_dump);
   dump_env_decode(ctx, current_dump);
   dump_bindless_handles(ctx, current_dump);
   dump_shader_program(ctx, current_dump);
   dump_texture_state(ctx, current_dump);
   dump_active_texture_contents(ctx, current_dump);
   dump_textures_float(ctx, current_dump);
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
 * JUICE: aligned combine-forensics dump. Unlike dump_comprehensive_state()
 * (which is gated on UBO updates / first-program-use, and whose NIR/uniform
 * side files are written at shader-COMPILE time with a stale counter), this
 * writes ONE self-contained file per draw, at DRAW time, under its own
 * counter, so every fact inside a file is guaranteed to describe the same
 * draw. Purpose: decide between the three final-combine hypotheses for the
 * VRED black background:
 *   (a) a sampler uniform (e.g. GLSL50's 'tex') left at default 0, sampling
 *       the black scene buffer instead of the gradient buffer;
 *   (b) a dropped/misordered composite draw;
 *   (c) a fullscreen paint (GLSL72) reading per-vertex attributes that are
 *       not enabled, falling back to a black current-attribute value.
 * Per draw it records: program id + per-stage shader names, FBO attachments,
 * blend/colormask/depth state, every default-block sampler uniform with its
 * unit VALUE and the RESOLVED texture id bound there, compact non-sampler
 * uniform values, VAO enables + array bindings, and the current generic
 * attribute values used as fallback for disabled arrays.
 */
static void
dump_combine_forensics(struct gl_context *ctx, GLenum mode, GLint start,
                       GLsizei count, GLuint numInstances)
{
   /* NO CAP, NO FILTERING: capture every single draw call, for the entire
    * run. Finding the black-background bug is paramount; disk and speed are
    * explicitly not a concern. These are cheap per-draw text files. */
   static unsigned forensic_counter = 0;
   unsigned id = ++forensic_counter;

   struct gl_shader_program *shProg =
      ctx->_Shader ? ctx->_Shader->ActiveProgram : NULL;

   char filename[512];
   snprintf(filename, sizeof(filename), "c:\\temp\\fdraw_%06u.txt", id);
   FILE *fp = fopen(filename, "w");
   if (!fp)
      return;

   fprintf(fp, "=== COMBINE FORENSICS DRAW %u ===\n", id);
   fprintf(fp, "Mode: 0x%x  Start: %d  Count: %d  Instances: %u\n",
           mode, start, count, numInstances);
   fprintf(fp, "GL Program: %u\n", shProg ? shProg->Name : 0);
   if (shProg) {
      for (unsigned st = 0; st < MESA_SHADER_STAGES; st++) {
         struct gl_linked_shader *sh = shProg->_LinkedShaders[st];
         if (sh && sh->Program) {
            fprintf(fp, "  stage[%u] %s: name=%s\n", st,
                    _mesa_shader_stage_to_string(st),
                    sh->Program->info.name ? sh->Program->info.name : "?");
         }
      }
   }

   /* --- framebuffer --- */
   struct gl_framebuffer *fb = ctx->DrawBuffer;
   fprintf(fp, "\n[framebuffer] FBO=%u %ux%u\n", fb->Name, fb->Width, fb->Height);
   for (int i = 0; i < MAX_DRAW_BUFFERS; i++) {
      struct gl_renderbuffer_attachment *att = &fb->Attachment[BUFFER_COLOR0 + i];
      if (att->Type == GL_TEXTURE && att->Texture)
         fprintf(fp, "  Color[%d]: Tex=%u Level=%u\n", i, att->Texture->Name,
                 att->TextureLevel);
      else if (att->Type == GL_RENDERBUFFER && att->Renderbuffer)
         fprintf(fp, "  Color[%d]: RB=%u\n", i, att->Renderbuffer->Name);
   }
   struct gl_renderbuffer_attachment *datt = &fb->Attachment[BUFFER_DEPTH];
   if (datt->Type == GL_TEXTURE && datt->Texture)
      fprintf(fp, "  Depth: Tex=%u\n", datt->Texture->Name);

   /* --- blend / mask / depth --- */
   fprintf(fp, "\n[pipeline] BlendEnabled=0x%x", ctx->Color.BlendEnabled);
   if (ctx->Color.BlendEnabled)
      fprintf(fp, " srcRGB=0x%x dstRGB=0x%x srcA=0x%x dstA=0x%x eqRGB=0x%x eqA=0x%x",
              ctx->Color.Blend[0].SrcRGB, ctx->Color.Blend[0].DstRGB,
              ctx->Color.Blend[0].SrcA, ctx->Color.Blend[0].DstA,
              ctx->Color.Blend[0].EquationRGB, ctx->Color.Blend[0].EquationA);
   fprintf(fp, "\n  ColorMask=0x%x DepthTest=%d DepthMask=%d DepthFunc=0x%x ScissorEnabled=0x%x\n",
           ctx->Color.ColorMask, ctx->Depth.Test ? 1 : 0,
           ctx->Depth.Mask ? 1 : 0, ctx->Depth.Func, ctx->Scissor.EnableFlags);

   /* --- sampler uniforms: VALUE (texture unit) + resolved texture id --- */
   fprintf(fp, "\n[samplers]\n");
   if (shProg && shProg->data) {
      for (unsigned u = 0; u < shProg->data->NumUniformStorage; u++) {
         struct gl_uniform_storage *uni = &shProg->data->UniformStorage[u];
         if (uni->block_index != -1 || uni->is_shader_storage ||
             !uni->type || !uni->storage)
            continue;
         if (glsl_get_base_type(uni->type) != GLSL_TYPE_SAMPLER)
            continue;
         /* Read the LIVE value the driver actually uses, via a plain memory
          * read (no GL API re-entry -- this runs inside the draw hook). Under
          * PackedDriverUniformStorage (gallium/zink enable PIPE_CAP_PACKED_
          * UNIFORMS) glUniform* writes bindless samplers and non-opaque
          * uniforms into uni->driver_storage[], leaving uni->storage stale;
          * plain (non-bindless) samplers still live in uni->storage. */
         const bool samp_packed =
            ctx->Const.PackedDriverUniformStorage && uni->is_bindless &&
            uni->num_driver_storage > 0 && uni->driver_storage[0].data;
         const union gl_constant_value *samp_live =
            samp_packed ? (const union gl_constant_value *)
                             uni->driver_storage[0].data
                        : uni->storage;
         unsigned elems = uni->array_elements ? uni->array_elements : 1u;
         for (unsigned k = 0; k < elems && k < 16; k++) {
            GLint loc = uni->remap_location + (uni->array_elements ? (int)k : 0);
            fprintf(fp, "  %s%s loc=%d bindless=%d",
                    uni->name.string ? uni->name.string : "?",
                    uni->array_elements ? "[]" : "", loc,
                    uni->is_bindless ? 1 : 0);
            if (uni->is_bindless) {
               /* Bindless samplers hold a 64-bit texture handle (2 dwords),
                * not a unit; handle==0 is zink's reserved null (black) slot. */
               uint64_t handle = 0;
               if (samp_live)
                  memcpy(&handle, &samp_live[k * 2], sizeof(handle));
               fprintf(fp, " handle=0x%llx", (unsigned long long)handle);
            } else {
               int unit = samp_live ? samp_live[k].i : -1;
               fprintf(fp, " unit=%d", unit);
               if (unit >= 0 &&
                   unit < (int)ctx->Const.MaxCombinedTextureImageUnits) {
                  struct gl_texture_object *tex =
                     ctx->Texture.Unit[unit]._Current;
                  if (tex) {
                     struct gl_texture_image *img = tex->Image[0][0];
                     fprintf(fp, " -> Tex=%u target=0x%x %ux%u fmt=0x%x",
                             tex->Name, tex->Target,
                             img ? img->Width : 0, img ? img->Height : 0,
                             img ? img->InternalFormat : 0);
                  } else {
                     fprintf(fp, " -> <no complete texture on unit>");
                  }
               }
            }
            fprintf(fp, "\n");
         }
      }

      /* --- compact non-sampler default-block uniforms --- */
      fprintf(fp, "\n[uniforms]\n");
      for (unsigned u = 0; u < shProg->data->NumUniformStorage; u++) {
         struct gl_uniform_storage *uni = &shProg->data->UniformStorage[u];
         if (uni->block_index != -1 || uni->is_shader_storage ||
             !uni->type || !uni->storage)
            continue;
         enum glsl_base_type bt = glsl_get_base_type(uni->type);
         if (bt == GLSL_TYPE_SAMPLER || bt == GLSL_TYPE_TEXTURE ||
             bt == GLSL_TYPE_IMAGE)
            continue;
         unsigned total = glsl_get_components(uni->type);
         if (total > 8)
            total = 8;
         /* Read live values via a plain memory read (see sampler note above):
          * under PackedDriverUniformStorage uni->storage is stale for
          * non-opaque uniforms, so glUniform4fv uploads (e.g. textureRegion)
          * appeared as 0 here even though the driver received them. The live
          * copy for a non-opaque uniform is uni->driver_storage[0].data. */
         /* Samplers/textures/images were already skipped above, so every
          * uniform reaching here is non-opaque and uses packed storage. */
         const bool u_packed =
            ctx->Const.PackedDriverUniformStorage &&
            uni->num_driver_storage > 0 && uni->driver_storage[0].data;
         const union gl_constant_value *u_live =
            u_packed ? (const union gl_constant_value *)
                          uni->driver_storage[0].data
                     : uni->storage;
         fprintf(fp, "  %s loc=%d =", uni->name.string ? uni->name.string : "?",
                 uni->remap_location);
         for (unsigned k = 0; k < total; k++)
            fprintf(fp, " %.6g(0x%08x)", u_live ? u_live[k].f : 0.f,
                    u_live ? u_live[k].u : 0u);
         fprintf(fp, "\n");
      }
   } else {
      fprintf(fp, "  <no active program>\n");
   }

   /* --- VAO + current generic attribute fallbacks --- */
   struct gl_vertex_array_object *vao = ctx->Array.VAO;
   fprintf(fp, "\n[vao] Name=%u Enabled=0x%llx IndexBuf=%u\n",
           vao ? vao->Name : 0,
           vao ? (unsigned long long)vao->Enabled : 0ull,
           (vao && vao->IndexBufferObj) ? vao->IndexBufferObj->Name : 0);
   if (vao) {
      GLbitfield mask = vao->Enabled;
      while (mask) {
         const gl_vert_attrib i = u_bit_scan(&mask);
         const struct gl_array_attributes *array = &vao->VertexAttrib[i];
         const struct gl_vertex_buffer_binding *binding =
            &vao->BufferBinding[array->BufferBindingIndex];
         fprintf(fp, "  Attr[%d]: size=%d type=0x%x stride=%d reloff=%ld buf=%u\n",
                 i, array->Format.Size, array->Format.Type, binding->Stride,
                 (long)array->RelativeOffset,
                 binding->BufferObj ? binding->BufferObj->Name : 0);
      }
   }
   fprintf(fp, "  current generic attrib values (fallback when array disabled):\n");
   for (unsigned i = 0; i < 8; i++) {
      const GLfloat *v = ctx->Current.Attrib[VERT_ATTRIB_GENERIC(i)];
      fprintf(fp, "    generic[%u] = %.6g, %.6g, %.6g, %.6g\n",
              i, v[0], v[1], v[2], v[3]);
   }

   fclose(fp);
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
_mesa_set_draw_vao(struct gl_context *ctx, struct gl_vertex_array_object *vao,
                   GLbitfield filter)
{
   struct gl_vertex_array_object **ptr = &ctx->Array._DrawVAO;
   bool new_vertex_buffers = false, new_vertex_elements = false;

   if (*ptr != vao) {
      _mesa_reference_vao_(ctx, ptr, vao);
      new_vertex_buffers = true;
      new_vertex_elements = true;
   }

   if (vao->NewVertexBuffers || vao->NewVertexElements) {
      _mesa_update_vao_derived_arrays(ctx, vao);
      new_vertex_buffers |= vao->NewVertexBuffers;
      new_vertex_elements |= vao->NewVertexElements;
      vao->NewVertexBuffers = false;
      vao->NewVertexElements = false;
   }

   assert(vao->_EnabledWithMapMode ==
          _mesa_vao_enable_to_vp_inputs(vao->_AttributeMapMode, vao->Enabled));

   /* Filter out unwanted arrays. */
   const GLbitfield enabled = filter & vao->_EnabledWithMapMode;
   if (ctx->Array._DrawVAOEnabledAttribs != enabled) {
      ctx->Array._DrawVAOEnabledAttribs = enabled;
      new_vertex_buffers = true;
      new_vertex_elements = true;
   }

   if (new_vertex_buffers || new_vertex_elements) {
      ctx->NewDriverState |= ST_NEW_VERTEX_ARRAYS;
      ctx->Array.NewVertexElements |= new_vertex_elements;
   }

   _mesa_set_varying_vp_inputs(ctx, enabled);
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
#if DEBUG
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
   /* GL_UNSIGNED_BYTE  = 0x1401
    * GL_UNSIGNED_SHORT = 0x1403
    * GL_UNSIGNED_INT   = 0x1405
    *
    * The trick is that bit 1 and bit 2 mean USHORT and UINT, respectively.
    * After clearing those two bits (with ~6), we should get UBYTE.
    * Both bits can't be set, because the enum would be greater than UINT.
    */
   if (!(type <= GL_UNSIGNED_INT && (type & ~6) == GL_UNSIGNED_BYTE))
      return GL_INVALID_ENUM;

   return GL_NO_ERROR;
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
                                 GLsizei primcount)
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
   if (!ctx->Array.VAO->IndexBufferObj) {
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
   if (ctx->API != API_OPENGL_COMPAT &&
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
      switch (array->Format.Type) {
      case GL_FLOAT:
         {
            GLfloat *f = (GLfloat *) ((GLubyte *) data + binding->Stride * j);
            GLint k;
            for (k = 0; k < array->Format.Size; k++) {
               if (util_is_inf_or_nan(f[k]) || f[k] >= 1.0e20F || f[k] <= -1.0e10F) {
                  printf("Bad array data:\n");
                  printf("  Element[%u].%u = %f\n", j, k, f[k]);
                  printf("  Array %u at %p\n", attrib, (void *) array);
                  printf("  Type 0x%x, Size %d, Stride %d\n",
                         array->Format.Type, array->Format.Size,
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


static inline unsigned
get_index_size_shift(GLenum type)
{
   /* The type is already validated, so use a fast conversion.
    *
    * GL_UNSIGNED_BYTE  - GL_UNSIGNED_BYTE = 0
    * GL_UNSIGNED_SHORT - GL_UNSIGNED_BYTE = 2
    * GL_UNSIGNED_INT   - GL_UNSIGNED_BYTE = 4
    *
    * Divide by 2 to get 0,1,2.
    */
   return (type - GL_UNSIGNED_BYTE) >> 1;
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
         unreachable("Unexpected index buffer type");
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
             array->Format.Size, binding->Stride,
             array->Ptr, bufObj ? bufObj->Name : 0);

      if (bufObj) {
         GLubyte *p = bufObj->Mappings[MAP_INTERNAL].Pointer;
         int offset = (int) (GLintptr)
            _mesa_vertex_attrib_address(array, binding);

         unsigned multiplier;
         switch (array->Format.Type) {
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
            + array->Format.Size;
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
   info.take_index_buffer_ownership = false;
   info.index_bias_varies = false;
   /* Packed section end. */
   info.start_instance = baseInstance;
   info.instance_count = numInstances;
   info.view_mask = 0;
   info.min_index = start;
   info.max_index = start + count - 1;

   draw.start = start;
   draw.count = count;

   ctx->Driver.DrawGallium(ctx, &info, 0, &draw, 1);

#ifdef JUICE_MESA_DUMP_DRAW_STATE
   /* Dump comprehensive state after draw if enabled */
   dump_comprehensive_state(ctx, mode, start, count, numInstances, baseInstance);
   dump_combine_forensics(ctx, mode, start, count, numInstances);
#endif

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

   CALL_Begin(ctx->CurrentServerDispatch, (GL_QUADS));
   /* Begin can change CurrentServerDispatch. */
   struct _glapi_table *dispatch = ctx->CurrentServerDispatch;
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


   CALL_Begin(ctx->CurrentServerDispatch, (prim));
   /* Begin can change CurrentServerDispatch. */
   struct _glapi_table *dispatch = ctx->CurrentServerDispatch;
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
      CALL_Begin(ctx->CurrentServerDispatch, (GL_POINTS));
      /* Begin can change CurrentServerDispatch. */
      dispatch = ctx->CurrentServerDispatch;
      for (v = v1, j = j1; j <= j2; j++, v += dv) {
         for (u = u1, i = i1; i <= i2; i++, u += du) {
            CALL_EvalCoord2f(dispatch, (u, v));
         }
      }
      CALL_End(dispatch, ());
      break;
   case GL_LINE:
      for (v = v1, j = j1; j <= j2; j++, v += dv) {
         CALL_Begin(ctx->CurrentServerDispatch, (GL_LINE_STRIP));
         /* Begin can change CurrentServerDispatch. */
         dispatch = ctx->CurrentServerDispatch;
         for (u = u1, i = i1; i <= i2; i++, u += du) {
            CALL_EvalCoord2f(dispatch, (u, v));
         }
         CALL_End(dispatch, ());
      }
      for (u = u1, i = i1; i <= i2; i++, u += du) {
         CALL_Begin(ctx->CurrentServerDispatch, (GL_LINE_STRIP));
         /* Begin can change CurrentServerDispatch. */
         dispatch = ctx->CurrentServerDispatch;
         for (v = v1, j = j1; j <= j2; j++, v += dv) {
            CALL_EvalCoord2f(dispatch, (u, v));
         }
         CALL_End(dispatch, ());
      }
      break;
   case GL_FILL:
      for (v = v1, j = j1; j < j2; j++, v += dv) {
         CALL_Begin(ctx->CurrentServerDispatch, (GL_TRIANGLE_STRIP));
         /* Begin can change CurrentServerDispatch. */
         dispatch = ctx->CurrentServerDispatch;
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

   _mesa_set_draw_vao(ctx, ctx->Array.VAO,
                      ctx->VertexProgram._VPModeInputFilter);

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
   GET_CURRENT_CONTEXT(ctx);
   FLUSH_FOR_DRAW(ctx);

   _mesa_set_draw_vao(ctx, ctx->Array.VAO,
                      ctx->VertexProgram._VPModeInputFilter);

   if (ctx->NewState)
      _mesa_update_state(ctx);

   if (!_mesa_is_no_error_enabled(ctx) &&
       !_mesa_validate_DrawArraysInstanced(ctx, mode, start, count,
                                           numInstances))
      return;

   if (0)
      check_draw_arrays_data(ctx, start, count);

   _mesa_draw_arrays(ctx, mode, start, count, numInstances, 0);

   if (0)
      print_draw_arrays(ctx, mode, start, count);
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

   _mesa_set_draw_vao(ctx, ctx->Array.VAO,
                      ctx->VertexProgram._VPModeInputFilter);

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

   _mesa_set_draw_vao(ctx, ctx->Array.VAO,
                      ctx->VertexProgram._VPModeInputFilter);

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
   info.take_index_buffer_ownership = false;
   info.index_bias_varies = false;
   /* Packed section end. */
   info.start_instance = 0;
   info.instance_count = 1;
   info.view_mask = 0;

   for (int i = 0; i < primcount; i++) {
      draw[i].start = first[i];
      draw[i].count = count[i];
   }

   ctx->Driver.DrawGallium(ctx, &info, 0, draw, primcount);

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


/**
 * Inner support for both _mesa_DrawElements and _mesa_DrawRangeElements.
 * Do the rendering for a glDrawElements or glDrawRangeElements call after
 * we've validated buffer bounds, etc.
 */
static void
_mesa_validated_drawrangeelements(struct gl_context *ctx, GLenum mode,
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

   struct pipe_draw_info info;
   struct pipe_draw_start_count_bias draw;
   unsigned index_size_shift = get_index_size_shift(type);
   struct gl_buffer_object *index_bo = ctx->Array.VAO->IndexBufferObj;

   if (index_bo && !indices_aligned(index_size_shift, indices))
      return;

   info.mode = mode;
   info.index_size = 1 << index_size_shift;
   /* Packed section begin. */
   info.primitive_restart = ctx->Array._PrimitiveRestart[index_size_shift];
   info.has_user_indices = index_bo == NULL;
   info.index_bounds_valid = index_bounds_valid;
   info.increment_draw_id = false;
   info.was_line_loop = false;
   info.take_index_buffer_ownership = false;
   info.index_bias_varies = false;
   /* Packed section end. */
   info.start_instance = baseInstance;
   info.instance_count = numInstances;
   info.view_mask = 0;
   info.restart_index = ctx->Array._RestartIndex[index_size_shift];

   if (info.has_user_indices) {
      info.index.user = indices;
      draw.start = 0;
   } else {
      uintptr_t start = (uintptr_t) indices;
      if (unlikely(index_bo->Size < start)) {
         _mesa_warning(ctx, "Invalid indices offset 0x%" PRIxPTR
                            " (indices buffer size is %ld bytes)."
                            " Draw skipped.", start, index_bo->Size);
         return;
      }
      info.index.gl_bo = index_bo;
      draw.start = start >> index_size_shift;
   }
   draw.index_bias = basevertex;

   info.min_index = start;
   info.max_index = end;
   draw.count = count;

   /* Need to give special consideration to rendering a range of
    * indices starting somewhere above zero.  Typically the
    * application is issuing multiple DrawRangeElements() to draw
    * successive primitives layed out linearly in the vertex arrays.
    * Unless the vertex arrays are all in a VBO (or locked as with
    * CVA), the OpenGL semantics imply that we need to re-read or
    * re-upload the vertex data on each draw call.
    *
    * In the case of hardware tnl, we want to avoid starting the
    * upload at zero, as it will mean every draw call uploads an
    * increasing amount of not-used vertex data.  Worse - in the
    * software tnl module, all those vertices might be transformed and
    * lit but never rendered.
    *
    * If we just upload or transform the vertices in start..end,
    * however, the indices will be incorrect.
    *
    * At this level, we don't know exactly what the requirements of
    * the backend are going to be, though it will likely boil down to
    * either:
    *
    * 1) Do nothing, everything is in a VBO and is processed once
    *       only.
    *
    * 2) Adjust the indices and vertex arrays so that start becomes
    *    zero.
    *
    * Rather than doing anything here, I'll provide a helper function
    * for the latter case elsewhere.
    */

   ctx->Driver.DrawGallium(ctx, &info, 0, &draw, 1);

#ifdef JUICE_MESA_DUMP_DRAW_STATE
   /* JUICE: the environment/background dome (VRED prog 54) is an indexed mesh
    * drawn through this path, not the glDrawArrays path, so mirror the state
    * dump here to capture its OSGViewport/OSGEnvironment UBOs and envMap. */
   dump_comprehensive_state(ctx, mode, start, count, numInstances, baseInstance);
   dump_combine_forensics(ctx, mode, start, count, numInstances);
   ctx->_UniformBufferDataUpdated = false;
#endif

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

   _mesa_set_draw_vao(ctx, ctx->Array.VAO,
                      ctx->VertexProgram._VPModeInputFilter);

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

   _mesa_validated_drawrangeelements(ctx, mode, index_bounds_valid, start, end,
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
   GET_CURRENT_CONTEXT(ctx);
   FLUSH_FOR_DRAW(ctx);

   _mesa_set_draw_vao(ctx, ctx->Array.VAO,
                      ctx->VertexProgram._VPModeInputFilter);

   if (ctx->NewState)
      _mesa_update_state(ctx);

   if (!_mesa_is_no_error_enabled(ctx) &&
       !_mesa_validate_DrawElements(ctx, mode, count, type))
      return;

   _mesa_validated_drawrangeelements(ctx, mode, false, 0, ~0,
                                     count, type, indices, 0, 1, 0);
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

   _mesa_set_draw_vao(ctx, ctx->Array.VAO,
                      ctx->VertexProgram._VPModeInputFilter);

   if (ctx->NewState)
      _mesa_update_state(ctx);

   if (!_mesa_is_no_error_enabled(ctx) &&
       !_mesa_validate_DrawElements(ctx, mode, count, type))
      return;

   _mesa_validated_drawrangeelements(ctx, mode, false, 0, ~0,
                                     count, type, indices, basevertex, 1, 0);
}


/**
 * Called by glDrawElementsInstanced() in immediate mode.
 */
void GLAPIENTRY
_mesa_DrawElementsInstanced(GLenum mode, GLsizei count, GLenum type,
                            const GLvoid * indices, GLsizei numInstances)
{
   GET_CURRENT_CONTEXT(ctx);
   FLUSH_FOR_DRAW(ctx);

   _mesa_set_draw_vao(ctx, ctx->Array.VAO,
                      ctx->VertexProgram._VPModeInputFilter);

   if (ctx->NewState)
      _mesa_update_state(ctx);

   if (!_mesa_is_no_error_enabled(ctx) &&
       !_mesa_validate_DrawElementsInstanced(ctx, mode, count, type,
                                             numInstances))
      return;

   _mesa_validated_drawrangeelements(ctx, mode, false, 0, ~0,
                                     count, type, indices, 0, numInstances, 0);
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
   GET_CURRENT_CONTEXT(ctx);
   FLUSH_FOR_DRAW(ctx);

   _mesa_set_draw_vao(ctx, ctx->Array.VAO,
                      ctx->VertexProgram._VPModeInputFilter);

   if (ctx->NewState)
      _mesa_update_state(ctx);

   if (!_mesa_is_no_error_enabled(ctx) &&
       !_mesa_validate_DrawElementsInstanced(ctx, mode, count, type,
                                             numInstances))
      return;

   _mesa_validated_drawrangeelements(ctx, mode, false, 0, ~0,
                                     count, type, indices,
                                     basevertex, numInstances, 0);
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
   GET_CURRENT_CONTEXT(ctx);
   FLUSH_FOR_DRAW(ctx);

   _mesa_set_draw_vao(ctx, ctx->Array.VAO,
                      ctx->VertexProgram._VPModeInputFilter);

   if (ctx->NewState)
      _mesa_update_state(ctx);

   if (!_mesa_is_no_error_enabled(ctx) &&
       !_mesa_validate_DrawElementsInstanced(ctx, mode, count, type,
                                             numInstances))
      return;

   _mesa_validated_drawrangeelements(ctx, mode, false, 0, ~0,
                                     count, type, indices, 0, numInstances,
                                     baseInstance);
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

   _mesa_set_draw_vao(ctx, ctx->Array.VAO,
                      ctx->VertexProgram._VPModeInputFilter);

   if (ctx->NewState)
      _mesa_update_state(ctx);

   if (!_mesa_is_no_error_enabled(ctx) &&
       !_mesa_validate_DrawElementsInstanced(ctx, mode, count, type,
                                             numInstances))
      return;

   _mesa_validated_drawrangeelements(ctx, mode, false, 0, ~0,
                                     count, type, indices, basevertex,
                                     numInstances, baseInstance);
}


/**
 * Inner support for both _mesa_MultiDrawElements() and
 * _mesa_MultiDrawRangeElements().
 * This does the actual rendering after we've checked array indexes, etc.
 */
static void
_mesa_validated_multidrawelements(struct gl_context *ctx, GLenum mode,
                                  const GLsizei *count, GLenum type,
                                  const GLvoid * const *indices,
                                  GLsizei primcount, const GLint *basevertex)
{
   uintptr_t min_index_ptr, max_index_ptr;
   bool fallback = false;
   int i;

   if (primcount == 0)
      return;

   unsigned index_size_shift = get_index_size_shift(type);

   min_index_ptr = (uintptr_t) indices[0];
   max_index_ptr = 0;
   for (i = 0; i < primcount; i++) {
      min_index_ptr = MIN2(min_index_ptr, (uintptr_t) indices[i]);
      max_index_ptr = MAX2(max_index_ptr, (uintptr_t) indices[i] +
                           (count[i] << index_size_shift));
   }

   /* Check if we can handle this thing as a bunch of index offsets from the
    * same index pointer.  If we can't, then we have to fall back to doing
    * a draw_prims per primitive.
    * Check that the difference between each prim's indexes is a multiple of
    * the index/element size.
    */
   if (index_size_shift) {
      for (i = 0; i < primcount; i++) {
         if ((((uintptr_t) indices[i] - min_index_ptr) &
              ((1 << index_size_shift) - 1)) != 0) {
            fallback = true;
            break;
         }
      }
   }

   struct gl_buffer_object *index_bo = ctx->Array.VAO->IndexBufferObj;
   struct pipe_draw_info info;

   info.mode = mode;
   info.index_size = 1 << index_size_shift;
   /* Packed section begin. */
   info.primitive_restart = ctx->Array._PrimitiveRestart[index_size_shift];
   info.has_user_indices = index_bo == NULL;
   info.index_bounds_valid = false;
   info.increment_draw_id = primcount > 1;
   info.was_line_loop = false;
   info.take_index_buffer_ownership = false;
   info.index_bias_varies = !!basevertex;
   /* Packed section end. */
   info.start_instance = 0;
   info.instance_count = 1;
   info.view_mask = 0;
   info.restart_index = ctx->Array._RestartIndex[index_size_shift];

   if (info.has_user_indices)
      info.index.user = (void*)min_index_ptr;
   else
      info.index.gl_bo = index_bo;

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

      ctx->Driver.DrawGallium(ctx, &info, 0, draw, primcount);
   } else {
      /* draw[i].start would overflow. Draw one at a time. */
      assert(info.has_user_indices);
      info.increment_draw_id = false;

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

         ctx->Driver.DrawGallium(ctx, &info, i, &draw, 1);
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

   _mesa_set_draw_vao(ctx, ctx->Array.VAO,
                      ctx->VertexProgram._VPModeInputFilter);

   if (ctx->NewState)
      _mesa_update_state(ctx);

   if (!_mesa_is_no_error_enabled(ctx) &&
       !_mesa_validate_MultiDrawElements(ctx, mode, count, type, indices,
                                         primcount))
      return;

   _mesa_validated_multidrawelements(ctx, mode, count, type, indices, primcount,
                                     NULL);
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

   _mesa_set_draw_vao(ctx, ctx->Array.VAO,
                      ctx->VertexProgram._VPModeInputFilter);

   if (ctx->NewState)
      _mesa_update_state(ctx);

   if (!_mesa_is_no_error_enabled(ctx) &&
       !_mesa_validate_MultiDrawElements(ctx, mode, count, type, indices,
                                         primcount))
      return;

   _mesa_validated_multidrawelements(ctx, mode, count, type, indices, primcount,
                                     basevertex);
}


/**
 * Draw a GL primitive using a vertex count obtained from transform feedback.
 * \param mode  the type of GL primitive to draw
 * \param obj  the transform feedback object to use
 * \param stream  index of the transform feedback stream from which to
 *                get the primitive count.
 * \param numInstances  number of instances to draw
 */
static void
_mesa_draw_transform_feedback(struct gl_context *ctx, GLenum mode,
                              struct gl_transform_feedback_object *obj,
                              GLuint stream, GLuint numInstances)
{
   FLUSH_FOR_DRAW(ctx);

   _mesa_set_draw_vao(ctx, ctx->Array.VAO,
                      ctx->VertexProgram._VPModeInputFilter);

   if (ctx->NewState)
      _mesa_update_state(ctx);

   if (!_mesa_is_no_error_enabled(ctx) &&
       !_mesa_validate_DrawTransformFeedback(ctx, mode, obj, stream,
                                             numInstances))
      return;

   /* Maybe we should do some primitive splitting for primitive restart
    * (like in DrawArrays), but we have no way to know how many vertices
    * will be rendered. */

   st_draw_transform_feedback(ctx, mode, numInstances, stream, obj);

   if (MESA_DEBUG_FLAGS & DEBUG_ALWAYS_FLUSH) {
      _mesa_flush(ctx);
   }
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
   GET_CURRENT_CONTEXT(ctx);
   struct gl_transform_feedback_object *obj =
      _mesa_lookup_transform_feedback_object(ctx, name);

   _mesa_draw_transform_feedback(ctx, mode, obj, 0, 1);
}


void GLAPIENTRY
_mesa_DrawTransformFeedbackStream(GLenum mode, GLuint name, GLuint stream)
{
   GET_CURRENT_CONTEXT(ctx);
   struct gl_transform_feedback_object *obj =
      _mesa_lookup_transform_feedback_object(ctx, name);

   _mesa_draw_transform_feedback(ctx, mode, obj, stream, 1);
}


void GLAPIENTRY
_mesa_DrawTransformFeedbackInstanced(GLenum mode, GLuint name,
                                     GLsizei primcount)
{
   GET_CURRENT_CONTEXT(ctx);
   struct gl_transform_feedback_object *obj =
      _mesa_lookup_transform_feedback_object(ctx, name);

   _mesa_draw_transform_feedback(ctx, mode, obj, 0, primcount);
}


void GLAPIENTRY
_mesa_DrawTransformFeedbackStreamInstanced(GLenum mode, GLuint name,
                                           GLuint stream,
                                           GLsizei primcount)
{
   GET_CURRENT_CONTEXT(ctx);
   struct gl_transform_feedback_object *obj =
      _mesa_lookup_transform_feedback_object(ctx, name);

   _mesa_draw_transform_feedback(ctx, mode, obj, stream, primcount);
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
   if (ctx->API == API_OPENGL_COMPAT &&
       !ctx->DrawIndirectBuffer) {
      DrawArraysIndirectCommand *cmd = (DrawArraysIndirectCommand *) indirect;

      _mesa_DrawArraysInstancedBaseInstance(mode, cmd->first, cmd->count,
                                            cmd->primCount,
                                            cmd->baseInstance);
      return;
   }

   FLUSH_FOR_DRAW(ctx);

   _mesa_set_draw_vao(ctx, ctx->Array.VAO,
                      ctx->VertexProgram._VPModeInputFilter);

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
   if (ctx->API == API_OPENGL_COMPAT &&
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

   _mesa_set_draw_vao(ctx, ctx->Array.VAO,
                      ctx->VertexProgram._VPModeInputFilter);

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

   _mesa_set_draw_vao(ctx, ctx->Array.VAO,
                      ctx->VertexProgram._VPModeInputFilter);

   if (ctx->NewState)
      _mesa_update_state(ctx);

   /* From the ARB_draw_indirect spec:
    *
    *    "Initially zero is bound to DRAW_INDIRECT_BUFFER. In the
    *    compatibility profile, this indicates that DrawArraysIndirect and
    *    DrawElementsIndirect are to source their arguments directly from the
    *    pointer passed as their <indirect> parameters."
    */
   if (ctx->API == API_OPENGL_COMPAT &&
       !ctx->DrawIndirectBuffer) {

      if (!_mesa_is_no_error_enabled(ctx) &&
          (!_mesa_valid_draw_indirect_multi(ctx, primcount, stride,
                                           "glMultiDrawArraysIndirect") ||
           !_mesa_validate_DrawArrays(ctx, mode, 1)))
         return;

      struct pipe_draw_info info;
      info.mode = mode;
      info.index_size = 0;
      info.view_mask = 0;
      /* Packed section begin. */
      info.primitive_restart = false;
      info.has_user_indices = false;
      info.index_bounds_valid = false;
      info.increment_draw_id = primcount > 1;
      info.was_line_loop = false;
      info.take_index_buffer_ownership = false;
      info.index_bias_varies = false;
      /* Packed section end. */

      const uint8_t *ptr = (const uint8_t *) indirect;
      for (unsigned i = 0; i < primcount; i++) {
         DrawArraysIndirectCommand *cmd = (DrawArraysIndirectCommand *) ptr;

         info.start_instance = cmd->baseInstance;
         info.instance_count = cmd->primCount;

         struct pipe_draw_start_count_bias draw;
         draw.start = cmd->first;
         draw.count = cmd->count;

         ctx->Driver.DrawGallium(ctx, &info, i, &draw, 1);
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

   _mesa_set_draw_vao(ctx, ctx->Array.VAO,
                      ctx->VertexProgram._VPModeInputFilter);

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
   if (ctx->API == API_OPENGL_COMPAT &&
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

      unsigned index_size_shift = get_index_size_shift(type);

      struct pipe_draw_info info;
      info.mode = mode;
      info.index_size = 1 << index_size_shift;
      info.view_mask = 0;
      /* Packed section begin. */
      info.primitive_restart = ctx->Array._PrimitiveRestart[index_size_shift];
      info.has_user_indices = false;
      info.index_bounds_valid = false;
      info.increment_draw_id = primcount > 1;
      info.was_line_loop = false;
      info.take_index_buffer_ownership = false;
      info.index_bias_varies = false;
      /* Packed section end. */
      info.restart_index = ctx->Array._RestartIndex[index_size_shift];

      const uint8_t *ptr = (const uint8_t *) indirect;
      for (unsigned i = 0; i < primcount; i++) {
         DrawElementsIndirectCommand *cmd = (DrawElementsIndirectCommand*)ptr;

         info.index.gl_bo = ctx->Array.VAO->IndexBufferObj;
         info.start_instance = cmd->baseInstance;
         info.instance_count = cmd->primCount;

         struct pipe_draw_start_count_bias draw;
         draw.start = cmd->firstIndex;
         draw.count = cmd->count;
         draw.index_bias = cmd->baseVertex;

         ctx->Driver.DrawGallium(ctx, &info, i, &draw, 1);
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

   _mesa_set_draw_vao(ctx, ctx->Array.VAO,
                      ctx->VertexProgram._VPModeInputFilter);

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

   _mesa_set_draw_vao(ctx, ctx->Array.VAO,
                      ctx->VertexProgram._VPModeInputFilter);

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
         CALL_DrawArrays(ctx->CurrentServerDispatch, ( m, first[i], count[i] ));
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
         CALL_DrawElements(ctx->CurrentServerDispatch, ( m, count[i], type,
                                                         indices[i] ));
      }
   }
}
