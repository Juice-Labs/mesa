/**
 * Texture VRAM reclamation for 3DEXPERIENCE.
 *
 * Two rules, both LRU eviction regardless of format:
 *
 * 1) 75% cap: if actual GPU device memory usage > 75% (queried via
 *    VK_EXT_memory_budget), evict until we're back under 75%.
 *    This sees ALL device allocations (CUDA, Vulkan, GL, etc.).
 *
 * 2) R32G32B32A32_FLOAT reaping: when a large (>8 MiB) RGBA32F alloc
 *    comes in and total RGBA32F texture VRAM > 1 GiB, evict until
 *    we're below 75% total OR below 1 GiB of RGBA32F, whichever
 *    threshold we cross first.
 *
 * Evicted textures keep their GL name — only the backing pipe_resource
 * is released.  st_finalize_texture recreates it lazily if needed.
 */

#include "main/context.h"
#include "main/hash.h"
#include "main/macros.h"
#include "main/mtypes.h"
#include "main/teximage.h"
#include "main/texobj.h"
#include "st_context.h"
#include "st_format.h"
#include "st_sampler_view.h"
#include "st_texture_gc.h"
#include "pipe/p_defines.h"
#include "pipe/p_screen.h"
#include "pipe/p_state.h"
#include "util/log.h"
#include "util/u_inlines.h"
#include "util/format/u_format.h"

#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#include <wctype.h>
#endif

static uint64_t
query_gpu_used_bytes(struct st_context *st)
{
   struct pipe_screen *screen = st->screen;
   if (!screen->query_memory_info)
      return 0;

   struct pipe_memory_info mi;
   screen->query_memory_info(screen, &mi);

   uint64_t used_kb = (uint64_t)mi.total_device_memory -
                      (uint64_t)mi.avail_device_memory;
   return used_kb * 1024;
}

static void
log_gpu_memory(struct st_context *st, const char *tag)
{
   struct pipe_screen *screen = st->screen;
   if (!screen->query_memory_info)
      return;

   struct pipe_memory_info mi;
   screen->query_memory_info(screen, &mi);

   static bool first = true;
   if (first) {
      first = false;
      uint64_t dev_total_mb  = (uint64_t)mi.total_device_memory / 1024;
      uint64_t dev_avail_mb  = (uint64_t)mi.avail_device_memory / 1024;
      uint64_t stg_total_mb  = (uint64_t)mi.total_staging_memory / 1024;
      uint64_t stg_avail_mb  = (uint64_t)mi.avail_staging_memory / 1024;
      mesa_logi("JUICE TEX GC: Device(VRAM) total=%llu avail=%llu used=%llu MiB; "
                "Staging(GART) total=%llu avail=%llu used=%llu MiB",
                (unsigned long long)dev_total_mb,
                (unsigned long long)dev_avail_mb,
                (unsigned long long)(dev_total_mb - dev_avail_mb),
                (unsigned long long)stg_total_mb,
                (unsigned long long)stg_avail_mb,
                (unsigned long long)(stg_total_mb - stg_avail_mb));
   }

   uint64_t total_mb = (uint64_t)mi.total_device_memory / 1024;
   uint64_t avail_mb = (uint64_t)mi.avail_device_memory / 1024;
   uint64_t used_mb  = total_mb - avail_mb;

   mesa_logi("JUICE TEX GC: %s GPU VRAM: %llu / %llu MiB used (%.0f%%)",
             tag, (unsigned long long)used_mb, (unsigned long long)total_mb,
             total_mb ? (used_mb * 100.0 / total_mb) : 0.0);
}

#define R32G32B32A32_BUDGET  (1ULL * 1024 * 1024 * 1024)
#define BIG_R32_THRESHOLD    (8ULL * 1024 * 1024)
#define BIG_TEX_BYTES        (1ULL * 1024 * 1024)

static bool
is_3dexperience(void)
{
#ifdef _WIN32
   static int cached = -1;
   if (cached >= 0)
      return cached;

   const char *env = getenv("JUICE_TEXTURE_GC");
   if (!env || env[0] != '1') {
      cached = 0;
      mesa_logi("JUICE TEX GC: disabled (set JUICE_TEXTURE_GC=1 to enable)");
      return false;
   }

   WCHAR path[MAX_PATH] = { 0 };
   GetModuleFileNameW(NULL, path, MAX_PATH);
   for (WCHAR *p = path; *p; p++)
      *p = towlower(*p);
   cached = (wcsstr(path, L"3dexperience") != NULL);
   mesa_logi("JUICE TEX GC: JUICE_TEXTURE_GC=1, is_3dexperience: path='%ls' result=%d",
             path, cached);
   return cached;
#else
   return false;
#endif
}

static uint64_t
get_vram_limit(struct st_context *st)
{
   static uint64_t cached = 0;
   if (cached)
      return cached;

   int vram_mb = st->screen->get_param(st->screen, PIPE_CAP_VIDEO_MEMORY);
   if (vram_mb <= 0)
      vram_mb = 4096;
   cached = (uint64_t)vram_mb * 1024 * 1024 * 3 / 4;  /* 75% */
   mesa_logi("JUICE TEX GC: GPU VRAM: %d MiB, 75%% limit: %.0f MiB",
             vram_mb, cached / (1024.0 * 1024.0));
   return cached;
}

static uint64_t
resource_bytes(const struct pipe_resource *pt)
{
   if (!pt)
      return 0;
   uint64_t bytes = 0;
   unsigned bpp = util_format_get_blocksize(pt->format);
   for (unsigned lv = 0; lv <= pt->last_level; lv++) {
      unsigned w = u_minify(pt->width0, lv);
      unsigned h = u_minify(pt->height0, lv);
      unsigned d = u_minify(pt->depth0, lv);
      bytes += (uint64_t)w * h * d * bpp * MAX2(pt->array_size, 1);
   }
   return bytes;
}

static bool
is_r32g32b32a32_float(const struct pipe_resource *pt)
{
   return pt && pt->format == PIPE_FORMAT_R32G32B32A32_FLOAT;
}

struct evict_entry {
   struct gl_texture_object *tex;
   uint64_t size;
   uint64_t stamp;
   bool     is_r32;
};

struct scan_data {
   struct gl_texture_object *keep;
   struct evict_entry *list;
   unsigned  count;
   unsigned  cap;
   uint64_t  total_vram;
   uint64_t  total_r32;
};

static void
scan_callback(void *data, void *user)
{
   struct gl_texture_object *tex = (struct gl_texture_object *)data;
   struct scan_data *sd = (struct scan_data *)user;

   if (!tex->pt || tex->Name == 0)
      return;

   uint64_t size = resource_bytes(tex->pt);
   bool r32 = is_r32g32b32a32_float(tex->pt);

   sd->total_vram += size;
   if (r32)
      sd->total_r32 += size;

   if (tex == sd->keep)
      return;
   if (tex->Immutable || tex->surface_based)
      return;
   if (size < BIG_TEX_BYTES)
      return;

   if (sd->count >= sd->cap) {
      unsigned new_cap = sd->cap ? sd->cap * 2 : 64;
      struct evict_entry *new_list =
         realloc(sd->list, new_cap * sizeof(*new_list));
      if (!new_list)
         return;
      sd->list = new_list;
      sd->cap = new_cap;
   }
   sd->list[sd->count].tex = tex;
   sd->list[sd->count].size = size;
   sd->list[sd->count].stamp = tex->last_used_stamp;
   sd->list[sd->count].is_r32 = r32;
   sd->count++;
}

static int
cmp_by_stamp_asc(const void *a, const void *b)
{
   const struct evict_entry *ea = a;
   const struct evict_entry *eb = b;
   return (ea->stamp > eb->stamp) - (ea->stamp < eb->stamp);
}

static void
evict_texture(struct st_context *st, struct gl_texture_object *tex)
{
   st_texture_release_all_sampler_views(st, tex);

   const GLuint nr_faces = _mesa_num_tex_faces(tex->Target);
   for (GLuint face = 0; face < nr_faces; face++) {
      for (GLuint lv = 0; lv < MAX_TEXTURE_LEVELS; lv++) {
         if (tex->Image[face][lv] && tex->Image[face][lv]->pt)
            pipe_resource_reference(&tex->Image[face][lv]->pt, NULL);
      }
   }

   pipe_resource_reference(&tex->pt, NULL);
   tex->needs_validation = true;
}

void
st_texture_gc_free_if_over_limit(struct st_context *st,
                                 struct gl_texture_object *keep,
                                 const struct gl_texture_image *incoming)
{
   if (!is_3dexperience())
      return;

   struct gl_context *ctx = st->ctx;
   if (!ctx->Shared || !ctx->Shared->TexObjects)
      return;

   uint64_t vram_limit = get_vram_limit(st);

   /* Determine if the incoming alloc is a big R32G32B32A32_FLOAT */
   bool incoming_is_big_r32 = false;
   if (incoming) {
      enum pipe_format pfmt =
         st_mesa_format_to_pipe_format(st, incoming->TexFormat);
      uint64_t incoming_size = (uint64_t)incoming->Width *
                               incoming->Height *
                               MAX2(incoming->Depth, 1) *
                               util_format_get_blocksize(pfmt);
      incoming_is_big_r32 = (pfmt == PIPE_FORMAT_R32G32B32A32_FLOAT &&
                             incoming_size > BIG_R32_THRESHOLD);
   }

   struct scan_data sd = {
      .keep       = keep,
      .list       = NULL,
      .count      = 0,
      .cap        = 0,
      .total_vram = 0,
      .total_r32  = 0,
   };

   _mesa_HashWalk(ctx->Shared->TexObjects, scan_callback, &sd);

   /* Rule 1: actual GPU device memory > 75% (includes CUDA, VK, etc.) */
   uint64_t gpu_used = query_gpu_used_bytes(st);
   bool over_vram = gpu_used > vram_limit;

   /* Rule 2: big R32 incoming and R32 total > 1 GiB */
   bool over_r32 = incoming_is_big_r32 && sd.total_r32 > R32G32B32A32_BUDGET;

   if (!over_vram && !over_r32) {
      free(sd.list);
      return;
   }

   log_gpu_memory(st, "before-evict");
   mesa_logi("JUICE TEX GC: triggers: over_vram=%d (gpu_used=%.0f MiB, limit=%.0f MiB) "
             "over_r32=%d (r32_total=%.0f MiB, limit=%.0f MiB)",
             over_vram, gpu_used / (1024.0 * 1024.0), vram_limit / (1024.0 * 1024.0),
             over_r32, sd.total_r32 / (1024.0 * 1024.0),
             R32G32B32A32_BUDGET / (1024.0 * 1024.0));

   qsort(sd.list, sd.count, sizeof(sd.list[0]), cmp_by_stamp_asc);

   uint64_t cur_gpu = gpu_used;
   uint64_t cur_r32 = sd.total_r32;
   uint64_t freed = 0;
   unsigned evicted = 0;

   for (unsigned i = 0; i < sd.count; i++) {
      bool vram_ok = cur_gpu <= vram_limit;
      bool r32_ok = !over_r32 || cur_r32 <= R32G32B32A32_BUDGET;

      if (over_vram && !over_r32) {
         /* Rule 1 only: stop when GPU usage is OK */
         if (vram_ok) break;
      } else if (!over_vram && over_r32) {
         /* Rule 2 only: stop when R32 total is under budget */
         if (r32_ok) break;
      } else {
         /* Both rules: stop when both are satisfied */
         if (vram_ok && r32_ok) break;
      }

      evict_texture(st, sd.list[i].tex);
      cur_gpu -= sd.list[i].size;
      if (sd.list[i].is_r32)
         cur_r32 -= sd.list[i].size;
      freed += sd.list[i].size;
      evicted++;
   }

   if (evicted > 0) {
      mesa_logi("JUICE TEX GC: evicted %u (freed %.0f MiB), "
                "GPU %.0f->%.0f/%.0f MiB, R32 %.0f->%.0f/%.0f MiB",
                evicted, freed / (1024.0 * 1024.0),
                gpu_used / (1024.0 * 1024.0),
                cur_gpu / (1024.0 * 1024.0),
                vram_limit / (1024.0 * 1024.0),
                sd.total_r32 / (1024.0 * 1024.0),
                cur_r32 / (1024.0 * 1024.0),
                R32G32B32A32_BUDGET / (1024.0 * 1024.0));

      log_gpu_memory(st, "after-evict");
   }

   free(sd.list);
}
