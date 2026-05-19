/*
 * JUICE diagnostic logger.
 *
 * Thread-safe append-only logger that writes a single line per call to
 *   c:\\temp\\juice_diag.log         (Windows)
 *   /tmp/juice_diag.log              (Linux)
 *
 * This is intentionally simple and always-on so we can correlate GL-side
 * uniform writes, Zink bindless descriptor state, and draw boundaries
 * without environment variables. It is used to debug GL_ARB_bindless_texture
 * behaviour in Mesa/Zink under the Juice ICD.
 */
#ifndef JUICE_DIAG_LOG_H
#define JUICE_DIAG_LOG_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Append a single line. `tag` is a short ASCII identifier such as
 * "H_CREATE", "DS_WRITE", "DRAW_PRE". `fmt`/varargs follow printf rules.
 * Newline is appended automatically; callers must not include one. */
void juice_diag_logf(const char *tag, const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
   __attribute__((format(printf, 2, 3)))
#endif
   ;

/* Append a single line with a hex blob (truncated to 2048 bytes). */
void juice_diag_log_hex(const char *tag,
                        const char *prefix,
                        const void *data,
                        size_t len);

/* Write `data` of `len` bytes verbatim to c:\temp\replay\<relpath>.
 *
 * Used to dump replay artifacts (SPIR-V binaries, UBO snapshots, encoded
 * sampler/view/image CreateInfo blobs). Returns 0 on success, -1 on error.
 * Creates the c:\temp\replay\ directory on first use. Truncates existing
 * files. Logs a JUICE_REPLAY_WRITE line whether the write succeeds or
 * fails so the diag log itself is sufficient to know what's on disk. */
int juice_diag_replay_write(const char *relpath,
                            const void *data,
                            size_t len);

#ifdef __cplusplus
}
#endif

#endif /* JUICE_DIAG_LOG_H */
