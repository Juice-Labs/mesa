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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "c11/threads.h"
#include "util/detect_os.h"
#include "util/log.h"
#include "util/ralloc.h"
#include "util/u_atomic.h"
#include "util/u_debug.h"

#if DETECT_OS_POSIX
#include <syslog.h>
#include "util/u_process.h"
#endif

#if DETECT_OS_ANDROID
#include <android/log.h>
#endif

#if DETECT_OS_WINDOWS
#include <windows.h>
#include <wchar.h>


/* Exported by RemoteGPUVlk.dll next to __wine_dbg_output, which can only log at
 * Debug. Both initialize the client, so either is safe as the first call in. */
typedef int (*PFN_juice_get_log_level)(void);
typedef int (*PFN_juice_log_output)(int level, const char *);
static PFN_juice_get_log_level juice_get_log_level = NULL;
static PFN_juice_log_output juice_log_level_output = NULL;

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

#endif /* DETECT_OS_WINDOWS */

/* Wine/Juice logging support - similar to vkd3d and dxvk */
typedef int (*PFN_juice_log)(const char *);
static PFN_juice_log juice_log_output = NULL;

static void mesa_log_init(void);

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
   /* Leaving every juice_* pointer NULL is what disables the routing. */
   if (mesa_log_file_override())
      return;

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
}

bool
mesa_log_level_enabled(enum mesa_log_level level)
{
   mesa_log_init();

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

enum mesa_log_control {
   MESA_LOG_CONTROL_NULL = 1 << 0,
   MESA_LOG_CONTROL_FILE = 1 << 1,
   MESA_LOG_CONTROL_SYSLOG = 1 << 2,
   MESA_LOG_CONTROL_ANDROID = 1 << 3,
   MESA_LOG_CONTROL_WINDBG = 1 << 4,
   MESA_LOG_CONTROL_JUICE = 1 << 5,
   MESA_LOG_CONTROL_LOGGER_MASK = 0xff,

   MESA_LOG_CONTROL_WAIT = 1 << 8,
};

enum logger_vasnprintf_affix {
   LOGGER_VASNPRINTF_AFFIX_TAG = 1 << 0,
   LOGGER_VASNPRINTF_AFFIX_LEVEL = 1 << 1,
   LOGGER_VASNPRINTF_AFFIX_NEWLINE = 1 << 2,
};

static const struct debug_control mesa_log_control_options[] = {
   /* loggers */
   { "null", MESA_LOG_CONTROL_NULL },
   { "file", MESA_LOG_CONTROL_FILE },
   { "syslog", MESA_LOG_CONTROL_SYSLOG },
   { "android", MESA_LOG_CONTROL_ANDROID },
   { "windbg", MESA_LOG_CONTROL_WINDBG },
   { "juice", MESA_LOG_CONTROL_JUICE },
   /* flags */
   { "wait", MESA_LOG_CONTROL_WAIT },
   { NULL, 0 },
};

static inline const char *
level_to_str(enum mesa_log_level l)
{
   switch (l) {
   case MESA_LOG_ERROR: return "error";
   case MESA_LOG_WARN: return "warning";
   case MESA_LOG_INFO: return "info";
   case MESA_LOG_DEBUG: return "debug";
   case MESA_NUM_LOG_LEVELS:
      break;
   }

   UNREACHABLE("bad mesa_log_level");
}

static enum mesa_log_level
level_from_str(const char *str)
{
   for (unsigned l = 0; l < MESA_NUM_LOG_LEVELS; l++) {
      if (strcmp(level_to_str(l), str) == 0)
         return l;
   }

   return MESA_NUM_LOG_LEVELS;
}

static uint32_t mesa_log_control;
static FILE *mesa_log_file;
static enum mesa_log_level mesa_max_log_level;
static int mesa_log_file_affixes;

static const struct debug_named_value log_prefix_options[] = {
   {"tag",     LOGGER_VASNPRINTF_AFFIX_TAG,     "include log tag"},
   {"level",   LOGGER_VASNPRINTF_AFFIX_LEVEL,   "include log level"},
   DEBUG_NAMED_VALUE_END
};

static void
mesa_log_init_once(void)
{
   mesa_log_control = parse_debug_string(os_get_option("MESA_LOG"),
         mesa_log_control_options);

#if DETECT_OS_WINDOWS
   init_juice_logging();
#endif

   if (!(mesa_log_control & MESA_LOG_CONTROL_LOGGER_MASK)) {
      /* pick the default loggers */
#if DETECT_OS_ANDROID
      mesa_log_control |= MESA_LOG_CONTROL_ANDROID;
#elif DETECT_OS_WINDOWS
      mesa_log_control |= MESA_LOG_CONTROL_JUICE;
#else
      mesa_log_control |= MESA_LOG_CONTROL_FILE;
#endif
   }

   mesa_max_log_level = MESA_DEFAULT_LOG_LEVEL;
   const char *log_level = os_get_option("MESA_LOG_LEVEL");
   if (log_level != NULL)
      mesa_max_log_level = level_from_str(log_level);

   mesa_log_file_affixes =
      debug_get_flags_option("MESA_LOG_PREFIX", log_prefix_options,
                             LOGGER_VASNPRINTF_AFFIX_TAG | LOGGER_VASNPRINTF_AFFIX_LEVEL);

   mesa_log_file = stderr;

#if !DETECT_OS_WINDOWS
   if (__normal_user()) {
      FILE *fp = NULL;

      if (os_get_option("MESA_LOG_FILE_AUTO")) {
         char log_file[512];
         int fd;

         snprintf(log_file, sizeof(log_file), "/tmp/mesa_%s_%d_XXXXXX.log", util_get_process_name(),
                  getpid());
         fd = mkstemps(log_file, 4);
         if (fd >= 0)
            fp = fdopen(fd, "w");
      } else {
         const char *log_file = os_get_option("MESA_LOG_FILE");
         if (log_file)
            fp = fopen(log_file, "w");
      }

      if (fp) {
         mesa_log_file = fp;
         mesa_log_control |= MESA_LOG_CONTROL_FILE;
      }
   }
#endif

#if DETECT_OS_POSIX
   if (mesa_log_control & MESA_LOG_CONTROL_SYSLOG)
      openlog(util_get_process_name(), LOG_NDELAY | LOG_PID, LOG_USER);
#endif
}

static void
mesa_log_init(void)
{
   static once_flag once = ONCE_FLAG_INIT;
   call_once(&once, mesa_log_init_once);

   /*
    * Report the Juice log level, outside of the once_flag above because
    * it calls mesa_log() which re-enters mesa_log_init() and deadlock.
    */
   static int juice_level_reported;
   if (unlikely(!p_atomic_read(&juice_level_reported)) &&
       p_atomic_cmpxchg(&juice_level_reported, 0, 1) == 0)
      report_juice_log_level();
}

static void
mesa_warn_invalid_level_once(void)
{
   const char *log_level = os_get_option("MESA_LOG_LEVEL");
   mesa_logw("Invalid log level: \"%s\"", log_level);
}

static void
mesa_warn_invalid_level(void)
{
   static once_flag once = ONCE_FLAG_INIT;
   call_once(&once, mesa_warn_invalid_level_once);
}

void
mesa_log_if_debug(enum mesa_log_level level, const char *outputString)
{
   static int debug = -1;

   /* Init the local 'debug' var once. */
   if (debug == -1) {
      const char *env = os_get_option("MESA_DEBUG");
      bool silent = env && strstr(env, "silent") != NULL;
#ifndef NDEBUG
      /* in debug builds, print messages unless MESA_DEBUG="silent" */
      if (silent)
         debug = 0;
      else
         debug = 1;
#else
      /* in release builds, print messages if any MESA_DEBUG value other than
       * MESA_DEBUG="silent" is set
       */
      debug = env && !silent;
#endif
   }

   /* Now only print the string if we're required to do so. */
   if (debug)
      mesa_log(level, "Mesa", "%s", outputString);
}

/* Try vsnprintf first and fall back to vasprintf if buf is too small.  This
 * function handles all errors and never fails.
 */
static char *
logger_vasnprintf(char *buf,
                  int size,
                  int affixes,
                  enum mesa_log_level level,
                  const char *tag,
                  const char *format,
                  va_list in_va)
{
   struct {
      char *cur;
      int rem;
      int total;
      bool invalid;
   } state = {
      .cur = buf,
      .rem = size,
   };

   va_list va;
   va_copy(va, in_va);

#define APPEND(state, func, ...)                                     \
   do {                                                              \
      int ret = func(state.cur, state.rem, __VA_ARGS__);             \
      if (ret < 0) {                                                 \
         state.invalid = true;                                       \
      }  else {                                                      \
         state.total += ret;                                         \
         if (ret >= state.rem)                                       \
            ret = state.rem;                                         \
         state.cur += ret;                                           \
         state.rem -= ret;                                           \
      }                                                              \
   } while (false)

   if (affixes & LOGGER_VASNPRINTF_AFFIX_TAG)
      APPEND(state, snprintf, "%s: ", tag);
   if (affixes & LOGGER_VASNPRINTF_AFFIX_LEVEL)
      APPEND(state, snprintf, "%s: ", level_to_str(level));

   APPEND(state, vsnprintf, format, va);

   if (affixes & LOGGER_VASNPRINTF_AFFIX_NEWLINE) {
      if (state.cur == buf || state.cur[-1] != '\n')
         APPEND(state, snprintf, "\n");
   }
#undef APPEND

   assert(size >= 64);
   if (state.invalid) {
      strncpy(buf, "invalid message format", size);
   } else if (state.total >= size) {
      /* print again into alloc to avoid truncation */
      void *alloc = malloc(state.total + 1);
      if (alloc) {
         buf = logger_vasnprintf(alloc, state.total + 1, affixes, level, tag,
               format, in_va);
         assert(buf == alloc);
      } else {
         /* pretty-truncate the message */
         strncpy(buf + size - 4, "...", 4);
      }
   }

   va_end(va);

   return buf;
}

static void
logger_file(enum mesa_log_level level,
            const char *tag,
            const char *format,
            va_list va)
{
   FILE *fp = mesa_log_file;
   char local_msg[1024];
   char *msg = logger_vasnprintf(local_msg, sizeof(local_msg),
         mesa_log_file_affixes |
         LOGGER_VASNPRINTF_AFFIX_NEWLINE,
         level, tag, format, va);

   fprintf(fp, "%s", msg);
   fflush(fp);

   if (msg != local_msg)
      free(msg);
}

#if DETECT_OS_POSIX

static inline int
level_to_syslog(enum mesa_log_level l)
{
   switch (l) {
   case MESA_LOG_ERROR: return LOG_ERR;
   case MESA_LOG_WARN: return LOG_WARNING;
   case MESA_LOG_INFO: return LOG_INFO;
   case MESA_LOG_DEBUG: return LOG_DEBUG;
   case MESA_NUM_LOG_LEVELS:
      break;
   }

   UNREACHABLE("bad mesa_log_level");
}

static void
logger_syslog(enum mesa_log_level level,
              const char *tag,
              const char *format,
              va_list va)
{
   char local_msg[1024];
   char *msg = logger_vasnprintf(local_msg, sizeof(local_msg),
         LOGGER_VASNPRINTF_AFFIX_TAG, level, tag, format, va);

   syslog(level_to_syslog(level), "%s", msg);

   if (msg != local_msg)
      free(msg);
}

#endif /* DETECT_OS_POSIX */

#if DETECT_OS_ANDROID

static inline android_LogPriority
level_to_android(enum mesa_log_level l)
{
   switch (l) {
   case MESA_LOG_ERROR: return ANDROID_LOG_ERROR;
   case MESA_LOG_WARN: return ANDROID_LOG_WARN;
   case MESA_LOG_INFO: return ANDROID_LOG_INFO;
   case MESA_LOG_DEBUG: return ANDROID_LOG_DEBUG;
   case MESA_NUM_LOG_LEVELS:
      break;
   }

   UNREACHABLE("bad mesa_log_level");
}

static void
logger_android(enum mesa_log_level level,
               const char *tag,
               const char *format,
               va_list va)
{
   /* Android can truncate/drop messages
    *
    *  - the internal buffer for vsnprintf has a fixed size (usually 1024)
    *  - the socket to logd is non-blocking
    *
    * and provides no way to detect.  Try our best.
    */
   char local_msg[1024];
   char *msg = logger_vasnprintf(local_msg, sizeof(local_msg), 0, level, tag,
         format, va);

   __android_log_write(level_to_android(level), tag, msg);

   if (msg != local_msg)
      free(msg);

   /* increase the chance of logd doing its part */
   if (mesa_log_control & MESA_LOG_CONTROL_WAIT)
      thrd_yield();
}

#endif /* DETECT_OS_ANDROID */

#if DETECT_OS_WINDOWS

static void
logger_windbg(enum mesa_log_level level,
              const char *tag,
              const char *format,
              va_list va)
{
   char local_msg[1024];
   char *msg = logger_vasnprintf(local_msg, sizeof(local_msg),
         LOGGER_VASNPRINTF_AFFIX_TAG |
         LOGGER_VASNPRINTF_AFFIX_LEVEL |
         LOGGER_VASNPRINTF_AFFIX_NEWLINE,
         level, tag, format, va);

   OutputDebugStringA(msg);

   if (msg != local_msg)
      free(msg);
}

static void
logger_juice(enum mesa_log_level level,
             const char *tag,
             const char *format,
             va_list va)
{
   char local_msg[1024];
   /* Only the non-leveled sinks need the level spelled out in the text:
    * Juice_LogOutput records it itself. */
   char *msg = logger_vasnprintf(local_msg, sizeof(local_msg),
         LOGGER_VASNPRINTF_AFFIX_TAG |
         (juice_leveled_sink_active() ? 0 : LOGGER_VASNPRINTF_AFFIX_LEVEL) |
         LOGGER_VASNPRINTF_AFFIX_NEWLINE,
         level, tag, format, va);

   mesa_log_emit(level, msg);

   if (msg != local_msg)
      free(msg);
}

#endif /* DETECT_OS_WINDOWS */

/* This is for use with debug functions that take a FILE, such as
 * nir_print_shader, although switching to nir_log_shader* is preferred.
 */
FILE *
mesa_log_get_file(void)
{
   mesa_log_init();
   return mesa_log_file;
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
   static const struct {
      enum mesa_log_control bit;
      void (*log)(enum mesa_log_level level,
                  const char *tag,
                  const char *format,
                  va_list va);
   } loggers[] = {
      { MESA_LOG_CONTROL_FILE, logger_file },
#if DETECT_OS_POSIX
      { MESA_LOG_CONTROL_SYSLOG, logger_syslog },
#endif
#if DETECT_OS_ANDROID
      { MESA_LOG_CONTROL_ANDROID, logger_android },
#endif
#if DETECT_OS_WINDOWS
      { MESA_LOG_CONTROL_WINDBG, logger_windbg },
      { MESA_LOG_CONTROL_JUICE, logger_juice },
#endif
   };

   mesa_log_init();

   /* Filtered before formatting: zink calls mesa_logi() per draw, and the
    * formatting below is not worth paying to then discard. */
   if (!mesa_log_level_enabled(level))
      return;

   if (unlikely(mesa_max_log_level >= MESA_NUM_LOG_LEVELS)) {
      /* Set to the default since this function will call back into mesa_log()
       * and we don't want to recurse back into the once.
       */
      mesa_max_log_level = MESA_DEFAULT_LOG_LEVEL;
      mesa_warn_invalid_level();
   }

   if (level > mesa_max_log_level)
      return;

   for (uint32_t i = 0; i < ARRAY_SIZE(loggers); i++) {
      if (mesa_log_control & loggers[i].bit) {
         va_list copy;
         va_copy(copy, va);
         loggers[i].log(level, tag, format, copy);
         va_end(copy);
      }
   }
}

void
_mesa_log(const char *fmtString, ...)
{
   char s[MAX_LOG_MESSAGE_LENGTH];
   va_list args;
   va_start(args, fmtString);
   vsnprintf(s, MAX_LOG_MESSAGE_LENGTH, fmtString, args);
   va_end(args);
   mesa_log_if_debug(MESA_LOG_INFO, s);
}

void
_mesa_log_direct(const char *string)
{
   mesa_log_if_debug(MESA_LOG_INFO, string);
}

struct log_stream *
_mesa_log_stream_create(enum mesa_log_level level, const char *tag)
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
