#ifndef ST_TEXTURE_GC_H
#define ST_TEXTURE_GC_H

struct st_context;
struct gl_texture_object;

void st_texture_gc_free_if_over_limit(struct st_context *st,
                                      struct gl_texture_object *keep);

#endif
