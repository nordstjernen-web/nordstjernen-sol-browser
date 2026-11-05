/* Nordstjernen — WebGL shader validation ahead of the GPU driver: the WebGL source rules, then glslang. */

#include "glsl_validate.h"

#if defined(NS_ENABLE_WEBGL)

#include <glslang/Include/glslang_c_interface.h>
#include <glslang/Public/resource_limits_c.h>
#include <stdarg.h>
#include <string.h>

#define NS_GLSL_MAX_IDENTIFIER 256
#define NS_GLSL_MAX_NESTING    256

typedef struct {
    const char *src;
    size_t      len;
    int         webgl_version;
    int         es_version;
    GString    *log;
} ns_glsl_scan;

static void ns_glsl_error(ns_glsl_scan *s, int line, const char *fmt, ...)
    G_GNUC_PRINTF(3, 4);

static void
ns_glsl_error(ns_glsl_scan *s, int line, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    g_string_append_printf(s->log, "ERROR: 0:%d: ", line);
    g_string_append_vprintf(s->log, fmt, ap);
    g_string_append_c(s->log, '\n');
    va_end(ap);
}

static gboolean
ns_glsl_char_allowed(unsigned char c, int es_version)
{
    if (g_ascii_isalnum(c) || c == '_') return TRUE;
    if (c == '\\') return es_version >= 300;
    return strchr(".+-/*%<>[](){}^|&~=!:;,?# \t\v\f\r\n", c) != NULL && c != '\0';
}

static gboolean
ns_glsl_identifier_start(unsigned char c)
{
    return g_ascii_isalpha(c) || c == '_';
}

static gboolean
ns_glsl_identifier_char(unsigned char c)
{
    return g_ascii_isalnum(c) || c == '_';
}

static size_t
ns_glsl_directive_word(const char *p, const char *end, char *out, size_t cap)
{
    size_t n = 0;
    while (p + n < end && ns_glsl_identifier_char((unsigned char)p[n])) {
        if (n + 1 < cap) out[n] = p[n];
        n++;
    }
    out[MIN(n, cap - 1)] = '\0';
    return n;
}

static const char *
ns_glsl_skip_blanks(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    return p;
}

static void
ns_glsl_check_version_directive(ns_glsl_scan *s, int line, const char *p,
                                const char *end)
{
    char number[16], profile[16];
    p = ns_glsl_skip_blanks(p, end);
    p += ns_glsl_directive_word(p, end, number, sizeof number);
    p = ns_glsl_skip_blanks(p, end);
    p += ns_glsl_directive_word(p, end, profile, sizeof profile);
    p = ns_glsl_skip_blanks(p, end);
    gboolean trailing = p < end && *p != '\n' && *p != '\r' &&
                        !(p[0] == '/' && p + 1 < end && (p[1] == '/' || p[1] == '*'));
    if (strcmp(number, "100") == 0 && !profile[0] && !trailing) {
        s->es_version = 100;
        return;
    }
    if (strcmp(number, "300") == 0 && strcmp(profile, "es") == 0 && !trailing) {
        if (s->webgl_version >= 2) {
            s->es_version = 300;
            return;
        }
        ns_glsl_error(s, line, "'#version 300 es' : a WebGL 1 context only "
                      "compiles GLSL ES 1.00 shaders");
        return;
    }
    ns_glsl_error(s, line, "'#version' : WebGL shaders are '#version 100' or "
                  "'#version 300 es'");
}

static void
ns_glsl_check_extension_directive(ns_glsl_scan *s, int line, const char *p,
                                  const char *end)
{
    char name[64];
    p = ns_glsl_skip_blanks(p, end);
    ns_glsl_directive_word(p, end, name, sizeof name);
    if (strcmp(name, "all") == 0) return;
    if (strcmp(name, "GL_OES_standard_derivatives") == 0 && s->es_version < 300)
        return;
    ns_glsl_error(s, line, "'#extension' : extension '%s' is not available to "
                  "WebGL shaders", name);
}

static const char *
ns_glsl_check_directive(ns_glsl_scan *s, int line, const char *p,
                        const char *end)
{
    static const char *const allowed[] = {
        "define", "undef", "if", "ifdef", "ifndef", "else", "elif", "endif",
        "error", "pragma", "extension", "version", "line",
    };
    char word[32];
    p = ns_glsl_skip_blanks(p + 1, end);
    size_t n = ns_glsl_directive_word(p, end, word, sizeof word);
    gboolean known = n == 0;
    for (gsize i = 0; !known && i < G_N_ELEMENTS(allowed); i++)
        known = strcmp(word, allowed[i]) == 0;
    if (!known)
        ns_glsl_error(s, line, "'#%s' : unknown preprocessor directive", word);
    else if (strcmp(word, "version") == 0)
        ns_glsl_check_version_directive(s, line, p + n, end);
    else if (strcmp(word, "extension") == 0)
        ns_glsl_check_extension_directive(s, line, p + n, end);
    return p + n;
}

static void
ns_glsl_scan_source(ns_glsl_scan *s)
{
    const char *p = s->src, *end = s->src + s->len;
    int line = 1, depth = 0;
    gboolean line_start = TRUE;
    while (p < end) {
        unsigned char c = (unsigned char)*p;
        if (c == '\n') {
            line++;
            line_start = TRUE;
            p++;
            continue;
        }
        if (c == '/' && p + 1 < end && p[1] == '/') {
            while (p < end && *p != '\n') p++;
            continue;
        }
        if (c == '/' && p + 1 < end && p[1] == '*') {
            p += 2;
            while (p < end && !(p[0] == '*' && p + 1 < end && p[1] == '/')) {
                if (*p == '\n') line++;
                p++;
            }
            p = MIN(p + 2, end);
            continue;
        }
        if (!ns_glsl_char_allowed(c, s->es_version)) {
            ns_glsl_error(s, line, "character 0x%02x is outside the GLSL ES "
                          "source character set", c);
            return;
        }
        if (c == '#') {
            if (!line_start) {
                ns_glsl_error(s, line, "'#' : must begin a preprocessor line");
                return;
            }
            p = ns_glsl_check_directive(s, line, p, end);
            continue;
        }
        if (c != ' ' && c != '\t' && c != '\r' && c != '\v' && c != '\f')
            line_start = FALSE;
        if (ns_glsl_identifier_start(c)) {
            const char *start = p;
            while (p < end && ns_glsl_identifier_char((unsigned char)*p)) p++;
            size_t n = (size_t)(p - start);
            if (n > NS_GLSL_MAX_IDENTIFIER) {
                ns_glsl_error(s, line, "identifier longer than %d characters",
                              NS_GLSL_MAX_IDENTIFIER);
                return;
            }
            if ((n >= 6 && strncmp(start, "webgl_", 6) == 0) ||
                (n >= 7 && strncmp(start, "_webgl_", 7) == 0)) {
                ns_glsl_error(s, line, "'%.*s' : identifiers starting with "
                              "'webgl_' or '_webgl_' are reserved", (int)n,
                              start);
                return;
            }
            continue;
        }
        if (g_ascii_isdigit(c)) {
            while (p < end && (ns_glsl_identifier_char((unsigned char)*p) ||
                               *p == '.'))
                p++;
            continue;
        }
        if (c == '{' || c == '(' || c == '[') {
            if (++depth > NS_GLSL_MAX_NESTING) {
                ns_glsl_error(s, line, "nesting deeper than %d levels",
                              NS_GLSL_MAX_NESTING);
                return;
            }
        } else if (c == '}' || c == ')' || c == ']') {
            if (depth > 0) depth--;
        }
        p++;
    }
}

static void
ns_glsl_find_declared_version(ns_glsl_scan *s)
{
    const char *p = s->src, *end = s->src + s->len;
    while (p < end) {
        p = ns_glsl_skip_blanks(p, end);
        if (p + 8 <= end && strncmp(p, "#version", 8) == 0) {
            const char *q = ns_glsl_skip_blanks(p + 8, end);
            if (q + 3 <= end && strncmp(q, "300", 3) == 0 &&
                s->webgl_version >= 2)
                s->es_version = 300;
            return;
        }
        if (*p == '/' && p + 1 < end && p[1] == '/') {
            while (p < end && *p != '\n') p++;
        } else if (*p == '/' && p + 1 < end && p[1] == '*') {
            p += 2;
            while (p < end && !(p[0] == '*' && p + 1 < end && p[1] == '/')) p++;
            p = MIN(p + 2, end);
        } else if (*p == '\n' || *p == '\r') {
            p++;
        } else {
            return;
        }
    }
}

static void
ns_glsl_init_once(void)
{
    static gsize once = 0;
    if (g_once_init_enter(&once)) {
        glslang_initialize_process();
        g_once_init_leave(&once, 1);
    }
}

static void
ns_glsl_es100_limits(glslang_resource_t *res)
{
    res->limits.while_loops = false;
    res->limits.non_inductive_for_loops = false;
    res->limits.general_uniform_indexing = false;
    res->limits.general_varying_indexing = false;
    res->limits.general_sampler_indexing = false;
    res->limits.general_variable_indexing = false;
    res->limits.general_attribute_matrix_vector_indexing = false;
}

static void
ns_glsl_run_glslang(ns_glsl_scan *s, gboolean fragment)
{
    ns_glsl_init_once();
    glslang_resource_t res = *glslang_default_resource();
    if (s->es_version < 300) ns_glsl_es100_limits(&res);
    g_autofree char *code = g_strndup(s->src, s->len);
    glslang_input_t in;
    memset(&in, 0, sizeof in);
    in.language = GLSLANG_SOURCE_GLSL;
    in.stage = fragment ? GLSLANG_STAGE_FRAGMENT : GLSLANG_STAGE_VERTEX;
    in.client = GLSLANG_CLIENT_NONE;
    in.target_language = GLSLANG_TARGET_NONE;
    in.code = code;
    in.default_version = 100;
    in.default_profile = GLSLANG_ES_PROFILE;
    in.messages = GLSLANG_MSG_DEFAULT_BIT;
    in.resource = &res;
    glslang_shader_t *shader = glslang_shader_create(&in);
    if (!shader) {
        ns_glsl_error(s, 0, "shader validator unavailable");
        return;
    }
    gboolean ok = glslang_shader_preprocess(shader, &in) &&
                  glslang_shader_parse(shader, &in);
    glslang_program_t *program = NULL;
    if (ok) {
        program = glslang_program_create();
        glslang_program_add_shader(program, shader);
        ok = glslang_program_link(program, GLSLANG_MSG_DEFAULT_BIT);
    }
    if (!ok) {
        const char *log = glslang_shader_get_info_log(shader);
        if ((!log || !*log) && program) log = glslang_program_get_info_log(program);
        g_string_append(s->log, log && *log ? log : "ERROR: shader rejected\n");
    }
    if (program) glslang_program_delete(program);
    glslang_shader_delete(shader);
}

char *
ns_glsl_validate(const char *src, size_t len, int webgl_version,
                 gboolean fragment)
{
    ns_glsl_scan s = {
        .src = src, .len = len, .webgl_version = webgl_version,
        .es_version = 100, .log = g_string_new(NULL),
    };
    ns_glsl_find_declared_version(&s);
    ns_glsl_scan_source(&s);
    if (!s.log->len) ns_glsl_run_glslang(&s, fragment);
    if (!s.log->len) {
        g_string_free(s.log, TRUE);
        return NULL;
    }
    return g_string_free(s.log, FALSE);
}

#else

char *
ns_glsl_validate(const char *src, size_t len, int webgl_version,
                 gboolean fragment)
{
    (void)src; (void)len; (void)webgl_version; (void)fragment;
    return g_strdup("ERROR: WebGL is not built into this binary\n");
}

#endif
