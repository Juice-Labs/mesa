/**
 * Texture VRAM reclamation for 3DEXPERIENCE.
 *
 * Two rules, both LRU eviction regardless of format:
 *
 * 1) 75% cap: if total texture VRAM > 75% of GPU memory, evict until
 *    we're back under 75%.
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
#include "util/log.h"
#include "util/u_inlines.h"
#include "util/format/u_format.h"

#include <stdlib.h>
#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
#include <wctype.h>
#endif

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

   WCHAR path[MAX_PATH] = { 0 };
   GetModuleFileNameW(NULL, path, MAX_PATH);
   for (WCHAR *p = path; *p; p++)
      *p = towlower(*p);
   cached = (wcsstr(path, L"3dexperience") != NULL);
   fprintf(stderr, "[TEX GC] is_3dexperience: path='%ls' result=%d\n",
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
   fprintf(stderr, "[TEX GC] GPU VRAM: %d MiB, 75%% limit: %.0f MiB\n",
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

   /* Rule 1: total > 75% of GPU VRAM */
   bool over_vram = sd.total_vram > vram_limit;

   /* Rule 2: big R32 incoming and R32 total > 1 GiB */
   bool over_r32 = incoming_is_big_r32 && sd.total_r32 > R32G32B32A32_BUDGET;

   if (!over_vram && !over_r32) {
      free(sd.list);
      return;
   }

   qsort(sd.list, sd.count, sizeof(sd.list[0]), cmp_by_stamp_asc);

   uint64_t cur_vram = sd.total_vram;
   uint64_t cur_r32 = sd.total_r32;
   uint64_t freed = 0;
   unsigned evicted = 0;

   for (unsigned i = 0; i < sd.count; i++) {
      /* Stop when both thresholds are satisfied */
      bool vram_ok = cur_vram <= vram_limit;
      bool r32_ok = !over_r32 || cur_r32 <= R32G32B32A32_BUDGET;

      if (over_vram && !over_r32) {
         /* Rule 1 only: stop when VRAM is OK */
         if (vram_ok) break;
      } else if (!over_vram && over_r32) {
         /* Rule 2 only: stop when VRAM is OK OR R32 is OK */
         if (vram_ok || r32_ok) break;
      } else {
         /* Both rules: stop when VRAM is OK AND R32 is OK */
         if (vram_ok && r32_ok) break;
      }

      evict_texture(st, sd.list[i].tex);
      cur_vram -= sd.list[i].size;
      if (sd.list[i].is_r32)
         cur_r32 -= sd.list[i].size;
      freed += sd.list[i].size;
      evicted++;
   }

   if (evicted > 0) {
      fprintf(stderr, "[TEX GC] evicted %u textures (freed %.0f MiB). "
              "VRAM: %.0f->%.0f MiB (limit %.0f), "
              "R32: %.0f->%.0f MiB (limit %.0f)\n",
              evicted, freed / (1024.0 * 1024.0),
              sd.total_vram / (1024.0 * 1024.0),
              cur_vram / (1024.0 * 1024.0),
              vram_limit / (1024.0 * 1024.0),
              sd.total_r32 / (1024.0 * 1024.0),
              cur_r32 / (1024.0 * 1024.0),
              R32G32B32A32_BUDGET / (1024.0 * 1024.0));

      mesa_logi("JUICE TEX GC: evicted %u (freed %.0f MiB), "
                "VRAM %.0f->%.0f/%.0f MiB, R32 %.0f->%.0f/%.0f MiB",
                evicted, freed / (1024.0 * 1024.0),
                sd.total_vram / (1024.0 * 1024.0),
                cur_vram / (1024.0 * 1024.0),
                vram_limit / (1024.0 * 1024.0),
                sd.total_r32 / (1024.0 * 1024.0),
                cur_r32 / (1024.0 * 1024.0),
                R32G32B32A32_BUDGET / (1024.0 * 1024.0));
   }

   free(sd.list);
}
