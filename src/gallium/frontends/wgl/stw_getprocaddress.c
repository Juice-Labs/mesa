/**************************************************************************
 *
 * Copyright 2008 VMware, Inc.
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

#include <windows.h>

#define WGL_WGLEXT_PROTOTYPES

#include <GL/gl.h>
#include <GL/wglext.h>

#include "glapi/glapi.h"
#include "stw_device.h"
#include "gldrv.h"
#include "stw_nopfuncs.h"

#include "util/u_debug.h"
#include "util/log.h"
#include "pipe/p_screen.h"
#include "pipe/p_context.h"
#include "main/context.h"
#include "main/externalobjects.h"
#include "state_tracker/st_context.h"
#include "stw_context.h"

/* NV_timeline_semaphore enum definitions */
#define GL_SEMAPHORE_TYPE_NV                            0x95B3
#define GL_SEMAPHORE_TYPE_BINARY_NV                     0x95B4
#define GL_SEMAPHORE_TYPE_TIMELINE_NV                   0x95B5
#define GL_TIMELINE_SEMAPHORE_VALUE_NV                  0x9595
#define GL_MAX_TIMELINE_SEMAPHORE_VALUE_DIFFERENCE_NV   0x95B6

/* Helper function to get GL context from HGLRC handle */
static struct gl_context* 
stw_get_gl_context_from_hglrc(HGLRC hglrc)
{
   if (!stw_dev) {
      mesa_log(MESA_LOG_ERROR, "WGL", "wglCopyImageSubDataNV: No STW device available");
      return NULL;
   }
   
   if (!hglrc) {
      /* NULL HGLRC means use current context */
      return _mesa_get_current_context();
   }
   
   /* Convert HGLRC to DHGLRC */
   DHGLRC dhglrc = (DHGLRC)(UINT_PTR)hglrc;
   
   /* Look up the stw_context */
   struct stw_context *stw_ctx = stw_lookup_context(dhglrc);
   if (!stw_ctx) {
      mesa_log(MESA_LOG_ERROR, "WGL", "wglCopyImageSubDataNV: Invalid HGLRC handle: %p", hglrc);
      return NULL;
   }
   
   /* Get the state tracker interface */
   struct st_context_iface *st = stw_ctx->st;
   if (!st) {
      mesa_log(MESA_LOG_ERROR, "WGL", "wglCopyImageSubDataNV: No state tracker interface in context");
      return NULL;
   }
   
   /* The Mesa GL context is stored in the state tracker context */
   struct st_context *st_ctx = (struct st_context*)st;
   if (!st_ctx) {
      mesa_log(MESA_LOG_ERROR, "WGL", "wglCopyImageSubDataNV: No state tracker context");
      return NULL;
   }
   
   struct gl_context *gl_ctx = st_ctx->ctx;
   if (!gl_ctx) {
      mesa_log(MESA_LOG_ERROR, "WGL", "wglCopyImageSubDataNV: No Mesa GL context in state tracker");
      return NULL;
   }
   
   return gl_ctx;
}

/* Implementation for WGL_NV_copy_image */
BOOL WINAPI
wglCopyImageSubDataNV(HGLRC hSrcRC, GLuint srcName, GLenum srcTarget,
                      GLint srcLevel, GLint srcX, GLint srcY, GLint srcZ,
                      HGLRC hDstRC, GLuint dstName, GLenum dstTarget,
                      GLint dstLevel, GLint dstX, GLint dstY, GLint dstZ,
                      GLsizei width, GLsizei height, GLsizei depth)
{
   struct gl_context *src_ctx = NULL;
   struct gl_context *dst_ctx = NULL;
   BOOL result = FALSE;
   
   mesa_log(MESA_LOG_INFO, "WGL", "wglCopyImageSubDataNV called (src=%p, dst=%p)", hSrcRC, hDstRC);
   
   if (!stw_dev) {
      mesa_log(MESA_LOG_ERROR, "WGL", "wglCopyImageSubDataNV: No STW device");
      return FALSE;
   }
   
   /* Convert HGLRC handles to Mesa GL contexts */
   src_ctx = stw_get_gl_context_from_hglrc(hSrcRC);
   dst_ctx = stw_get_gl_context_from_hglrc(hDstRC);
   
   /* Validate that we have contexts */
   if (!src_ctx) {
      mesa_log(MESA_LOG_ERROR, "WGL", "wglCopyImageSubDataNV: Invalid or missing source GL context");
      return FALSE;
   }
   
   if (!dst_ctx) {
      mesa_log(MESA_LOG_ERROR, "WGL", "wglCopyImageSubDataNV: Invalid or missing destination GL context");
      return FALSE;
   }
   
   /* Validate parameters */
   if (width <= 0 || height <= 0 || depth <= 0) {
      mesa_log(MESA_LOG_ERROR, "WGL", "wglCopyImageSubDataNV: Invalid dimensions");
      return FALSE;
   }
   
   /* Check if the NV_copy_image extension is supported in source context */
   if (!src_ctx->Extensions.NV_copy_image) {
      mesa_log(MESA_LOG_ERROR, "WGL", "wglCopyImageSubDataNV: NV_copy_image extension not available in source context");
      return FALSE;
   }
   
   /* For cross-context copying, we need to ensure both contexts support the operation */
   if (src_ctx != dst_ctx && !dst_ctx->Extensions.NV_copy_image) {
      mesa_log(MESA_LOG_ERROR, "WGL", "wglCopyImageSubDataNV: NV_copy_image extension not available in destination context");
      return FALSE;
   }
   
   /* Set up an error handler to catch Mesa errors */
   GLenum saved_error = src_ctx->ErrorValue;
   src_ctx->ErrorValue = GL_NO_ERROR;

   extern bool zink_copy_image_subdata_nv_cross_context(struct gl_context *src_ctx,
                                                         struct gl_context *dst_ctx,
                                                         uint32_t srcName, uint32_t srcTarget,
                                                         int32_t srcLevel, int32_t srcX, int32_t srcY, int32_t srcZ,
                                                         uint32_t dstName, uint32_t dstTarget,
                                                         int32_t dstLevel, int32_t dstX, int32_t dstY, int32_t dstZ,
                                                         int32_t width, int32_t height, int32_t depth);

   bool ok = zink_copy_image_subdata_nv_cross_context(src_ctx, dst_ctx,
                                                      srcName, srcTarget,
                                                      srcLevel, srcX, srcY, srcZ,
                                                      dstName, dstTarget,
                                                      dstLevel, dstX, dstY, dstZ,
                                                      width, height, depth);
   result = ok ? TRUE : FALSE;
  
   return result;
}

/* Stub functions for WGL_NV_gpu_affinity */
HDC WINAPI
wglCreateAffinityDCNV(const HGPUNV *phGpuList)
{
   if (!stw_dev) {
      mesa_log(MESA_LOG_ERROR, "WGL", "wglCreateAffinityDCNV: No STW device");
      return NULL;
   }

   /* Only support affinity DC creation when using Zink */
   if (!stw_dev->zink) {
      mesa_log(MESA_LOG_ERROR, "WGL", "wglCreateAffinityDCNV: Not using Zink driver");
      return NULL;
   }

   if (!phGpuList) {
      mesa_log(MESA_LOG_ERROR, "WGL", "wglCreateAffinityDCNV: NULL GPU list");
      return NULL;
   }

   /* Forward to Zink implementation */
   extern HDC zink_misc_create_affinity_dc(const HGPUNV *gpu_list);
   HDC result = zink_misc_create_affinity_dc(phGpuList);
   
   if (result) {
      mesa_log(MESA_LOG_INFO, "WGL", "wglCreateAffinityDCNV: Created affinity DC %p", result);
   } else {
      mesa_log(MESA_LOG_ERROR, "WGL", "wglCreateAffinityDCNV: Failed to create affinity DC");
   }
   
   return result;
}

BOOL WINAPI
wglDeleteDCNV(HDC hdc)
{
   if (!stw_dev) {
      mesa_log(MESA_LOG_ERROR, "WGL", "wglDeleteDCNV: No STW device");
      return FALSE;
   }

   /* Only support affinity DC deletion when using Zink */
   if (!stw_dev->zink) {
      mesa_log(MESA_LOG_ERROR, "WGL", "wglDeleteDCNV: Not using Zink driver");
      return FALSE;
   }

   if (!hdc) {
      mesa_log(MESA_LOG_ERROR, "WGL", "wglDeleteDCNV: NULL HDC");
      return FALSE;
   }

   /* Forward to Zink implementation */
   extern bool zink_misc_delete_affinity_dc(HDC hdc);
   bool result = zink_misc_delete_affinity_dc(hdc);
   
   if (result) {
      debug_printf("wglDeleteDCNV: Deleted affinity DC %p\n", hdc);
   } else {
      debug_printf("wglDeleteDCNV: Failed to delete affinity DC %p\n", hdc);
   }
   
   return result ? TRUE : FALSE;
}

BOOL WINAPI
wglEnumGpusNV(UINT iGpuIndex, HGPUNV *phGpu)
{
   if (!stw_dev) {
      debug_printf("wglEnumGpusNV: No STW device\n");
      return FALSE;
   }

   /* Only support GPU enumeration when using Zink */
   if (!stw_dev->zink) {
      debug_printf("wglEnumGpusNV: Not using Zink driver\n");
      return FALSE;
   }

   if (!phGpu) {
      debug_printf("wglEnumGpusNV: NULL phGpu pointer\n");
      return FALSE;
   }

   /* Forward to Zink implementation - will be linked if Zink is available */
   extern bool zink_misc_enum_gpus(uint32_t gpu_index, HGPUNV *gpu_handle);
   if (zink_misc_enum_gpus(iGpuIndex, phGpu)) {
      return TRUE;
   } else {
      /* Index out of range or other error */
      return FALSE;
   }
}

BOOL WINAPI
wglEnumGpuDevicesNV(HGPUNV hGpu, UINT iDeviceIndex, PGPU_DEVICE lpGpuDevice)
{
   if (!stw_dev) {
      debug_printf("wglEnumGpuDevicesNV: No STW device\n");
      return FALSE;
   }

   /* Only support GPU device enumeration when using Zink */
   if (!stw_dev->zink) {
      debug_printf("wglEnumGpuDevicesNV: Not using Zink driver\n");
      return FALSE;
   }

   if (!hGpu || !lpGpuDevice) {
      debug_printf("wglEnumGpuDevicesNV: Invalid parameters\n");
      return FALSE;
   }

   /* Forward to Zink implementation */
   extern bool zink_misc_enum_gpu_devices(HGPUNV gpu_handle, uint32_t device_index, PGPU_DEVICE gpu_device);
   if (zink_misc_enum_gpu_devices(hGpu, iDeviceIndex, lpGpuDevice)) {
      return TRUE;
   } else {
      /* Device index out of range or other error */
      return FALSE;
   }
}

BOOL WINAPI
wglEnumGpusFromAffinityDCNV(HDC hAffinityDC, UINT iGpuIndex, HGPUNV *phGpu)
{
   if (!stw_dev) {
      debug_printf("wglEnumGpusFromAffinityDCNV: No STW device\n");
      return FALSE;
   }

   /* Only support GPU enumeration when using Zink */
   if (!stw_dev->zink) {
      debug_printf("wglEnumGpusFromAffinityDCNV: Not using Zink driver\n");
      return FALSE;
   }

   if (!hAffinityDC || !phGpu) {
      debug_printf("wglEnumGpusFromAffinityDCNV: Invalid parameters\n");
      return FALSE;
   }

   /* Forward to Zink implementation */
   extern bool zink_misc_enum_gpus_from_affinity_dc(HDC hdc, uint32_t gpu_index, HGPUNV *gpu_handle);
   if (zink_misc_enum_gpus_from_affinity_dc(hAffinityDC, iGpuIndex, phGpu)) {
      return TRUE;
   } else {
      /* GPU index out of range or affinity DC not found */
      return FALSE;
   }
}

/* Stub functions for WGL_NV_DX_interop */
HANDLE WINAPI
wglDXOpenDeviceNV(void *dxDevice)
{
   debug_printf("wglDXOpenDeviceNV: Not implemented, fatal error\n");
   assert(0);
   return NULL;
}

BOOL WINAPI
wglDXCloseDeviceNV(HANDLE hDevice)
{
   debug_printf("wglDXCloseDeviceNV: Not implemented, fatal error\n");
   assert(0);
   return FALSE;
}

HANDLE WINAPI
wglDXRegisterObjectNV(HANDLE hDevice, void *dxObject, GLuint name, GLenum type, GLenum access)
{
   debug_printf("wglDXRegisterObjectNV: Not implemented, fatal error\n");
   assert(0);
   return NULL;
}

BOOL WINAPI
wglDXUnregisterObjectNV(HANDLE hDevice, HANDLE hObject)
{
   debug_printf("wglDXUnregisterObjectNV: Not implemented, fatal error\n");
   assert(0);
   return FALSE;
}

BOOL WINAPI
wglDXObjectAccessNV(HANDLE hObject, GLenum access)
{
   debug_printf("wglDXObjectAccessNV: Not implemented, fatal error\n");
   assert(0);
   return FALSE;
}

BOOL WINAPI
wglDXLockObjectsNV(HANDLE hDevice, GLint count, HANDLE *hObjects)
{
   debug_printf("wglDXLockObjectsNV: Not implemented, fatal error\n");
   assert(0);
   return FALSE;
}

BOOL WINAPI
wglDXUnlockObjectsNV(HANDLE hDevice, GLint count, HANDLE *hObjects)
{
   debug_printf("wglDXUnlockObjectsNV: Not implemented, fatal error\n");
   assert(0);
   return FALSE;
}

BOOL WINAPI
wglDXSetResourceShareHandleNV(void *dxObject, HANDLE shareHandle)
{
   debug_printf("wglDXSetResourceShareHandleNV: Not implemented, fatal error\n");
   assert(0);
   return FALSE;
}

VOID WINAPI
glBufferAddressRangeNV(GLenum target, GLuint index, GLuint64EXT address, GLsizeiptr length)
{
   debug_printf("glBufferAddressRangeNV: Not implemented, fatal error\n");
   assert(0);
}

VOID WINAPI
glCreateSemaphoresNV(GLsizei n, GLuint *semaphores)
{
   GET_CURRENT_CONTEXT(ctx);
   
   if (!ctx) {
      debug_printf("glCreateSemaphoresNV: No current context\n");
      return;
   }
   
   /* First generate the semaphore objects */
   _mesa_GenSemaphoresEXT(n, semaphores);
   
   /* The NV spec doesn't set a default type, but it's a good practice to
    * initialize them as TIMELINE semaphores since that's the extension's purpose */
   for (GLsizei i = 0; i < n; i++) {
      if (semaphores[i] > 0) {
         /* This will set the default type to timeline */
         GLint params = GL_SEMAPHORE_TYPE_TIMELINE_NV;
         _mesa_SemaphoreParameterivNV(semaphores[i], GL_SEMAPHORE_TYPE_NV, &params);
      }
   }
}

VOID WINAPI
glDepthRangedNV(GLdouble zNear, GLdouble zFar)
{
   debug_printf("glDepthRangedNV: Not implemented, fatal error\n");
   assert(0);
}

VOID WINAPI
glGetBufferParameterui64vNV(GLenum target, GLenum pname, GLuint64EXT *params)
{
   (void)target;
   (void)pname;
   if (params)
      *params = 0;
}

VOID WINAPI
glGetNamedBufferParameterui64vNV(GLuint buffer, GLenum pname, GLuint64EXT *params)
{
   (void)buffer;
   (void)pname;
   if (params)
      *params = 0;
}

VOID WINAPI
glGetIntegerui64vNV(GLenum value, GLuint64EXT *result)
{
   (void)value;
   if (result)
      *result = 0;
}

VOID WINAPI
glGetUniformui64vNV(GLuint program, GLint location, GLuint64EXT *params)
{
   (void)program;
   (void)location;
   if (params)
      *params = 0;
}

VOID WINAPI
glProgramUniformui64NV(GLuint program, GLint location, GLuint64EXT value)
{
   (void)program;
   (void)location;
   (void)value;
}

VOID WINAPI
glProgramUniformui64vNV(GLuint program, GLint location, GLsizei count,
                        const GLuint64EXT *value)
{
   (void)program;
   (void)location;
   (void)count;
   (void)value;
}

GLuint64 WINAPI
glGetTextureSamplerHandleNV(GLuint texture, GLuint sampler)
{
   debug_printf("glGetTextureSamplerHandleNV: Not implemented\n");
   //assert(0);
   return 0;
}

GLboolean WINAPI
glIsBufferResidentNV(GLenum target)
{
   (void)target;
   return FALSE;
}

GLboolean WINAPI
glIsNamedBufferResidentNV(GLuint buffer)
{
   (void)buffer;
   return FALSE;
}

VOID WINAPI
glMakeBufferResidentNV(GLenum target, GLenum access)
{
   (void)target;
   (void)access;
}

VOID WINAPI
glMakeBufferNonResidentNV(GLenum target)
{
   (void)target;
}

VOID WINAPI
glMakeNamedBufferResidentNV(GLuint buffer, GLenum access)
{
   (void)buffer;
   (void)access;
}

VOID WINAPI
glMakeNamedBufferNonResidentNV(GLuint buffer)
{
   (void)buffer;
}

VOID WINAPI
glMakeTextureHandleNonResidentNV(GLuint64 handle)
{
   debug_printf("glMakeTextureHandleNonResidentNV: Not implemented\n");
   //assert(0);
}

VOID WINAPI
glMakeTextureHandleResidentNV(GLuint64 handle)
{
   debug_printf("glMakeTextureHandleResidentNV: Not implemented\n");
   //assert(0);
}

VOID WINAPI
glSemaphoreParameterivNV(GLuint semaphore, GLenum pname, const GLint *params)
{
   GET_CURRENT_CONTEXT(ctx);
   
   if (!ctx) {
      debug_printf("glSemaphoreParameterivNV: No current context\n");
      return;
   }

   if (pname == GL_SEMAPHORE_TYPE_NV || pname == GL_TIMELINE_SEMAPHORE_VALUE_NV) {
      /* Let the Mesa implementation handle these properly */
      _mesa_SemaphoreParameterivNV(semaphore, pname, params);
   } else {
      debug_printf("glSemaphoreParameterivNV: Invalid parameter name\n");
   }
}

VOID WINAPI
glGetSemaphoreParameterivNV(GLuint semaphore, GLenum pname, GLint *params)
{
   GET_CURRENT_CONTEXT(ctx);
   
   if (!ctx) {
      debug_printf("glGetSemaphoreParameterivNV: No current context\n");
      return;
   }

   if (pname == GL_SEMAPHORE_TYPE_NV || pname == GL_TIMELINE_SEMAPHORE_VALUE_NV) {
      /* Let the Mesa implementation handle these properly */
      _mesa_GetSemaphoreParameterivNV(semaphore, pname, params);
   } else {
      debug_printf("glGetSemaphoreParameterivNV: Invalid parameter name\n");
   }
}

VOID WINAPI
glUniformui64NV(GLint location, GLuint64EXT value)
{
   debug_printf("glUniformui64NV: Not implemented\n");
   //assert(0);
}

VOID WINAPI
glUniformui64vNV(GLint location, GLsizei count, const GLuint64EXT *value)
{
   debug_printf("glUniformui64vNV: Not implemented\n");
   //assert(0);
}

struct stw_extension_entry
{
   const char *name;
   PROC proc;
};

#define STW_EXTENSION_ENTRY(P) { #P, (PROC) P }

static const struct stw_extension_entry stw_extension_entries[] = {

   /* WGL_ARB_extensions_string */
   STW_EXTENSION_ENTRY( wglGetExtensionsStringARB ),

   /* WGL_ARB_pbuffer */
   STW_EXTENSION_ENTRY( wglCreatePbufferARB ),
   STW_EXTENSION_ENTRY( wglGetPbufferDCARB ),
   STW_EXTENSION_ENTRY( wglReleasePbufferDCARB ),
   STW_EXTENSION_ENTRY( wglDestroyPbufferARB ),
   STW_EXTENSION_ENTRY( wglQueryPbufferARB ),

   /* WGL_ARB_pixel_format */
   STW_EXTENSION_ENTRY( wglChoosePixelFormatARB ),
   STW_EXTENSION_ENTRY( wglGetPixelFormatAttribfvARB ),
   STW_EXTENSION_ENTRY( wglGetPixelFormatAttribivARB ),

   /* WGL_EXT_extensions_string */
   STW_EXTENSION_ENTRY( wglGetExtensionsStringEXT ),

   /* WGL_EXT_swap_control */
   STW_EXTENSION_ENTRY( wglGetSwapIntervalEXT ),
   STW_EXTENSION_ENTRY( wglSwapIntervalEXT ),

   /* WGL_ARB_create_context */
   STW_EXTENSION_ENTRY( wglCreateContextAttribsARB ),

   /* WGL_ARB_render_texture */
   STW_EXTENSION_ENTRY( wglBindTexImageARB ),
   STW_EXTENSION_ENTRY( wglReleaseTexImageARB ),
   STW_EXTENSION_ENTRY( wglSetPbufferAttribARB ),

   /*  WGL_ARB_make_current_read */
   STW_EXTENSION_ENTRY( wglMakeContextCurrentARB ),
   STW_EXTENSION_ENTRY( wglGetCurrentReadDCARB ),

   /* WGL_NV_copy_image */
   STW_EXTENSION_ENTRY( wglCopyImageSubDataNV ),

   /* WGL_NV_gpu_affinity */
   STW_EXTENSION_ENTRY( wglCreateAffinityDCNV ),
   STW_EXTENSION_ENTRY( wglDeleteDCNV ),
   STW_EXTENSION_ENTRY( wglEnumGpusNV ),
   STW_EXTENSION_ENTRY( wglEnumGpuDevicesNV ),
   STW_EXTENSION_ENTRY( wglEnumGpusFromAffinityDCNV ),

   /* WGL_NV_DX_interop */
   STW_EXTENSION_ENTRY( wglDXOpenDeviceNV ),
   STW_EXTENSION_ENTRY( wglDXCloseDeviceNV ),
   STW_EXTENSION_ENTRY( wglDXRegisterObjectNV ),
   STW_EXTENSION_ENTRY( wglDXUnregisterObjectNV ),
   STW_EXTENSION_ENTRY( wglDXObjectAccessNV ),
   STW_EXTENSION_ENTRY( wglDXLockObjectsNV ),
   STW_EXTENSION_ENTRY( wglDXUnlockObjectsNV ),
   STW_EXTENSION_ENTRY( wglDXSetResourceShareHandleNV ),
   { NULL, NULL }
};

static const struct stw_extension_entry stw_gl_extension_entries[] = {
   /* GL_NV_command_list */
   STW_EXTENSION_ENTRY( glBufferAddressRangeNV ),
   STW_EXTENSION_ENTRY( glCreateSemaphoresNV ),
   STW_EXTENSION_ENTRY( glDepthRangedNV ),
   STW_EXTENSION_ENTRY( glGetBufferParameterui64vNV ),
   STW_EXTENSION_ENTRY( glGetNamedBufferParameterui64vNV ),
   STW_EXTENSION_ENTRY( glGetIntegerui64vNV ),
   STW_EXTENSION_ENTRY( glGetUniformui64vNV ),
   STW_EXTENSION_ENTRY( glIsBufferResidentNV ),
   STW_EXTENSION_ENTRY( glIsNamedBufferResidentNV ),
   STW_EXTENSION_ENTRY( glMakeBufferResidentNV ),
   STW_EXTENSION_ENTRY( glMakeBufferNonResidentNV ),
   STW_EXTENSION_ENTRY( glMakeNamedBufferResidentNV ),
   STW_EXTENSION_ENTRY( glMakeNamedBufferNonResidentNV ),
   STW_EXTENSION_ENTRY( glProgramUniformui64NV ),
   STW_EXTENSION_ENTRY( glProgramUniformui64vNV ),
   STW_EXTENSION_ENTRY( glGetTextureSamplerHandleNV ),
   STW_EXTENSION_ENTRY( glMakeTextureHandleNonResidentNV ),
   STW_EXTENSION_ENTRY( glMakeTextureHandleResidentNV ),
   STW_EXTENSION_ENTRY( glSemaphoreParameterivNV ),
   STW_EXTENSION_ENTRY( glGetSemaphoreParameterivNV ),
   STW_EXTENSION_ENTRY( glUniformui64NV ),
   STW_EXTENSION_ENTRY( glUniformui64vNV ),
   { NULL, NULL }
};   

PROC APIENTRY
DrvGetProcAddress(
   LPCSTR lpszProc )
{
   const struct stw_extension_entry *entry;
   PROC p;

   if (!stw_dev)
      return NULL;

   if (lpszProc[0] == 'w' && lpszProc[1] == 'g' && lpszProc[2] == 'l')
      for (entry = stw_extension_entries; entry->name; entry++)
         if (strcmp( lpszProc, entry->name ) == 0)
            return entry->proc;

   if (lpszProc[0] == 'g' && lpszProc[1] == 'l') {

      for (entry = stw_gl_extension_entries; entry->name; entry++)
         if (strcmp( lpszProc, entry->name ) == 0)
            return entry->proc;

      p = (PROC) _glapi_get_proc_address(lpszProc);
      if (p)
         return p;
   }

   /* If we get here, we'd normally just return NULL, but since some apps
    * (like Viewperf12) crash when they try to use the null pointer, try
    * returning a pointer to a no-op function instead.
    */
   p = stw_get_nop_function(lpszProc);
   if (p) {
      debug_printf("wglGetProcAddress(\"%s\") returning no-op function\n",
                   lpszProc);
      return p;
   }

   debug_printf("wglGetProcAddress(\"%s\") returning NULL\n", lpszProc);
   return NULL;
}
