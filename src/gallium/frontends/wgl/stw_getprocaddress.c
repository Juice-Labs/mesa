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
#include <GL/mesa_glinterop.h>

#include "glapi/glapi/glapi.h"
#include "stw_device.h"
#include "stw_gdishim.h"
#include "gldrv.h"
#include "stw_nopfuncs.h"

#include "util/u_debug.h"

/* Stub function for WGL_NV_copy_image */
BOOL WINAPI
wglCopyImageSubDataNV(HGLRC hSrcRC, GLuint srcName, GLenum srcTarget,
                      GLint srcLevel, GLint srcX, GLint srcY, GLint srcZ,
                      HGLRC hDstRC, GLuint dstName, GLenum dstTarget,
                      GLint dstLevel, GLint dstX, GLint dstY, GLint dstZ,
                      GLsizei width, GLsizei height, GLsizei depth)
{
   debug_printf("wglCopyImageSubDataNV: Not implemented, fatal error\n");
   assert(0);
   return FALSE;
}

/* Stub functions for WGL_NV_gpu_affinity */
HDC WINAPI
wglCreateAffinityDCNV(const HGPUNV *gpuList)
{
   debug_printf("wglCreateAffinityDCNV: Not implemented, fatal error\n");
   assert(0);
   return NULL;
}

BOOL WINAPI
wglDeleteDCNV(HDC hdc)
{
   debug_printf("wglDeleteDCNV: Not implemented, fatal error\n");
   assert(0);
   return FALSE;
}

BOOL WINAPI
wglEnumGpusNV(UINT iGpuIndex, HGPUNV *phGpu)
{
   debug_printf("wglEnumGpusNV: Not implemented, fatal error\n");
   assert(0);
   return FALSE;
}

BOOL WINAPI
wglEnumGpuDevicesNV(HGPUNV hGpu, UINT iDeviceIndex, PGPU_DEVICE lpGpuDevice)
{
   debug_printf("wglEnumGpuDevicesNV: Not implemented, fatal error\n");
   assert(0);
   return FALSE;
}

BOOL WINAPI
wglEnumGpusFromAffinityDCNV(HDC hAffinityDC, UINT iGpuIndex, HGPUNV *phGpu)
{
   debug_printf("wglEnumGpusFromAffinityDCNV: Not implemented, fatal error\n");
   assert(0);
   return FALSE;
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
   _mesa_GenSemaphoresEXT(n, semaphores);
}

VOID WINAPI
glDepthRangedNV(GLdouble zNear, GLdouble zFar)
{
   debug_printf("glDepthRangedNV: Not implemented, fatal error\n");
   assert(0);
}

VOID WINAPI
glGetNamedBufferParameterui64vNV(GLuint buffer, GLenum pname, GLuint64EXT *params)
{
   debug_printf("glGetNamedBufferParameterui64vNV: Not implemented\n");
   //assert(0);
}

GLuint64 WINAPI
glGetTextureSamplerHandleNV(GLuint texture, GLuint sampler)
{
   debug_printf("glGetTextureSamplerHandleNV: Not implemented\n");
   //assert(0);
   return 0;
}

GLboolean WINAPI
glIsNamedBufferResidentNV(GLuint buffer)
{
   debug_printf("glIsNamedBufferResidentNV: Not implemented\n");
   //assert(0);
   return FALSE;
}

VOID WINAPI
glMakeNamedBufferResidentNV(GLuint buffer, GLenum access)
{
   debug_printf("glMakeNamedBufferResidentNV: Not implemented\n");
   //assert(0);
}

VOID WINAPI
glMakeNamedBufferNonResidentNV(GLuint buffer)
{
   debug_printf("glMakeNamedBufferNonResidentNV: Not implemented\n");
   //assert(0);
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
   debug_printf("glSemaphoreParameterivNV: Not implemented\n");
   //assert(0);
}

VOID WINAPI
glGetSemaphoreParameterivNV(GLuint semaphore, GLenum pname, GLint *params)
{
   debug_printf("glGetSemaphoreParameterivNV: Not implemented, fatal error\n");
   //assert(0);
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

   /* Unnamed */
   STW_EXTENSION_ENTRY( wglMesaGLInteropQueryDeviceInfo ),
   STW_EXTENSION_ENTRY( wglMesaGLInteropExportObject ),
   STW_EXTENSION_ENTRY( wglMesaGLInteropFlushObjects ),
   { NULL, NULL }
};

static const struct stw_extension_entry stw_gl_extension_entries[] = {
   /* GL_NV_command_list */
   STW_EXTENSION_ENTRY( glBufferAddressRangeNV ),
   STW_EXTENSION_ENTRY( glCreateSemaphoresNV ),
   STW_EXTENSION_ENTRY( glDepthRangedNV ),
   STW_EXTENSION_ENTRY( glGetNamedBufferParameterui64vNV ),
   STW_EXTENSION_ENTRY( glGetTextureSamplerHandleNV ),
   STW_EXTENSION_ENTRY( glIsNamedBufferResidentNV ),
   STW_EXTENSION_ENTRY( glMakeNamedBufferResidentNV ),
   STW_EXTENSION_ENTRY( glMakeNamedBufferNonResidentNV ),
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

      p = (PROC) _mesa_glapi_get_proc_address(lpszProc);
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
