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

#if defined(_WIN32)
#include <windows.h>
#endif

// Wine/Juice logging support - similar to vkd3d and dxvk
typedef int (*PFN_juice_log)(const char *);
static PFN_juice_log juice_log_output = NULL;
static int juice_log_initialized = 0;

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
   // Try to get juice logging from RemoteGPUVlk.dll
   const char* juiceLib = "RemoteGPUVlk.dll";
   HMODULE juicevlk = LoadLibraryA(juiceLib);
   if (juicevlk)
      juice_log_output = (PFN_juice_log)GetProcAddress(juicevlk, "__wine_dbg_output");

   // THROWAWAY HOST-PASSTHROUGH TEST BRANCH:
   // RemoteGPUVlk.dll is intentionally blocked from loading in passthrough mode, so juice
   // log forwarding is simply unavailable. Fall back silently (juice_log_output stays NULL)
   // instead of popping a modal MessageBox on every process.
   
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
   
   // If juice logging is not available, don't output anything
   if (!juice_log_output)
      return;
   
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
   
   // Output through juice logging
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
