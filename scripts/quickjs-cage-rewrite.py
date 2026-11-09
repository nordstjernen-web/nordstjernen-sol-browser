#!/usr/bin/env python3
"""Check or re-apply the compressed-pointer rewrites in src/quickjs/quickjs.c.

The QuickJS fork keeps pointer fields of heap structures as 32-bit cage
offsets (JS_CPTR fields read through js_cload and written through
js_cstore; see docs/heap-hardening.md). After an upstream refresh, run

    scripts/quickjs-cage-rewrite.py --check     # list raw accesses, exit 1 if any
    scripts/quickjs-cage-rewrite.py --apply     # rewrite them in place, then re-check

The field table below is the single list of what is compressed. A new
compressed field is added here, declared JS_CPTR(T) in the struct, and
the script converts its sites. Address-taking sites (&x->field) and slot
to slot moves (weak-reference chains) are reported for manual review.
"""
import argparse
import re
import sys
from pathlib import Path

SOURCE = Path(__file__).resolve().parent.parent / "src" / "quickjs" / "quickjs.c"

OBJECT_HEAD = r"(?:JS_VALUE_GET_OBJ\(\w+\)|js_cload\(JSShape \*, [^()]*\)|\w+)(?:->\w+)*"
BYTECODE_HEAD = r"b"

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

FIELDS = [
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
    (r"slice", r"parent", "JSString *"),
] + [(OBJECT_HEAD, r"u\." + m, t) for m, t in UNION_MEMBERS.items()]

MANUAL_FIELDS = ["first_weak_ref", "next_weak_ref"]

HOST_INDEX_IDIOMS = (
    r"\(uintptr_t\)\w+->u\.opaque",
    r"u\.opaque = \(JS_CPTR\(void \*\)\)",
    r"js_ept_get\(\(const void \*\)",
)


def already_converted(line, start, end):
    before = line[:start]
    return before.endswith("js_cstore(") or re.search(r"js_cload\([^,]+, $", before) is not None


def raw_sites(text):
    out = []
    for lineno, line in enumerate(text.split("\n"), 1):
        if any(re.search(idiom, line) for idiom in HOST_INDEX_IDIOMS):
            continue
        for head, field, _type in FIELDS:
            for m in re.finditer(r"(?<![\w.>])(" + head + r"->" + field + r")\b", line):
                if not already_converted(line, m.start(1), m.end(1)):
                    out.append((lineno, line.strip()))
                    break
    return out


def address_sites(text):
    out = []
    for lineno, line in enumerate(text.split("\n"), 1):
        for head, field, _type in FIELDS:
            if re.search(r"&" + head + r"->" + field + r"\b(?!\[)", line):
                out.append((lineno, line.strip()))
                break
    return out


def apply(text):
    for head, field, type_name in FIELDS:
        write_re = re.compile(r"(?<![\w.>])(" + head + r"->" + field + r") = (?!\(JS_CPTR)([^;=][^;]*);")
        text = write_re.sub(lambda m: "js_cstore(%s, %s);" % (m.group(1), m.group(2)), text)
        read_re = re.compile(r"(?<![\w.>])(?<!\(uintptr_t\))(" + head + r"->" + field + r")\b(?! = [^=])")
        text = read_re.sub(lambda m: "js_cload(%s, %s)" % (type_name, m.group(1)), text)
        esc = re.escape(type_name)
        text = re.sub(r"js_cstore\(js_cload\(" + esc + r", ([^()]*(?:\([^()]*\))?[^()]*)\), ", r"js_cstore(\1, ", text)
        text = re.sub(r"js_cload\(" + esc + r", js_cload\(" + esc + r", ([^()]*(?:\([^()]*\))?[^()]*)\)\)", r"js_cload(" + type_name + r", \1)", text)
    return text


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--apply", action="store_true", help="rewrite raw accesses in place")
    ap.add_argument("--check", action="store_true", help="report raw accesses (default)")
    ap.add_argument("--source", default=str(SOURCE))
    args = ap.parse_args()
    path = Path(args.source)
    text = path.read_text()
    if args.apply:
        new = apply(text)
        if new != text:
            path.write_text(new)
            print("rewrote %s" % path)
        text = new
    sites = raw_sites(text)
    for lineno, line in sites:
        print("%s:%d: raw access to a compressed field: %s" % (path.name, lineno, line[:110]))
    for lineno, line in address_sites(text):
        print("%s:%d: address of a compressed field, convert by hand: %s" % (path.name, lineno, line[:110]))
    for field in MANUAL_FIELDS:
        n = len(re.findall(r"->" + field + r"\b", text))
        print("%s: %d sites of %s are slot moves and decodes; review by hand after a refresh" % (path.name, n, field))
    return 1 if sites else 0


if __name__ == "__main__":
    sys.exit(main())
