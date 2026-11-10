/* bcgen: compiles one engine script to hidden-source bytecode for embedding in the browser. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "quickjs.h"

static char *read_file(const char *path, size_t *plen)
{
    FILE *f = fopen(path, "rb");
    char *buf;
    long len;

    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0 || (len = ftell(f)) < 0 ||
        fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    buf = malloc((size_t)len + 1);
    if (!buf || fread(buf, 1, (size_t)len, f) != (size_t)len) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    buf[len] = '\0';
    *plen = (size_t)len;
    return buf;
}

int main(int argc, char **argv)
{
    JSRuntime *rt;
    JSContext *ctx;
    JSValue fn;
    uint8_t *bc;
    size_t src_len, bc_len;
    char *src;
    FILE *out;
    int status = 1;

    if (argc != 4) {
        fprintf(stderr, "usage: %s <script-name> <input.js> <output.bc>\n", argv[0]);
        return 2;
    }
    src = read_file(argv[2], &src_len);
    if (!src) {
        fprintf(stderr, "%s: cannot read %s\n", argv[0], argv[2]);
        return 1;
    }
    rt = JS_NewRuntime();
    ctx = rt ? JS_NewContext(rt) : NULL;
    if (!ctx) {
        fprintf(stderr, "%s: cannot create a context\n", argv[0]);
        return 1;
    }
    fn = JS_Eval(ctx, src, src_len, argv[1],
                 JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY |
                 JS_EVAL_FLAG_HIDE_SOURCE);
    if (JS_IsException(fn)) {
        JSValue ex = JS_GetException(ctx);
        const char *msg = JS_ToCString(ctx, ex);
        fprintf(stderr, "%s: %s: %s\n", argv[0], argv[2], msg ? msg : "compile error");
        JS_FreeCString(ctx, msg);
        JS_FreeValue(ctx, ex);
    } else {
        bc = JS_WriteObject(ctx, &bc_len, fn, JS_WRITE_OBJ_BYTECODE);
        out = bc ? fopen(argv[3], "wb") : NULL;
        if (out && fwrite(bc, 1, bc_len, out) == bc_len)
            status = 0;
        else
            fprintf(stderr, "%s: cannot write %s\n", argv[0], argv[3]);
        if (out && fclose(out) != 0)
            status = 1;
        js_free(ctx, bc);
    }
    JS_FreeValue(ctx, fn);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    free(src);
    return status;
}
