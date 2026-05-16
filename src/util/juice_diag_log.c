/*
 * JUICE diagnostic logger. See juice_diag_log.h.
 *
 * One process-global mutex protects a single fopen("a") FILE handle so that
 * lines from different threads (including the GL frontend thread and the
 * Zink/Mesa-driver thread under threaded-context) never interleave.
 */

#include "juice_diag_log.h"

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "c11/threads.h"

#if defined(_WIN32)
#  include <direct.h>
#  include <process.h>
#  define JUICE_DIAG_LOG_PATH "c:\\temp\\juice_diag.log"
#  define JUICE_REPLAY_DIR    "c:\\temp\\replay"
#  define JUICE_PATH_SEP      "\\"
#  define JUICE_GETPID()      ((int)_getpid())
#  define JUICE_MKDIR(p)      _mkdir(p)
#else
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <unistd.h>
#  define JUICE_DIAG_LOG_PATH "/tmp/juice_diag.log"
#  define JUICE_REPLAY_DIR    "/tmp/replay"
#  define JUICE_PATH_SEP      "/"
#  define JUICE_GETPID()      ((int)getpid())
#  define JUICE_MKDIR(p)      mkdir((p), 0755)
#endif

static once_flag g_once = ONCE_FLAG_INIT;
static mtx_t     g_mtx;
static FILE     *g_fp = NULL;

static void
init_once(void)
{
   mtx_init(&g_mtx, mtx_plain);
   g_fp = fopen(JUICE_DIAG_LOG_PATH, "a");
   if (g_fp) {
      /* Line-buffered so even a crash flushes most data. */
      setvbuf(g_fp, NULL, _IOLBF, 4096);
      /* Build marker: lets the reader confirm WHICH instrumentation level
       * the running DLL has. Bump the version string whenever we add new
       * tags so a stale log is obvious. */
      fprintf(g_fp,
              "----- juice_diag_log init "
              "build=" __DATE__ " " __TIME__
              " version=9-props-replay -----\n");
      fflush(g_fp);
   }
}

static once_flag g_replay_dir_once = ONCE_FLAG_INIT;

static void
init_replay_dir(void)
{
   /* Best-effort directory creation; we don't track success here because
    * fopen() in juice_diag_replay_write reports the real outcome anyway. */
   (void)JUICE_MKDIR(JUICE_REPLAY_DIR);
}

static void
write_header(FILE *fp, const char *tag)
{
   struct timespec ts;
   memset(&ts, 0, sizeof(ts));
#if defined(_WIN32)
   timespec_get(&ts, TIME_UTC);
#else
   clock_gettime(CLOCK_REALTIME, &ts);
#endif
   fprintf(fp, "%lld.%09ld pid=%d %s ",
           (long long)ts.tv_sec, (long)ts.tv_nsec,
           JUICE_GETPID(), tag);
}

void
juice_diag_logf(const char *tag, const char *fmt, ...)
{
   call_once(&g_once, init_once);
   if (!g_fp)
      return;
   mtx_lock(&g_mtx);
   write_header(g_fp, tag);
   va_list ap;
   va_start(ap, fmt);
   vfprintf(g_fp, fmt, ap);
   va_end(ap);
   fputc('\n', g_fp);
   fflush(g_fp);
   mtx_unlock(&g_mtx);
}

/* Cap hex blob at 2048 bytes per line. Lines longer than ~64K can be a
 * problem for some text editors and our diag log is meant to remain
 * append-friendly. 2048 is enough to cover GLSL57's 688-byte default UBO. */
#define JUICE_DIAG_HEX_CAP 2048u

void
juice_diag_log_hex(const char *tag,
                   const char *prefix,
                   const void *data,
                   size_t len)
{
   call_once(&g_once, init_once);
   if (!g_fp)
      return;
   mtx_lock(&g_mtx);
   write_header(g_fp, tag);
   fprintf(g_fp, "%s len=%zu hex=", prefix, len);
   const unsigned char *p = (const unsigned char *)data;
   size_t n = len > JUICE_DIAG_HEX_CAP ? JUICE_DIAG_HEX_CAP : len;
   for (size_t i = 0; i < n; i++)
      fprintf(g_fp, "%02x", p[i]);
   if (n < len)
      fprintf(g_fp, "...");
   fputc('\n', g_fp);
   fflush(g_fp);
   mtx_unlock(&g_mtx);
}

int
juice_diag_replay_write(const char *relpath, const void *data, size_t len)
{
   call_once(&g_once, init_once);
   call_once(&g_replay_dir_once, init_replay_dir);

   char path[512];
   /* relpath is intentionally relative; we sanitize only the most obvious
    * traversal mistake (leading slash/backslash) so a buggy caller can't
    * write outside the replay dir. */
   const char *p = relpath ? relpath : "unnamed.bin";
   while (*p == '/' || *p == '\\')
      p++;
   snprintf(path, sizeof(path), "%s%s%s", JUICE_REPLAY_DIR, JUICE_PATH_SEP, p);

   FILE *f = fopen(path, "wb");
   if (!f) {
      juice_diag_logf("REPLAY_WRITE",
                      "FAILED path=%s len=%zu errno=%d", path, len, errno);
      return -1;
   }
   size_t w = (data && len) ? fwrite(data, 1, len, f) : 0;
   int closed = fclose(f);
   if (w != len || closed != 0) {
      juice_diag_logf("REPLAY_WRITE",
                      "PARTIAL path=%s len=%zu wrote=%zu close=%d",
                      path, len, w, closed);
      return -1;
   }
   juice_diag_logf("REPLAY_WRITE",
                   "OK path=%s len=%zu", path, len);
   return 0;
}
