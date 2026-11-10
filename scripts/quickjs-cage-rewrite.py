#!/usr/bin/env python3
"""Check or re-apply the compressed-pointer rewrites in the two QuickJS forks.

Both in-tree engines (src/quickjs-bellard/quickjs.c, the default, and
src/quickjs/quickjs.c, the quickjs-ng fork) keep pointer fields of heap
structures as 32-bit cage offsets (JS_CPTR fields read through js_cload
and written through js_cstore; see docs/heap-hardening.md). After an
upstream refresh, run

    scripts/quickjs-cage-rewrite.py --check     # list raw accesses, exit 1 if any
    scripts/quickjs-cage-rewrite.py --apply     # rewrite them in place, then re-check

Both engines are processed unless --engine names one.

The field table below is the single list of what is compressed. A new
compressed field is added here, declared JS_CPTR(T) in the struct, and
the script converts its sites. Address-taking sites (&x->field) and slot
to slot moves (weak-reference chains) are reported for manual review.
"""
import argparse
import re
import sys
from pathlib import Path

SRC = Path(__file__).resolve().parent.parent / "src"

OBJECT_HEAD = r"(?:JS_VALUE_GET_OBJ\(\w+\)|js_cload\(JSShape \*, [^()]*\)|\w+)(?:->\w+)*"
BYTECODE_HEAD = r"b"
TYPED_ARRAY_HEAD = r"(?:ta|dest_ta|src_ta)"
REGEXP_HEAD = r"re1?"
MODULE_HEAD = r"(?:m|m1|module|res_m)"

UNION_MEMBERS = {
    "bound_function": "struct JSBoundFunction *",
    "c_function_data_record": "struct JSCFunctionDataRecord *",
    "c_closure_record": "struct JSCClosureRecord *",
    "for_in_iterator": "struct JSForInIterator *",
    "array_buffer": "struct JSArrayBuffer *",
    "typed_array": "struct JSTypedArray *",
    "map_state": "struct JSMapState *",
    "map_iterator_data": "struct JSMapIteratorData *",
    "array_iterator_data": "struct JSArrayIteratorData *",
    "regexp_string_iterator_data": "struct JSRegExpStringIteratorData *",
    "generator_data": "struct JSGeneratorData *",
    "iterator_concat_data": "struct JSIteratorConcatData *",
    "iterator_helper_data": "struct JSIteratorHelperData *",
    "iterator_wrap_data": "struct JSIteratorWrapData *",
    "proxy_data": "struct JSProxyData *",
    "promise_data": "struct JSPromiseData *",
    "promise_function_data": "struct JSPromiseFunctionData *",
    "async_function_data": "struct JSAsyncFunctionData *",
    "async_from_sync_iterator_data": "struct JSAsyncFromSyncIteratorData *",
    "async_generator_data": "struct JSAsyncGeneratorData *",
}

NG_FIELDS = [
    (OBJECT_HEAD, r"shape", "JSShape *"),
    (OBJECT_HEAD, r"prop", "JSProperty *"),
    (OBJECT_HEAD, r"proto", "JSObject *"),
    (OBJECT_HEAD, r"shape_hash_next", "JSShape *"),
    (OBJECT_HEAD, r"u\.array\.u\.values", "JSValue *"),
    (OBJECT_HEAD, r"u\.(?:func|array\.u)\.var_refs", "JSVarRef **"),
    (OBJECT_HEAD, r"u\.func\.home_object", "JSObject *"),
    (OBJECT_HEAD, r"u\.func\.function_bytecode", "struct JSFunctionBytecode *"),
    (OBJECT_HEAD, r"u\.opaque", "void *"),
    (BYTECODE_HEAD, r"byte_code_buf", "uint8_t *"),
    (BYTECODE_HEAD, r"cpool", "JSValue *"),
    (BYTECODE_HEAD, r"closure_var", "JSClosureVar *"),
    (BYTECODE_HEAD, r"vardefs", "JSVarDef *"),
    (BYTECODE_HEAD, r"pc2line_buf", "uint8_t *"),
    (BYTECODE_HEAD, r"source", "char *"),
    (r"slice", r"parent", "JSString *"),
    (TYPED_ARRAY_HEAD, r"obj", "JSObject *"),
    (TYPED_ARRAY_HEAD, r"buffer", "JSObject *"),
    (OBJECT_HEAD, r"u\.array\.u1\.typed_array", "struct JSTypedArray *"),
    (REGEXP_HEAD, r"(?:pattern|bytecode)", "JSString *"),
    (OBJECT_HEAD, r"u\.regexp\.(?:pattern|bytecode)", "JSString *"),
    (OBJECT_HEAD, r"hash_buckets", "struct list_head *"),
    (r"mr", r"map", "struct JSMapState *"),
    (OBJECT_HEAD, r"cur_record", "JSMapRecord *"),
    (OBJECT_HEAD, r"presolved", "JSPromiseFunctionDataResolved *"),
    (r"s", r"generator", "JSObject *"),
    (r"it", r"buf", "JSValue *"),
    (r"wr", r"u\.map_record", "struct JSMapRecord *"),
    (r"wr", r"u\.weak_ref_data", "struct JSWeakRefData *"),
    (r"wr", r"u\.fin_rec_entry", "struct JSFinRecEntry *"),
    (r"pr", r"u\.getset\.(?:getter|setter)", "JSObject *"),
    (r"pr", r"u\.var_ref", "JSVarRef *"),
    (MODULE_HEAD, r"req_module_entries", "JSReqModuleEntry *"),
    (MODULE_HEAD, r"export_entries", "JSExportEntry *"),
    (MODULE_HEAD, r"star_export_entries", "JSStarExportEntry *"),
    (MODULE_HEAD, r"import_entries", "JSImportEntry *"),
    (MODULE_HEAD, r"stack_prev", "JSModuleDef *"),
    (MODULE_HEAD, r"async_parent_modules", "JSModuleDef **"),
    (MODULE_HEAD, r"cycle_root", "JSModuleDef *"),
    (r"rme2?", r"module", "JSModuleDef *"),
    (r"js_cload\(JSReqModuleEntry \*, " + MODULE_HEAD + r"->req_module_entries\)\[[^\]]+\]", r"module", "JSModuleDef *", r"\."),
    (r"(?:me|res_me)", r"u\.local\.var_ref", "JSVarRef *"),
] + [(OBJECT_HEAD, r"u\." + m, t) for m, t in UNION_MEMBERS.items()]

NG_MANUAL_FIELDS = ["first_weak_ref", "next_weak_ref"]

BELLARD_OBJECT_HEAD = r"(?:JS_VALUE_GET_OBJ\(\w+\)|js_cload\(JSShape \*, [^()]*\)|js_cload\(JSObject \*, [^()]*\)|\w+)(?:->\w+)*"
BELLARD_ABUF_HEAD = r"(?:abuf|new_abuf|src_abuf|dest_abuf|array_buffer)"
BELLARD_MODULE_HEAD = r"(?:m|m1|module|res_m|req_m)"

BELLARD_UNION_MEMBERS = {
    "bound_function": "struct JSBoundFunction *",
    "c_function_data_record": "struct JSCFunctionDataRecord *",
    "for_in_iterator": "struct JSForInIterator *",
    "array_buffer": "struct JSArrayBuffer *",
    "typed_array": "struct JSTypedArray *",
    "map_state": "struct JSMapState *",
    "map_iterator_data": "struct JSMapIteratorData *",
    "array_iterator_data": "struct JSArrayIteratorData *",
    "regexp_string_iterator_data": "struct JSRegExpStringIteratorData *",
    "generator_data": "struct JSGeneratorData *",
    "iterator_concat_data": "struct JSIteratorConcatData *",
    "iterator_helper_data": "struct JSIteratorHelperData *",
    "iterator_wrap_data": "struct JSIteratorWrapData *",
    "iterator_zip_data": "struct JSIteratorZipData *",
    "array_from_async_data": "struct JSArrayFromAsyncData *",
    "proxy_data": "struct JSProxyData *",
    "promise_data": "struct JSPromiseData *",
    "promise_function_data": "struct JSPromiseFunctionData *",
    "async_function_data": "struct JSAsyncFunctionState *",
    "async_from_sync_iterator_data": "struct JSAsyncFromSyncIteratorData *",
    "async_generator_data": "struct JSAsyncGeneratorData *",
}

BELLARD_FIELDS = [
    (BELLARD_OBJECT_HEAD, r"shape", "JSShape *"),
    (BELLARD_OBJECT_HEAD, r"prop", "JSProperty *"),
    (r"(?:sh|sh1|shape|old_sh|js_cload\(JSShape \*, [^()]*\))", r"proto", "JSObject *"),
    (BELLARD_OBJECT_HEAD, r"shape_hash_next", "JSShape *"),
    (BELLARD_OBJECT_HEAD, r"u\.array\.u\.values", "JSValue *"),
    (BELLARD_OBJECT_HEAD, r"u\.(?:func|array\.u)\.var_refs", "JSVarRef **"),
    (BELLARD_OBJECT_HEAD, r"u\.func\.home_object", "JSObject *"),
    (BELLARD_OBJECT_HEAD, r"u\.func\.function_bytecode", "struct JSFunctionBytecode *"),
    (BELLARD_OBJECT_HEAD, r"u\.opaque", "void *"),
    (r"b", r"byte_code_buf", "uint8_t *"),
    (r"b", r"cpool", "JSValue *"),
    (r"b", r"closure_var", "JSClosureVar *"),
    (r"b", r"vardefs", "JSBytecodeVarDef *"),
    (r"b", r"debug\.pc2line_buf", "uint8_t *"),
    (r"b", r"debug\.source", "char *"),
    (r"(?:ta|dest_ta|src_ta|typed_array)", r"obj", "JSObject *"),
    (r"(?:ta|dest_ta|src_ta|typed_array)", r"buffer", "JSObject *"),
    (BELLARD_OBJECT_HEAD, r"u\.array\.u1\.typed_array", "struct JSTypedArray *"),
    (r"re1?", r"(?:pattern|bytecode)", "JSString *"),
    (BELLARD_OBJECT_HEAD, r"u\.regexp\.(?:pattern|bytecode)", "JSString *"),
    (BELLARD_ABUF_HEAD, r"data", "uint8_t *"),
    (r"(?:mr|mr1)", r"hash_next", "struct JSMapRecord *"),
    (r"it", r"cur_record", "JSMapRecord *"),
    (r"it", r"tab_atom", "JSPropertyEnum *"),
    (r"it", r"(?:iters|nexts|padding)", "JSValue *"),
    (r"s", r"presolved", "JSPromiseFunctionDataResolved *"),
    (r"s", r"generator", "JSObject *"),
    (r"s", r"func_state", "JSAsyncFunctionState *"),
    (r"(?:s|ds|ns)", r"resources", "JSDisposableResource *"),
    (r"pr", r"u\.getset", "JSPropertyGetSet *"),
    (r"(?:pr|pr1)", r"u\.var_ref", "JSVarRef *"),
    (r"pr", r"u\.init", "JSPropertyAutoInit *"),
    (r"(?:js_cload\(JSPropertyGetSet \*, pr->u\.getset\)|getset|gs)", r"(?:getter|setter)", "JSObject *"),
    (BELLARD_MODULE_HEAD, r"req_module_entries", "JSReqModuleEntry *"),
    (BELLARD_MODULE_HEAD, r"export_entries", "JSExportEntry *"),
    (BELLARD_MODULE_HEAD, r"star_export_entries", "JSStarExportEntry *"),
    (BELLARD_MODULE_HEAD, r"import_entries", "JSImportEntry *"),
    (BELLARD_MODULE_HEAD, r"stack_prev", "JSModuleDef *"),
    (BELLARD_MODULE_HEAD, r"async_parent_modules", "JSModuleDef **"),
    (BELLARD_MODULE_HEAD, r"cycle_root", "JSModuleDef *"),
    (r"rme2?", r"module", "JSModuleDef *"),
    (r"js_cload\(JSReqModuleEntry \*, " + BELLARD_MODULE_HEAD + r"->req_module_entries\)\[[^\]]+\]", r"module", "JSModuleDef *", r"\."),
    (r"(?:me|res_me|me1)", r"u\.local\.var_ref", "JSVarRef *"),
] + [(BELLARD_OBJECT_HEAD, r"u\." + m, t) for m, t in BELLARD_UNION_MEMBERS.items()]

BELLARD_MANUAL_FIELDS = []

ENGINES = {
    "quickjs": (SRC / "quickjs-bellard" / "quickjs.c", BELLARD_FIELDS, BELLARD_MANUAL_FIELDS),
    "quickjs-ng": (SRC / "quickjs" / "quickjs.c", NG_FIELDS, NG_MANUAL_FIELDS),
}

FIELDS = NG_FIELDS
MANUAL_FIELDS = NG_MANUAL_FIELDS

HOST_INDEX_IDIOMS = (
    r"\(uintptr_t\)\w+->u\.opaque",
    r"u\.opaque = \(JS_CPTR\(void \*\)\)",
    r"js_ept_get\(\(const void \*\)",
)


def already_converted(line, start, end):
    before = line[:start]
    return before.endswith(("js_cstore(", "js_resize_cptr_array(ctx, ")) or re.search(r"js_cload\([^,]+, $", before) is not None


COMMENT_RE = re.compile(r"/\*.*?\*/|(?<![:\"'\\])//[^\n]*", re.S)


def blank_comments(text):
    return COMMENT_RE.sub(lambda m: re.sub(r"[^\n]", " ", m.group(0)), text)


def raw_sites(text):
    out = []
    for lineno, line in enumerate(blank_comments(text).split("\n"), 1):
        if any(re.search(idiom, line) for idiom in HOST_INDEX_IDIOMS):
            continue
        for head, field, _type, *sep in FIELDS:
            sep = sep[0] if sep else r"->"
            for m in re.finditer(r"(?<![\w.>])(" + head + sep + field + r")\b", line):
                if not already_converted(line, m.start(1), m.end(1)):
                    out.append((lineno, line.strip()))
                    break
    return out


def address_sites(text):
    out = []
    for lineno, line in enumerate(blank_comments(text).split("\n"), 1):
        for head, field, _type, *sep in FIELDS:
            sep = sep[0] if sep else r"->"
            if re.search(r"&" + head + sep + field + r"\b(?!\[)", line):
                out.append((lineno, line.strip()))
                break
    return out


def apply(text):
    comments = []

    def stash(m):
        comments.append(m.group(0))
        return "\0%d\0" % (len(comments) - 1)

    text = COMMENT_RE.sub(stash, text)
    for head, field, type_name, *sep in FIELDS:
        sep = sep[0] if sep else r"->"
        write_re = re.compile(r"(?<![\w.>])(" + head + sep + field + r") = (?!\(JS_CPTR)([^;=][^;]*);")
        text = write_re.sub(lambda m: "js_cstore(%s, %s);" % (m.group(1), m.group(2)), text)
        read_re = re.compile(r"(?<![\w.>])(?<!\(uintptr_t\))(?<!js_resize_cptr_array\(ctx, )(" + head + sep + field + r")\b(?! = [^=])")
        text = read_re.sub(lambda m: "js_cload(%s, %s)" % (type_name, m.group(1)), text)
        esc = re.escape(type_name)
        text = re.sub(r"js_cstore\(js_cload\(" + esc + r", ([^()]*(?:\([^()]*\))?[^()]*)\), ", r"js_cstore(\1, ", text)
        text = re.sub(r"js_cload\(" + esc + r", js_cload\(" + esc + r", ([^()]*(?:\([^()]*\))?[^()]*)\)\)", r"js_cload(" + type_name + r", \1)", text)
    return re.sub(r"\0(\d+)\0", lambda m: comments[int(m.group(1))], text)


def run(engine, do_apply, source=None):
    global FIELDS, MANUAL_FIELDS
    path, FIELDS, MANUAL_FIELDS = ENGINES[engine]
    if source:
        path = Path(source)
    text = path.read_bytes().decode()
    if do_apply:
        new = apply(text)
        if new != text:
            path.write_bytes(new.encode())
            print("rewrote %s" % path)
        text = new
    sites = raw_sites(text)
    name = "%s/%s" % (path.parent.name, path.name)
    for lineno, line in sites:
        print("%s:%d: raw access to a compressed field: %s" % (name, lineno, line[:110]))
    for lineno, line in address_sites(text):
        print("%s:%d: address of a compressed field, convert by hand: %s" % (name, lineno, line[:110]))
    for field in MANUAL_FIELDS:
        n = len(re.findall(r"->" + field + r"\b", text))
        print("%s: %d sites of %s are slot moves and decodes; review by hand after a refresh" % (name, n, field))
    return 1 if sites else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--apply", action="store_true", help="rewrite raw accesses in place")
    ap.add_argument("--check", action="store_true", help="report raw accesses (default)")
    ap.add_argument("--engine", choices=sorted(ENGINES), help="process one engine instead of both")
    ap.add_argument("--source", help="path of quickjs.c, with --engine")
    args = ap.parse_args()
    if args.source and not args.engine:
        ap.error("--source needs --engine")
    status = 0
    for engine in ([args.engine] if args.engine else sorted(ENGINES)):
        status |= run(engine, args.apply, args.source)
    return status


if __name__ == "__main__":
    sys.exit(main())
