#ifndef ST_TEXTURE_GC_H
#define ST_TEXTURE_GC_H

#include "main/mtypes.h"

struct st_context;
struct gl_texture_object;
struct gl_texture_image;

void st_texture_gc_free_if_over_limit(struct st_context *st,
                                      struct gl_texture_object *keep,
                                      const struct gl_texture_image *incoming);

static inline void
st_texture_gc_touch(struct gl_texture_object *tex)
{
   static uint64_t counter = 0;
   tex->last_used_stamp = ++counter;
}

#endif
