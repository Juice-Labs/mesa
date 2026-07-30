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
#include <wchar.h>
#endif

// Wine/Juice logging support - similar to vkd3d and dxvk
typedef int (*PFN_juice_log)(const char *);
/* Exported by RemoteGPUVlk.dll next to __wine_dbg_output, which can only log at
 * Debug. Both initialize the client, so either is safe as the first call in. */
typedef int (*PFN_juice_get_log_level)(void);
typedef int (*PFN_juice_log_output)(int level, const char *);
static PFN_juice_log juice_log_output = NULL;
static PFN_juice_get_log_level juice_get_log_level = NULL;
static PFN_juice_log_output juice_log_level_output = NULL;
static int juice_log_initialized = 0;

/* Cached because the client sets it once at startup and never changes it, which
 * keeps the per-message filter an integer compare - zink calls mesa_logi() on
 * per-draw paths. -1 means no Juice client, in which case nothing is filtered. */
static int juice_log_level = -1;

/* Levels as Juice_GetLogLevel() reports them and Juice_LogOutput() takes them.
 * A level is enabled when it is <= the configured one. */
#define JUICE_LOG_LEVEL_FATAL   0
#define JUICE_LOG_LEVEL_NOTIFY  1
#define JUICE_LOG_LEVEL_ERROR   2
#define JUICE_LOG_LEVEL_WARNING 3
#define JUICE_LOG_LEVEL_INFO    4
#define JUICE_LOG_LEVEL_DEBUG   5
#define JUICE_LOG_LEVEL_TRACE   9

#if defined(_WIN32)
/* Load RemoteGPUVlk.dll: already loaded, then the default search path, then the
 * graphics/ directory of the install root this DLL sits under. */
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

/* MESA_LOG_INFO is deliberately mapped to Debug, not Info: mesa_logi() is not
 * informational in this tree, zink uses it for per-draw tracing at tens of
 * thousands of lines a frame. */
static int
level_to_juice_level(enum mesa_log_level l)
{
   switch (l) {
   case MESA_LOG_ERROR: return JUICE_LOG_LEVEL_ERROR;
   case MESA_LOG_WARN:  return JUICE_LOG_LEVEL_WARNING;
   case MESA_LOG_INFO:  return JUICE_LOG_LEVEL_DEBUG;
   case MESA_LOG_DEBUG: return JUICE_LOG_LEVEL_DEBUG;
   }
   return JUICE_LOG_LEVEL_DEBUG;
}

static const char *
juice_level_name(int level)
{
   switch (level) {
   case JUICE_LOG_LEVEL_FATAL:   return "Fatal";
   case JUICE_LOG_LEVEL_NOTIFY:  return "Notify";
   case JUICE_LOG_LEVEL_ERROR:   return "Error";
   case JUICE_LOG_LEVEL_WARNING: return "Warning";
   case JUICE_LOG_LEVEL_INFO:    return "Info";
   case JUICE_LOG_LEVEL_DEBUG:   return "Debug";
   case 6:                       return "Debug1";
   case 7:                       return "Debug2";
   case 8:                       return "Debug3";
   case JUICE_LOG_LEVEL_TRACE:   return "Trace";
   default:                      return "unknown";
   }
}

static bool
juice_sink_active(void)
{
   return juice_log_level_output != NULL || juice_log_output != NULL;
}

/* Juice_LogOutput records the level itself, so text going there must not
 * repeat it. */
static bool
juice_leveled_sink_active(void)
{
   return juice_log_level_output != NULL;
}

/* Announce the level mesa is filtering against, so a missing message can be
 * told apart from a suppressed one. */
static void
report_juice_log_level(void)
{
   if (juice_log_level < 0) {
      /* Logged as an error precisely because that is the only thing
       * mesa_log_level_enabled() still lets through in this case. */
      if (juice_sink_active())
         mesa_log(MESA_LOG_ERROR, MESA_LOG_TAG,
                  "JUICE logging: Juice_GetLogLevel unavailable, "
                  "limiting mesa logging to errors");
      return;
   }

   /* Probed rather than derived, so it stays right as level_to_juice_level()
    * changes - MESA_LOG_INFO does not map to Info. */
   enum mesa_log_level banner = MESA_LOG_ERROR;
   if (mesa_log_level_enabled(MESA_LOG_DEBUG))
      banner = MESA_LOG_DEBUG;
   else if (mesa_log_level_enabled(MESA_LOG_INFO))
      banner = MESA_LOG_INFO;
   else if (mesa_log_level_enabled(MESA_LOG_WARN))
      banner = MESA_LOG_WARN;

   mesa_log(banner, MESA_LOG_TAG,
            "logging enabled: client level=%s(%d), sink=%s"
            " -> mesa error=%s warn=%s info=%s debug=%s",
            juice_level_name(juice_log_level), juice_log_level,
            juice_log_level_output ? "Juice_LogOutput"
                                   : "__wine_dbg_output (all messages filed as Debug)",
            mesa_log_level_enabled(MESA_LOG_ERROR) ? "on" : "off",
            mesa_log_level_enabled(MESA_LOG_WARN)  ? "on" : "off",
            mesa_log_level_enabled(MESA_LOG_INFO)  ? "on" : "off",
            mesa_log_level_enabled(MESA_LOG_DEBUG) ? "on" : "off");
}

/* Stock mesa's log destination (docs/envvars.rst). When set it takes over
 * completely: no Juice routing and no level filtering. */
static FILE *
mesa_log_file_override(void)
{
   static FILE *file;
   static bool checked;

   if (!checked) {
      const char *path = getenv("MESA_LOG_FILE");
      if (path && *path)
         file = fopen(path, "w");
      checked = true;
   }
   return file;
}

static void
init_juice_logging(void)
{
   if (juice_log_initialized)
      return;

   /* Leaving every juice_* pointer NULL is what disables the routing. */
   if (mesa_log_file_override()) {
      juice_log_initialized = 1;
      return;
   }

#if defined(_WIN32)
   HMODULE juicevlk = mesa_juice_load_remote_gpu_vlk();
   if (juicevlk) {
      juice_log_output = (PFN_juice_log)GetProcAddress(juicevlk, "__wine_dbg_output");
      juice_get_log_level = (PFN_juice_get_log_level)
         GetProcAddress(juicevlk, "Juice_GetLogLevel");
      juice_log_level_output = (PFN_juice_log_output)
         GetProcAddress(juicevlk, "Juice_LogOutput");
   }

   /* Must be called even when the level is not wanted: it is what brings the
    * client up. Until it has been, the level reads back as the lowest one and
    * everything mesa logs is filtered out. */
   if (juice_get_log_level)
      juice_log_level = juice_get_log_level();
#endif

   /* Set before reporting: the report logs, and every log re-enters here. */
   juice_log_initialized = 1;

   report_juice_log_level();
}

bool
mesa_log_level_enabled(enum mesa_log_level level)
{
   init_juice_logging();

   if (juice_log_level >= 0)
      return level_to_juice_level(level) <= juice_log_level;

   /* Level unknown, ie an ICD too old to export Juice_GetLogLevel. Keep errors
    * so real failures still surface, but not zink's per-draw spew. With no
    * Juice sink there is nothing to flood and stock mesa never filtered. */
   if (juice_sink_active())
      return level == MESA_LOG_ERROR;

   return true;
}

static void
mesa_log_emit(enum mesa_log_level level, const char *text)
{
   if (juice_log_level_output) {
      juice_log_level_output(level_to_juice_level(level), text);
      return;
   }

   /* Older ICD without Juice_LogOutput: everything lands at Debug. */
   if (juice_log_output) {
      juice_log_output(text);
      return;
   }

   FILE *out = mesa_log_file_override();
   if (!out)
      out = stderr;
   fputs(text, out);
   fflush(out);
}

void
mesa_log_write(enum mesa_log_level level, const char *text)
{
   if (!mesa_log_level_enabled(level))
      return;

   mesa_log_emit(level, text);
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

   /* Filtered before formatting: zink calls mesa_logi() per draw, and the two
    * formatting passes below are not worth paying to then discard. */
   if (!mesa_log_level_enabled(level))
      return;

   // Format the message part first
   vsnprintf(msg, sizeof(msg), format, va);

   /* Only the non-leveled sinks need the level spelled out in the text. */
   if (juice_leveled_sink_active()) {
      snprintf(buf, sizeof(buf), "%s: %s", tag, msg);
   } else {
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
   }

   mesa_log_emit(level, buf);
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
