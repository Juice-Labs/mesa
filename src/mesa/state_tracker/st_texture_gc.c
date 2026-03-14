/**
 * Texture VRAM reclamation for 3DEXPERIENCE.
 *
 * CATIA's Stellar renderer creates render-target textures each frame via
 * glTexImage2D and never calls glDeleteTextures, leaking ~2 GiB/min.
 *
 * On each glTexImage2D we tally all "offending" textures — big (>=1 MiB),
 * mutable, non-surface textures.  If their total exceeds the offender
 * budget (2 GiB), we release the backing pipe_resource on the oldest ones
 * until we're back under budget.  The GL texture object stays alive so
 * st_finalize_texture can lazily recreate backing if the app touches it.
 */

#include "main/context.h"
#include "main/hash.h"
#include "main/macros.h"
#include "main/mtypes.h"
#include "main/teximage.h"
#include "main/texobj.h"
#include "st_context.h"
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

#define OFFENDER_BUDGET_BYTES  (2ULL * 1024 * 1024 * 1024)
#define BIG_TEX_BYTES          (1ULL * 1024 * 1024)

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
is_offending_texture(const struct gl_texture_object *tex, uint64_t size)
{
   if (tex->Immutable || tex->surface_based)
      return false;
   if (size < BIG_TEX_BYTES)
      return false;
   return true;
}

struct offender_entry {
   struct gl_texture_object *tex;
   uint64_t size;
   GLuint   name;
};

struct scan_data {
   struct gl_texture_object *keep;
   struct offender_entry *list;
   unsigned  count;
   unsigned  cap;
   uint64_t  total_offender_bytes;
};

static void
scan_callback(void *data, void *user)
{
   struct gl_texture_object *tex = (struct gl_texture_object *)data;
   struct scan_data *sd = (struct scan_data *)user;

   if (!tex->pt || tex->Name == 0)
      return;
   if (tex == sd->keep)
      return;

   uint64_t size = resource_bytes(tex->pt);
   if (!is_offending_texture(tex, size))
      return;

   sd->total_offender_bytes += size;

   if (sd->count >= sd->cap) {
      unsigned new_cap = sd->cap ? sd->cap * 2 : 64;
      struct offender_entry *new_list =
         realloc(sd->list, new_cap * sizeof(*new_list));
      if (!new_list)
         return;
      sd->list = new_list;
      sd->cap = new_cap;
   }
   sd->list[sd->count].tex = tex;
   sd->list[sd->count].size = size;
   sd->list[sd->count].name = tex->Name;
   sd->count++;
}

static int
cmp_by_name_asc(const void *a, const void *b)
{
   const struct offender_entry *ea = a;
   const struct offender_entry *eb = b;
   return (ea->name > eb->name) - (ea->name < eb->name);
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
                                 struct gl_texture_object *keep)
{
   if (!is_3dexperience())
      return;

   struct gl_context *ctx = st->ctx;
   if (!ctx->Shared || !ctx->Shared->TexObjects)
      return;

   struct scan_data sd = {
      .keep                = keep,
      .list                = NULL,
      .count               = 0,
      .cap                 = 0,
      .total_offender_bytes = 0,
   };

   _mesa_HashWalk(ctx->Shared->TexObjects, scan_callback, &sd);

   if (sd.total_offender_bytes <= OFFENDER_BUDGET_BYTES || sd.count == 0) {
      free(sd.list);
      return;
   }

   /* Sort by name ascending — lowest names are oldest allocations. */
   qsort(sd.list, sd.count, sizeof(sd.list[0]), cmp_by_name_asc);

   uint64_t to_free = sd.total_offender_bytes - OFFENDER_BUDGET_BYTES;
   uint64_t freed = 0;
   unsigned evicted = 0;

   for (unsigned i = 0; i < sd.count && freed < to_free; i++) {
      evict_texture(st, sd.list[i].tex);
      freed += sd.list[i].size;
      evicted++;
   }

   fprintf(stderr, "[TEX GC] offenders %.0f MiB > budget %.0f MiB, "
           "evicted %u (freed %.0f MiB, %u remain)\n",
           sd.total_offender_bytes / (1024.0 * 1024.0),
           (double)OFFENDER_BUDGET_BYTES / (1024.0 * 1024.0),
           evicted, freed / (1024.0 * 1024.0),
           sd.count - evicted);

   mesa_logi("JUICE TEX GC: offenders %.0f MiB > budget %.0f MiB, "
             "evicted %u (freed %.0f MiB)",
             sd.total_offender_bytes / (1024.0 * 1024.0),
             (double)OFFENDER_BUDGET_BYTES / (1024.0 * 1024.0),
             evicted, freed / (1024.0 * 1024.0));

   free(sd.list);
}
