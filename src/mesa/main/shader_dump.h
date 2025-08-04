/**
 * Shader dumping functionality for debugging
 * Always enabled - outputs preprocessed shaders and pipeline JSON
 */

#ifndef SHADER_DUMP_H
#define SHADER_DUMP_H

#include "glheader.h"
#include "mtypes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Simple hash function for shader content */
uint32_t _mesa_simple_hash(const char *str);

/* Dump preprocessed shader source to c:\shader directory */
void _mesa_dump_preprocessed_shader(struct gl_context *ctx, 
                                   struct gl_shader *shader,
                                   const char *preprocessed_source);

/* Dump pipeline JSON when program is linked */
void _mesa_dump_pipeline_json(struct gl_context *ctx,
                             struct gl_shader_program *shProg);

/* Initialize shader dump directory */
void _mesa_init_shader_dump(void);

#ifdef __cplusplus
}
#endif

#endif /* SHADER_DUMP_H */