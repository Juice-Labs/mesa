/**
 * Shader dumping functionality for debugging
 * Always enabled - outputs preprocessed shaders and pipeline JSON
 */

#include "shader_dump.h"
#include "shaderapi.h"
#include "context.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

#ifdef _WIN32
#include <direct.h>
#define mkdir(path, mode) _mkdir(path)
#endif

static bool dump_initialized = false;
static char dump_dir[] = "c:\\shader";

/* Simple hash function - basic but sufficient for our needs */
uint32_t 
_mesa_simple_hash(const char *str)
{
   uint32_t hash = 5381;
   int c;
   
   if (!str) return 0;
   
   while ((c = *str++)) {
      hash = ((hash << 5) + hash) + c; /* hash * 33 + c */
   }
   
   return hash & 0x7FFFFFFF; /* Keep it positive */
}

void
_mesa_init_shader_dump(void)
{
   if (dump_initialized)
      return;
      
   /* Create c:\shader directory if it doesn't exist */
   if (mkdir(dump_dir, 0755) == -1 && errno != EEXIST) {
      /* Failed to create directory, but continue anyway */
   }
   
   dump_initialized = true;
}

void
_mesa_dump_preprocessed_shader(struct gl_context *ctx, 
                              struct gl_shader *shader,
                              const char *preprocessed_source)
{
   FILE *file;
   char filename[512];
   uint32_t hash;
   const char *stage_name;
   char *complete_content;
   size_t content_len;
   
   if (!preprocessed_source || !shader)
      return;
      
   _mesa_init_shader_dump();
   
   /* Get stage name */
   stage_name = _mesa_shader_stage_to_string(shader->Stage);
   
   /* Use Mesa's built-in source hash for consistency */
   hash = *(uint32_t*)shader->source_sha1; /* Use first 4 bytes of SHA1 */
   
   /* Build complete file content */
   size_t header_size = 512; /* Conservative estimate for header */
   size_t source_len = strlen(preprocessed_source);
   content_len = header_size + source_len + 100; /* Extra padding */
   
   complete_content = malloc(content_len);
   if (!complete_content) {
      return; /* Silent failure */
   }
   
   /* Build the complete content with hash in header */
   snprintf(complete_content, content_len,
            "// Mesa Shader Dump\n"
            "// Stage: %s\n"
            "// Shader ID: %u\n"
            "// Preprocessed: YES\n"
            "// Hash: 0x%08x (Mesa source SHA1)\n"
            "// =====================================\n\n"
            "%s",
            stage_name, shader->Name, hash, preprocessed_source);
   
   /* Create filename: shader_[hash]_[ordinal].glsl */
   snprintf(filename, sizeof(filename), "%s\\shader_%08x_%u_%s.glsl", 
            dump_dir, hash, shader->Name, stage_name);
   
   file = fopen(filename, "w");
   if (!file) {
      free(complete_content);
      return; /* Silent failure - don't interfere with normal operation */
   }
   
   /* Write the complete content */
   fprintf(file, "%s", complete_content);
   
   fclose(file);
   free(complete_content);
}

void
_mesa_dump_pipeline_json(struct gl_context *ctx,
                        struct gl_shader_program *shProg)
{
   FILE *file;
   char filename[512];
   bool first_shader = true;
   
   if (!shProg || !shProg->data->LinkStatus)
      return;
      
   _mesa_init_shader_dump();
   
   /* Create JSON filename: pipeline_[program_id].json */
   snprintf(filename, sizeof(filename), "%s\\pipeline_%u.json", 
            dump_dir, shProg->Name);
   
   file = fopen(filename, "w");
   if (!file) {
      return; /* Silent failure */
   }
   
   /* Write JSON pipeline description */
   fprintf(file, "{\n");
   fprintf(file, "  \"program_id\": %u,\n", shProg->Name);
   fprintf(file, "  \"link_status\": %s,\n", 
           shProg->data->LinkStatus ? "true" : "false");
   fprintf(file, "  \"version\": %u,\n", shProg->data->Version);
   fprintf(file, "  \"is_es\": %s,\n", shProg->IsES ? "true" : "false");
   fprintf(file, "  \"separate_shader\": %s,\n", 
           shProg->SeparateShader ? "true" : "false");
   fprintf(file, "  \"num_shaders\": %u,\n", shProg->NumShaders);
   fprintf(file, "  \"shaders\": [\n");
   
   /* List all shaders in the program */
   for (unsigned i = 0; i < shProg->NumShaders; i++) {
      struct gl_shader *shader = shProg->Shaders[i];
      uint32_t hash = 0;
      
      if (!first_shader) {
         fprintf(file, ",\n");
      }
      first_shader = false;
      
      /* Use Mesa's built-in source hash for consistency with dumped files */
      hash = *(uint32_t*)shader->source_sha1; /* Use first 4 bytes of SHA1 */
      
      fprintf(file, "    {\n");
      fprintf(file, "      \"shader_id\": %u,\n", shader->Name);
      fprintf(file, "      \"stage\": \"%s\",\n", 
              _mesa_shader_stage_to_string(shader->Stage));
      fprintf(file, "      \"hash\": \"0x%08x\",\n", hash);
      fprintf(file, "      \"compile_status\": %s,\n", 
              shader->CompileStatus ? "true" : "false");
      fprintf(file, "      \"version\": %u,\n", shader->Version);
      fprintf(file, "      \"is_es\": %s,\n", shader->IsES ? "true" : "false");
      
      /* Reference to the dumped shader file */
      fprintf(file, "      \"dumped_file\": \"shader_%08x_%u_%s.glsl\"\n", 
              hash, shader->Name, _mesa_shader_stage_to_string(shader->Stage));
      
      fprintf(file, "    }");
   }
   
   fprintf(file, "\n  ],\n");
   
   /* List linked shader stages */
   fprintf(file, "  \"linked_stages\": [\n");
   first_shader = true;
   for (unsigned stage = 0; stage < MESA_SHADER_STAGES; stage++) {
      if (shProg->_LinkedShaders[stage]) {
         if (!first_shader) {
            fprintf(file, ",\n");
         }
         first_shader = false;
         
         fprintf(file, "    {\n");
         fprintf(file, "      \"stage\": \"%s\",\n", 
                 _mesa_shader_stage_to_string(stage));
         fprintf(file, "      \"program_id\": %u\n", 
                 shProg->_LinkedShaders[stage]->Program ? 
                 shProg->_LinkedShaders[stage]->Program->Id : 0);
         fprintf(file, "    }");
      }
   }
   fprintf(file, "\n  ]\n");
   
   fprintf(file, "}\n");
   
   fclose(file);
}