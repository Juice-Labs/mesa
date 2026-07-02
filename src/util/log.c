/*
 * Copyright © 2017 Google, Inc.
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
 */

#include <stdarg.h>

#ifdef ANDROID
#include <android/log.h>
#else
#include <stdio.h>
#endif

#include <stdlib.h>
#include <string.h>
#include "util/detect_os.h"
#include "util/log.h"
#include "util/ralloc.h"
#include "util/simple_mtx.h"

#if defined(_WIN32)
#include <windows.h>
#include <wchar.h>
#endif

#if defined(_WIN32)
/* Self-contained diagnostic log sink.
 *
 * Normally mesa's log output is forwarded to the Juice logging DLL
 * (__wine_dbg_output in RemoteGPUVlk.dll). When mesa/zink is run "passthrough"
 * directly on a native Vulkan driver (Juice's Vulkan stack bypassed), that DLL
 * is not loaded, so juice_log_output is NULL and mesa would otherwise emit
 * nothing. To keep diagnostics available in that mode, every mesa_log message
 * is additionally written to c:\temp\mesa_zink.log. The file is truncated once
 * per process on first use and flushed per line so output survives a crash. */
static simple_mtx_t mesa_file_log_mtx = SIMPLE_MTX_INITIALIZER;
static FILE *mesa_file_log_fp;
static int mesa_file_log_tried;

static void
mesa_file_log_write(const char *buf)
{
   simple_mtx_lock(&mesa_file_log_mtx);
   if (!mesa_file_log_tried) {
      mesa_file_log_tried = 1;
      mesa_file_log_fp = fopen("c:\\temp\\mesa_zink.log", "w");
   }
   if (mesa_file_log_fp) {
      fputs(buf, mesa_file_log_fp);
      fflush(mesa_file_log_fp);
   }
   simple_mtx_unlock(&mesa_file_log_mtx);
}
#endif

// Wine/Juice logging support - similar to vkd3d and dxvk
typedef int (*PFN_juice_log)(const char *);
static PFN_juice_log juice_log_output = NULL;
static int juice_log_initialized = 0;

#if defined(_WIN32)
/* Load RemoteGPUVlk.dll the same way as KnownPaths (shim dir / graphics / DLL). */
HMODULE
mesa_juice_load_remote_gpu_vlk(void)
{
   static HMODULE s_mod;
   static int s_tried;
   if (s_tried)
      return s_mod;
   s_tried = 1;

   s_mod = GetModuleHandleA("RemoteGPUVlk.dll");
   if (s_mod)
      return s_mod;

   s_mod = LoadLibraryA("RemoteGPUVlk.dll");
   if (s_mod)
      return s_mod;

   {
      HMODULE self = NULL;
      static int s_addr_cookie;
      if (!GetModuleHandleExA(
             GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
             (LPCSTR)&s_addr_cookie, &self))
         return NULL;

      wchar_t module_path[MAX_PATH + 1];
      if (!GetModuleFileNameW(self, module_path, MAX_PATH))
         return NULL;
      module_path[MAX_PATH] = 0;

      /* Directory containing this DLL (e.g. .../graphics) */
      {
         wchar_t *last = wcsrchr(module_path, L'\\');
         if (!last)
            last = wcsrchr(module_path, L'/');
         if (!last)
            return NULL;
         *last = 0;
      }

      /* If that folder is x86, compute, or graphics, use parent (install root). */
      {
         wchar_t *last = wcsrchr(module_path, L'\\');
         if (!last)
            last = wcsrchr(module_path, L'/');
         const wchar_t *leaf = last ? last + 1 : module_path;
         if (!_wcsicmp(leaf, L"x86") || !_wcsicmp(leaf, L"compute") ||
             !_wcsicmp(leaf, L"graphics")) {
            if (last)
               *last = 0;
         }
      }

      {
         wchar_t full_path[MAX_PATH + 40];
         const wchar_t *suffix = L"\\graphics\\RemoteGPUVlk.dll";
         size_t n = wcslen(module_path);
         size_t slen = wcslen(suffix);
         if (n + slen + 1 > sizeof(full_path) / sizeof(full_path[0]))
            return NULL;
         memcpy(full_path, module_path, n * sizeof(wchar_t));
         memcpy(full_path + n, suffix, (slen + 1) * sizeof(wchar_t));
         s_mod = LoadLibraryW(full_path);
      }
   }
   return s_mod;
}
#endif

#ifdef ANDROID
static inline android_LogPriority
level_to_android(enum mesa_log_level l)
{
   switch (l) {
   case MESA_LOG_ERROR: return ANDROID_LOG_ERROR;
   case MESA_LOG_WARN: return ANDROID_LOG_WARN;
   case MESA_LOG_INFO: return ANDROID_LOG_INFO;
   case MESA_LOG_DEBUG: return ANDROID_LOG_DEBUG;
   }

   unreachable("bad mesa_log_level");
}
#endif

#ifndef ANDROID
static inline const char *
level_to_str(enum mesa_log_level l)
{
   switch (l) {
   case MESA_LOG_ERROR: return "error";
   case MESA_LOG_WARN: return "warning";
   case MESA_LOG_INFO: return "info";
   case MESA_LOG_DEBUG: return "debug";
   }

   unreachable("bad mesa_log_level");
}
#endif

static void
init_juice_logging(void)
{
   if (juice_log_initialized)
      return;
      
#if defined(_WIN32)
   HMODULE juicevlk = mesa_juice_load_remote_gpu_vlk();
   if (juicevlk)
      juice_log_output = (PFN_juice_log)GetProcAddress(juicevlk, "__wine_dbg_output");
#endif
   
   juice_log_initialized = 1;
}

void
mesa_log(enum mesa_log_level level, const char *tag, const char *format, ...)
{
   va_list va;

   va_start(va, format);
   mesa_log_v(level, tag, format, va);
   va_end(va);
}

void
mesa_log_v(enum mesa_log_level level, const char *tag, const char *format,
            va_list va)
{
   char buf[4096];  // Buffer for the complete log message
   char msg[4096];  // Buffer for the formatted message part
   
   // Initialize juice logging if not already done
   init_juice_logging();
   
   // Format the message part first
   vsnprintf(msg, sizeof(msg), format, va);
   
   // Create the complete log message with tag and level
#ifdef ANDROID
   snprintf(buf, sizeof(buf), "%s: %s: %s", tag, 
           level == MESA_LOG_ERROR ? "error" :
           level == MESA_LOG_WARN ? "warning" :
           level == MESA_LOG_INFO ? "info" : "debug", msg);
#else
   snprintf(buf, sizeof(buf), "%s: %s: %s", tag, level_to_str(level), msg);
#endif
   
   // Ensure newline at end if not present
   size_t len = strlen(buf);
   if (len > 0 && buf[len - 1] != '\n') {
      if (len < sizeof(buf) - 1) {
         buf[len] = '\n';
         buf[len + 1] = '\0';
      }
   }
   
#if defined(_WIN32)
   // Always mirror to the self-contained c:\temp sink so diagnostics survive
   // even when the Juice logging DLL isn't loaded (native-Vulkan passthrough).
   mesa_file_log_write(buf);
#endif
   
   // Output through juice logging when available
   if (juice_log_output)
      juice_log_output(buf);
}

struct log_stream *
_mesa_log_stream_create(enum mesa_log_level level, char *tag)
{
   struct log_stream *stream = ralloc(NULL, struct log_stream);
   stream->level = level;
   stream->tag = tag;
   stream->msg = ralloc_strdup(stream, "");
   stream->pos = 0;
   return stream;
}

void
mesa_log_stream_destroy(struct log_stream *stream)
{
   /* If you left trailing stuff in the log stream, flush it out as a line. */
   if (stream->pos != 0)
      mesa_log(stream->level, stream->tag, "%s", stream->msg);

   ralloc_free(stream);
}

static void
mesa_log_stream_flush(struct log_stream *stream, size_t scan_offset)
{
   char *end;
   char *next = stream->msg;
   while ((end = strchr(stream->msg + scan_offset, '\n'))) {
      *end = 0;
      mesa_log(stream->level, stream->tag, "%s", next);
      next = end + 1;
      scan_offset = next - stream->msg;
   }
   if (next != stream->msg) {
      /* Clear out the lines we printed and move any trailing chars to the start. */
      size_t remaining = stream->msg + stream->pos - next;
      memmove(stream->msg, next, remaining);
      stream->pos = remaining;
   }
}

void mesa_log_stream_printf(struct log_stream *stream, const char *format, ...)
{
   size_t old_pos = stream->pos;

   va_list va;
   va_start(va, format);
   ralloc_vasprintf_rewrite_tail(&stream->msg, &stream->pos, format, va);
   va_end(va);

   mesa_log_stream_flush(stream, old_pos);
}

void
_mesa_log_multiline(enum mesa_log_level level, const char *tag, const char *lines)
{
   struct log_stream tmp = {
      .level = level,
      .tag = tag,
      .msg = strdup(lines),
      .pos = strlen(lines),
   };
   mesa_log_stream_flush(&tmp, 0);
   free(tmp.msg);
}
