/*
 * Minimal replacements for the few Mesa util symbols the r300 shader
 * compiler references but which would otherwise drag in the whole of
 * src/util (u_debug.c, log.c, os support ...).
 *
 * Part of rc_compile (tools/mesa-rc), not Mesa.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/u_debug.h"
#include "util/log.h"

/* print_stats() in radeon_compiler.c reports instruction counts through
 * the pipe_debug_callback; we have no gallium context, so just print the
 * message.  rc_compile redirects stderr into stdout, so this lands in the
 * "=== COMPILE ===" section. */
void
_util_debug_message(struct util_debug_callback *cb, unsigned *id,
                    enum util_debug_type type, const char *fmt, ...)
{
   va_list ap;
   (void)cb; (void)id;
   fprintf(stderr, "%s", type == UTIL_DEBUG_TYPE_SHADER_INFO ? "stats: " : "debug: ");
   va_start(ap, fmt);
   vfprintf(stderr, fmt, ap);
   va_end(ap);
   fputc('\n', stderr);
}

/* radeon_pair_schedule.c: debug_get_num_option("RADEON_TEX_GROUP", 8) */
long
debug_get_num_option(const char *name, long dfault)
{
   const char *s = getenv(name);
   char *end;
   long v;
   if (!s || !*s)
      return dfault;
   v = strtol(s, &end, 0);
   return (*end == '\0') ? v : dfault;
}

/* radeon_program_alu.c: mesa_logw_once(...) */
void
mesa_log(enum mesa_log_level level, const char *tag, const char *format, ...)
{
   va_list ap;
   fprintf(stderr, "%s: %s: ", tag ? tag : "mesa",
           level == MESA_LOG_ERROR ? "error" :
           level == MESA_LOG_WARN ? "warning" :
           level == MESA_LOG_INFO ? "info" : "debug");
   va_start(ap, format);
   vfprintf(stderr, format, ap);
   va_end(ap);
   fputc('\n', stderr);
}
