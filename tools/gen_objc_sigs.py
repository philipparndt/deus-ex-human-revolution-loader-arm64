#!/usr/bin/env python3
"""Generate the i386 / x86_64 Objective-C method signature table.

The guest (32-bit, legacy ObjC runtime) pushes i386-typed arguments, but the
host runtime only knows 64-bit type encodings, which cannot tell CGFloat from
double or NSInteger from long long.  This tool extracts, for every selector the
game sends (docs/analysis/hr-selectors.txt), every selector the game's own
classes implement (method_name in docs/analysis/hr-objc.txt -- overrides and
delegate callbacks the host may call into the guest), every method of the SDK
protocols the game's classes adopt (protocol_name in hr-objc.txt) and a few
EXTRA selectors, this tool extracts every declaration of that selector in the
SDK frameworks listed in FRAMEWORKS (class interfaces, categories -- recorded under their class --
and protocols) and lets clang itself compute the type encodings:

  1. Parse the SDK headers twice with clang's textual AST dump:
       i386-apple-macosx10.8 -fobjc-runtime=macosx-fragile   (CGFloat=float,
           NSInteger=int, NSRect=struct _NSRect, BOOL=signed char, ...)
       x86_64-apple-macosx10.13                              (host ABI)
     and collect every ObjCMethodDecl (including implicit property accessors)
     whose selector is in the list.
  2. Re-declare each method (types copied from the AST, generic type
     parameters replaced by id) in its own probe protocol, compile the probe
     file to assembly for the same target, and read the method type strings
     out of the protocol method lists.  The encodings are therefore exactly
     what clang emits for i386 / x86_64 (e.g. "v28@0:4{_NSRect=...}8c24").
     Probes that fail to compile with the written type are retried with the
     canonical (desugared) type; anything still failing is reported.
  3. Write src/host/api/objc_sigs_gen.c (sorted table + lookup functions,
     declared in src/host/api/objc_sigs.h) and docs/analysis/objc-sigs-report.txt.

A few SDK headers do not compile for i386 any more (classes that only exist
under __OBJC2__ but are referenced unconditionally); the prelude stubs them
and an overlay directory replaces GameController.h (pulled in by GameKit.h)
with an empty header.  Neither affects the classes the game uses.

Usage (from anywhere; needs Xcode / clang, Python 3, no extra modules):
    tools/gen_objc_sigs.py [--work DIR] [--keep]
--work DIR   directory for intermediate files (AST dumps, probes); default is a
             fresh temporary directory, removed afterwards unless --keep.
The output is deterministic for a given SDK.
"""
import argparse
import collections
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SELECTORS = os.path.join(ROOT, "docs/analysis/hr-selectors.txt")
OBJC_DUMP = os.path.join(ROOT, "docs/analysis/hr-objc.txt")

# Not in hr-selectors.txt but wanted for the bridge / its tests (may be sent
# via sel_registerName or by the host side).
EXTRA = ["setFrame:display:", "setValues:forParameter:", "getValues:forParameter:",
         "setFrame:", "setContentSize:", "makeKeyAndOrderFront:", "flushBuffer",
         "makeCurrentContext", "setView:", "update", "initWithFormat:shareContext:"]
CLASSREFS = os.path.join(ROOT, "docs/analysis/hr-classrefs.txt")
OUT_C = os.path.join(ROOT, "src/host/api/objc_sigs_gen.c")
OUT_REPORT = os.path.join(ROOT, "docs/analysis/objc-sigs-report.txt")

TARGETS = {
    "i386": ["-target", "i386-apple-macosx10.8", "-fobjc-runtime=macosx-fragile"],
    "x64": ["-target", "x86_64-apple-macosx10.13"],
}

PRELUDE = r"""
#if !__LP64__
/* Declared only under __OBJC2__ but referenced unconditionally by other
   headers; stubbed so the i386 parse is error free. */
#import <Foundation/NSObject.h>
@interface NSItemProvider : NSObject @end
typedef NSInteger NSItemProviderRepresentationVisibility;
@interface NSExtensionContext : NSObject @end
@interface NSUserActivity : NSObject @end
@interface AUAudioUnit : NSObject @end
typedef NSInteger AVSpeechSynthesisVoiceGender;
#endif
#import <Foundation/Foundation.h>
#import <Cocoa/Cocoa.h>
#import <GameKit/GameKit.h>
#import <StoreKit/StoreKit.h>
#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <ScriptingBridge/ScriptingBridge.h>
#import <QuartzCore/QuartzCore.h>
"""

# Only declarations from these frameworks (plus the objc runtime headers for
# NSObject) go into the table; the umbrella headers drag in Metal, SceneKit,
# ModelIO, ... whose same-named selectors are irrelevant to the game.
FRAMEWORKS = ("Foundation", "AppKit", "GameKit", "StoreKit", "AVFoundation", "AVFAudio",
              "CoreMedia", "ScriptingBridge", "QuartzCore")
RE_FRAMEWORK = re.compile(r"/Frameworks/(\w+)\.framework/")

RE_LOC = re.compile(r"(/[^\s:<>]+|<scratch space>|<built-in>):\d+:\d+")

CONTAINER_KINDS = ("ObjCInterfaceDecl", "ObjCCategoryDecl", "ObjCProtocolDecl")

RE_NODE = re.compile(r"^([| `]*)[|`]-(\w+)(?: (.*))?$")
RE_METHOD = re.compile(r" (implicit )?([-+]) (\S+) '([^']*)'(?::'([^']*)')?( variadic)?\s*$")
RE_PARM = re.compile(r" '([^']*)'(?::'([^']*)')?[^']*$")
RE_LASTWORD = re.compile(r" (\w+)$")
RE_QUOTED = re.compile(r"'([^']*)'")
RE_TPARAM = re.compile(r" col:\d+ (?:referenced )?(\w+)(?: \w+)* '([^']*)'")


def sh(cmd, **kw):
    return subprocess.run(cmd, check=False, text=True, capture_output=True, **kw)


class Decl:
    """One ObjCMethodDecl: container, ckind, category, is_class, selector, ret,
    ret_canon, params [(written, canonical)], variadic, implicit, tparams,
    order, types (encoding computed by clang, or None)."""

    def __init__(self):
        self.params = []
        self.types = None


def parse_ast(path, wanted, protocols):
    """Stream the textual AST dump; return (decls, superclasses)."""
    decls = []
    supers = {}
    cont = None          # (kind, name, category, tparams list) of depth-0 node
    meth = None
    order = 0
    cur_file = ""        # clang prints a file name only when it changes
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = RE_NODE.match(line.rstrip("\n"))
            if not m:
                continue
            depth = len(m.group(1)) // 2
            kind, rest = m.group(2), m.group(3) or ""
            locs = RE_LOC.findall(rest)
            node_file = cur_file
            if rest.startswith("0x") and locs:
                # the first location of a node is the begin of its range
                br = rest.find("<")
                if br >= 0 and rest[br + 1:].startswith(locs[0]):
                    node_file = locs[0]
            if locs:
                cur_file = locs[-1]
            if depth == 0:
                meth = None
                cont = None
                if kind in CONTAINER_KINDS:
                    w = RE_LASTWORD.search(rest)
                    name = w.group(1) if w else ""
                    if kind == "ObjCCategoryDecl":
                        # category name; the class follows as "ObjCInterface" child
                        cont = [kind, None, name if w else "", []]
                    else:
                        cont = [kind, name, None, []]
                continue
            if cont is None:
                continue
            if depth == 1:
                meth = None
                if kind == "ObjCInterface" and cont[0] == "ObjCCategoryDecl":
                    q = RE_QUOTED.search(rest)
                    if q:
                        cont[1] = q.group(1)
                elif kind == "super" and cont[0] == "ObjCInterfaceDecl":
                    q = RE_QUOTED.search(rest)
                    if q:
                        supers.setdefault(cont[1], q.group(1))
                elif kind == "ObjCTypeParamDecl":
                    t = RE_TPARAM.search(rest)
                    if t:
                        cont[3].append((t.group(1), t.group(2)))
                elif kind == "ObjCMethodDecl":
                    mm = RE_METHOD.search(rest)
                    fw = RE_FRAMEWORK.search(node_file)
                    in_scope = (fw.group(1) in FRAMEWORKS) if fw else "/usr/include/objc/" in node_file
                    if (mm and cont[1] and in_scope and
                            (mm.group(3) in wanted or
                             (cont[0] == "ObjCProtocolDecl" and cont[1] in protocols))):
                        d = Decl()
                        d.container = cont[1]
                        d.ckind = cont[0]
                        d.category = cont[2]
                        d.implicit = bool(mm.group(1))
                        d.is_class = mm.group(2) == "+"
                        d.selector = mm.group(3)
                        d.ret = mm.group(4)
                        d.ret_canon = mm.group(5) or mm.group(4)
                        d.variadic = bool(mm.group(6))
                        d.tparams = cont[3]
                        d.order = order
                        order += 1
                        decls.append(d)
                        meth = d
                continue
            if depth == 2 and meth is not None and kind == "ParmVarDecl":
                pm = RE_PARM.search(rest)
                if pm:
                    meth.params.append((pm.group(1), pm.group(2) or pm.group(1)))
    return decls, supers


def subst_tparams(t, params):
    """Replace generic type parameters (ObjectType, KeyType, ...) by their bound."""
    for n, bound in params:
        t = re.sub(r"\b%s\b" % re.escape(n), bound, t)
    return t


def decl_text(d, canon):
    def ty(written, desugared):
        return subst_tparams(desugared if canon else written, d.tparams)

    s = "%s (%s)" % ("+" if d.is_class else "-", ty(d.ret, d.ret_canon))
    pieces = d.selector.split(":")
    if len(pieces) == 1:
        return s + pieces[0] + ";"
    pieces = pieces[:-1]
    if len(pieces) != len(d.params):
        return None
    parts = []
    for i, (piece, (w, c)) in enumerate(zip(pieces, d.params)):
        parts.append("%s:(%s)a%d" % (piece, ty(w, c), i))
    s += " ".join(parts)
    if d.variadic:
        s += ", ..."
    return s + ";"


def compile_probes(work, tname, flags, sdk, probes):
    """probes: dict id -> declaration text.  Returns (types, failed_ids)."""
    src = os.path.join(work, "probe_%s.m" % tname)
    asm = os.path.join(work, "probe_%s.s" % tname)
    remaining = dict(probes)
    failed = set()
    for _attempt in range(20):
        lines = ['#include "prelude.h"']
        line_owner = {}
        for pid in sorted(remaining):
            lines.append("@protocol DXP_%d" % pid)
            lines.append(remaining[pid])
            line_owner[len(lines)] = pid
            lines.append("@end")
        lines.append("void dxp_refs(void **o) {")
        for i, pid in enumerate(sorted(remaining)):
            lines.append("  o[%d] = (void *)@protocol(DXP_%d);" % (i, pid))
            line_owner[len(lines)] = pid
        lines.append("}")
        with open(src, "w") as f:
            f.write("\n".join(lines) + "\n")
        r = sh(["clang"] + flags + ["-isysroot", sdk, "-F", os.path.join(work, "ovl"),
                                    "-w", "-ferror-limit=0", "-S", src, "-o", asm], cwd=work)
        if r.returncode == 0:
            break
        bad = set()
        for em in re.finditer(r"probe_%s\.m:(\d+):\d+: error" % tname, r.stderr):
            ln = int(em.group(1))
            if ln in line_owner:
                bad.add(line_owner[ln])
        if not bad:
            sys.exit("probe compile for %s failed with unattributable errors:\n%s"
                     % (tname, r.stderr[:4000]))
        failed |= bad
        for b in bad:
            del remaining[b]
    else:
        sys.exit("probe compile for %s did not converge" % tname)

    # Parse the assembly: label -> string, then protocol method lists.
    strings = {}
    text = open(asm).read().splitlines()
    for i, l in enumerate(text):
        lm = re.match(r"^(\S+):", l)
        if lm and i + 1 < len(text):
            am = re.match(r'^\s*\.asciz\s+"(.*)"\s*$', text[i + 1])
            if am:
                strings[lm.group(1)] = am.group(1).replace('\\"', '"').replace("\\\\", "\\")
    types = {}
    for i, l in enumerate(text):
        lm = re.match(r"^\S*PROTOCOL_(?:INSTANCE|CLASS)_METHODS_DXP_(\d+):", l)
        if not lm:
            continue
        pid = int(lm.group(1))
        for l2 in text[i + 1:i + 8]:
            tm = re.match(r"^\s*\.(?:long|quad)\s+(L_OBJC_METH_VAR_TYPE_\S*)", l2)
            if tm:
                types[pid] = strings[tm.group(1)]
                break
    return types, failed


def run_target(work, tname, sdk, wanted, protocols):
    flags = TARGETS[tname]
    ast = os.path.join(work, "ast_%s.txt" % tname)
    if not os.path.exists(ast):
        with open(ast, "w") as out:
            r = subprocess.run(["clang"] + flags + ["-isysroot", sdk, "-F", os.path.join(work, "ovl"),
                                                    "-fsyntax-only", "-fno-color-diagnostics",
                                                    "-Xclang", "-ast-dump", "ast.m"],
                               cwd=work, stdout=out, stderr=subprocess.PIPE, text=True)
        if r.returncode != 0:
            os.unlink(ast)
            sys.exit("AST dump for %s failed:\n%s" % (tname, r.stderr[:4000]))
    decls, supers = parse_ast(ast, wanted, protocols)
    probes = {}
    for i, d in enumerate(decls):
        t = decl_text(d, False)
        if t:
            probes[i] = t
    types, failed = compile_probes(work, tname, flags, sdk, probes)
    retry = {}
    for i in failed:
        t = decl_text(decls[i], True)
        if t:
            retry[i] = t
    unprobed = set(range(len(decls))) - set(probes)
    if retry:
        t2, failed2 = compile_probes(work, tname + "_canon", flags, sdk, retry)
        types.update(t2)
    else:
        failed2 = set()
    hard_fail = [decls[i] for i in sorted(failed2 | unprobed | (failed - set(retry)))]
    for i, d in enumerate(decls):
        d.types = types.get(i)
    return decls, supers, hard_fail


def norm_abi(t):
    """Strip things that do not change the calling convention: struct tags,
    type qualifiers, and pointee details."""
    if t is None:
        return None
    t = re.sub(r"[rnNoORV]", "", t)          # qualifiers (r const, V oneway, ...)
    t = re.sub(r"\{[^=}{]*=", "{", t)         # struct tags
    t = re.sub(r"\{[^=}{]*\}", "{}", t)
    # pointee types are irrelevant for argument passing
    out, i = [], 0
    while i < len(t):
        c = t[i]
        if c == "^":
            j = i + 1
            depth = 0
            while j < len(t):
                cj = t[j]
                if cj in "{([":
                    depth += 1
                elif cj in "})]":
                    depth -= 1
                elif cj == "^":
                    j += 1
                    continue
                j += 1
                if depth == 0:
                    break
            out.append("^")
            i = j
            continue
        if c == "@" and i + 1 < len(t) and t[i + 1] == "?":
            out.append("@")
            i += 2
            continue
        out.append(c)
        i += 1
    s = "".join(out)
    return s.replace("#", "@")  # Class vs id: same passing


def cstr(s):
    if s is None:
        return "NULL"
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--work", help="intermediate directory (kept)")
    ap.add_argument("--keep", action="store_true", help="keep temporary directory")
    args = ap.parse_args()

    sdk = sh(["xcrun", "--show-sdk-path"]).stdout.strip()
    if not sdk:
        sys.exit("xcrun --show-sdk-path failed")
    selectors = [l.strip() for l in open(SELECTORS) if l.strip()]
    dump = open(OBJC_DUMP, encoding="utf-8", errors="replace").read()
    implemented = sorted(set(re.findall(r"^\s*method_name 0x[0-9a-f]+ (\S+)\s*$", dump, re.M)))
    protocols = sorted(set(re.findall(r"^\s*protocol_name 0x[0-9a-f]+ (\S+)\s*$", dump, re.M)))
    wanted = set(selectors) | set(implemented) | set(EXTRA)
    classrefs = [l.strip() for l in open(CLASSREFS) if l.strip()]

    work = args.work or tempfile.mkdtemp(prefix="objc_sigs_")
    os.makedirs(os.path.join(work, "ovl/GameController.framework/Headers"), exist_ok=True)
    with open(os.path.join(work, "ovl/GameController.framework/Headers/GameController.h"), "w") as f:
        f.write("/* empty: GameController does not parse for i386 and is not used */\n")
    with open(os.path.join(work, "prelude.h"), "w") as f:
        f.write(PRELUDE)
    with open(os.path.join(work, "ast.m"), "w") as f:
        f.write('#include "prelude.h"\n')

    try:
        res = {}
        for tname in ("i386", "x64"):
            res[tname] = run_target(work, tname, sdk, wanted, set(protocols))
    finally:
        if not args.work and not args.keep:
            shutil.rmtree(work, ignore_errors=True)
        elif not args.work:
            print("work dir:", work)

    # Join: key (selector, container, is_class) -> first declaration per target
    entries = {}
    redecl_conflicts = []
    for tname in ("i386", "x64"):
        decls = res[tname][0]
        for d in sorted(decls, key=lambda d: (d.implicit, d.order)):
            if d.types is None:
                continue
            key = (d.selector, d.container, int(d.is_class))
            e = entries.setdefault(key, {"i386": None, "x64": None, "protocol": d.ckind == "ObjCProtocolDecl"})
            if e[tname] is None:
                e[tname] = d.types
            elif e[tname] != d.types and tname == "i386":
                redecl_conflicts.append((key, e[tname], d.types))
    supers = dict(res["x64"][1])
    supers.update(res["i386"][1])

    keys = sorted(entries)
    # superclass closure for containers + classrefs
    need = set(k[1] for k in keys if not entries[k]["protocol"]) | set(classrefs)
    sup_rows = {}
    stack = list(need)
    while stack:
        c = stack.pop()
        if c in sup_rows or c not in supers:
            continue
        sup_rows[c] = supers[c]
        stack.append(supers[c])

    write_c(keys, entries, sup_rows)
    write_report(selectors, implemented, protocols, keys, entries, res, redecl_conflicts,
                 sdk, sup_rows, classrefs)


def write_c(keys, entries, sup_rows):
    o = []
    o.append("/* Generated by tools/gen_objc_sigs.py -- do not edit.")
    o.append(" *")
    o.append(" * i386 (guest) and x86_64 (host) Objective-C method type encodings of every")
    o.append(" * SDK declaration of the selectors the game sends (hr-selectors.txt) or")
    o.append(" * implements (hr-objc.txt), of all methods of the SDK protocols its classes")
    o.append(" * adopt, and of the EXTRA selectors in the generator.")
    o.append(" * container = class (categories folded into their class) or protocol.")
    o.append(" * Sorted by (selector, container, is_class_method) using strcmp order.")
    o.append(" * i386_types / x64_types are NULL when the method is not declared for that")
    o.append(" * architecture in the SDK headers. */")
    o.append("#include <stddef.h>")
    o.append("#include <string.h>")
    o.append('#include "objc_sigs.h"')
    o.append("")
    o.append("static const struct objc_sig objc_sigs[] = {")
    for k in keys:
        e = entries[k]
        o.append("    {%s, %s, %d, %s, %s}," % (cstr(k[1]), cstr(k[0]), k[2], cstr(e["i386"]), cstr(e["x64"])))
    o.append("};")
    o.append("")
    o.append("#define OBJC_SIGS_COUNT (sizeof(objc_sigs) / sizeof(objc_sigs[0]))")
    o.append("")
    o.append("/* SDK superclasses of the classes above and of docs/analysis/hr-classrefs.txt,")
    o.append(" * sorted by class. */")
    o.append("static const struct { const char *cls, *super; } objc_sig_supers[] = {")
    for c in sorted(sup_rows):
        o.append("    {%s, %s}," % (cstr(c), cstr(sup_rows[c])))
    o.append("};")
    o.append("")
    o.append(C_FUNCS)
    with open(OUT_C, "w") as f:
        f.write("\n".join(o))


C_FUNCS = r"""const struct objc_sig *objc_sig_table(size_t *count)
{
    if (count)
        *count = OBJC_SIGS_COUNT;
    return objc_sigs;
}

static int sig_cmp(const char *sel, const char *cont, int kind, const struct objc_sig *e)
{
    int c = strcmp(sel, e->selector);
    if (c)
        return c;
    c = strcmp(cont, e->container);
    if (c)
        return c;
    return kind - (int)e->is_class_method;
}

const struct objc_sig *objc_sig_lookup(const char *container, const char *selector, int is_class_method)
{
    size_t lo = 0, hi = OBJC_SIGS_COUNT;
    int kind = is_class_method ? 1 : 0;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = sig_cmp(selector, container, kind, &objc_sigs[mid]);
        if (c == 0)
            return &objc_sigs[mid];
        if (c < 0)
            hi = mid;
        else
            lo = mid + 1;
    }
    return NULL;
}

const struct objc_sig *objc_sig_lookup_any_ex(const char *selector, int is_class_method, int *conflict)
{
    size_t lo = 0, hi = OBJC_SIGS_COUNT, i, j;
    unsigned char kind = is_class_method ? 1 : 0;
    const struct objc_sig *best = NULL;
    int best_n = 0, distinct = 0;
    /* lower bound of selector */
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (strcmp(objc_sigs[mid].selector, selector) < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    for (i = lo; i < OBJC_SIGS_COUNT && strcmp(objc_sigs[i].selector, selector) == 0; i++) {
        const struct objc_sig *e = &objc_sigs[i];
        int n = 0, seen = 0;
        if (e->is_class_method != kind || !e->i386_types)
            continue;
        for (j = lo; j < OBJC_SIGS_COUNT && strcmp(objc_sigs[j].selector, selector) == 0; j++) {
            const struct objc_sig *f = &objc_sigs[j];
            if (f->is_class_method != kind || !f->i386_types || strcmp(f->i386_types, e->i386_types))
                continue;
            if (j < i)
                seen = 1;
            n++;
        }
        if (!seen)
            distinct++;
        if (n > best_n) {
            best_n = n;
            best = e;
        }
    }
    if (conflict)
        *conflict = distinct > 1;
    return best;
}

const struct objc_sig *objc_sig_lookup_any(const char *selector, int is_class_method)
{
    return objc_sig_lookup_any_ex(selector, is_class_method, NULL);
}

const char *objc_sig_superclass(const char *cls)
{
    size_t lo = 0, hi = sizeof(objc_sig_supers) / sizeof(objc_sig_supers[0]);
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = strcmp(cls, objc_sig_supers[mid].cls);
        if (c == 0)
            return objc_sig_supers[mid].super;
        if (c < 0)
            hi = mid;
        else
            lo = mid + 1;
    }
    return NULL;
}

const struct objc_sig *objc_sig_lookup_inherited(const char *cls, const char *selector, int is_class_method)
{
    const struct objc_sig *e;
    int guard = 0;
    for (; cls && guard < 64; cls = objc_sig_superclass(cls), guard++) {
        e = objc_sig_lookup(cls, selector, is_class_method);
        if (e && e->i386_types)
            return e;
    }
    /* class methods of the root class are also reachable as instance methods */
    return objc_sig_lookup_any(selector, is_class_method);
}
"""


def write_report(selectors, implemented, protocols, keys, entries, res, redecl_conflicts,
                 sdk, sup_rows, classrefs):
    relevant = set()
    for c in classrefs:
        while c and c not in relevant:
            relevant.add(c)
            c = sup_rows.get(c)
    by_sel = collections.defaultdict(list)
    for k in keys:
        by_sel[k[0]].append(k)
    resolved = [s for s in selectors if any(entries[k]["i386"] for k in by_sel.get(s, []))]
    x64_only = [s for s in selectors if s in by_sel and s not in resolved]
    unresolved = [s for s in selectors if s not in by_sel]
    r = []
    r.append("Objective-C signature table report (generated by tools/gen_objc_sigs.py)")
    r.append("SDK: %s" % sdk)
    r.append("")
    r.append("selectors in hr-selectors.txt:          %d" % len(selectors))
    r.append("resolved (i386 declaration found):      %d" % len(resolved))
    r.append("declared only for x86_64 (no i386):     %d" % len(x64_only))
    r.append("unresolved (no SDK declaration):        %d" % len(unresolved))
    impl_sdk = [s for s in implemented if any(entries[k]["i386"] for k in by_sel.get(s, []))]
    proto_found = sorted(set(k[1] for k in keys if entries[k]["protocol"]) & set(protocols))
    r.append("selectors implemented by the game's classes (hr-objc.txt): %d, of which declared")
    r[-1] = r[-1] % len(implemented)
    r.append("  in the SDK (overrides / delegate callbacks): %d" % len(impl_sdk))
    r.append("protocols adopted by the game's classes: %d, SDK ones included with all methods: %s"
             % (len(protocols), ", ".join(proto_found)))
    r.append("extra selectors (EXTRA in the generator): %s" % ", ".join(EXTRA))
    r.append("table entries (selector x container x kind): %d" % len(keys))
    r.append("  with i386 types: %d, with x86_64 types: %d" % (
        sum(1 for k in keys if entries[k]["i386"]), sum(1 for k in keys if entries[k]["x64"])))
    r.append("")
    for tname in ("i386", "x64"):
        hf = res[tname][2]
        r.append("%s declarations that could not be probed: %d" % (tname, len(hf)))
        for d in hf:
            r.append("  %s[%s %s]" % ("+" if d.is_class else "-", d.container, d.selector))
    r.append("")
    r.append("== Unresolved sent selectors (game's own classes / private API / not in SDK) ==")
    for s in unresolved:
        r.append("  " + s)
    r.append("")
    r.append("== Declared only in 64-bit headers (no i386 declaration) ==")
    for s in x64_only:
        r.append("  %s  (%s)" % (s, ", ".join("%s%s" % ("+" if k[2] else "-", k[1]) for k in by_sel[s])))
    r.append("")

    abi, cosmetic, game_abi = [], [], []
    r.append("== Game-implemented selectors that are declared in the SDK (%d) ==" % len(impl_sdk))
    for s in impl_sdk:
        r.append("  %s  (%s)" % (s, ", ".join("%s%s" % ("+" if k[2] else "-", k[1]) for k in by_sel[s])))
    r.append("")
    for s in sorted(by_sel):
        for kind in (0, 1):
            ks = [k for k in by_sel.get(s, []) if k[2] == kind and entries[k]["i386"]]
            ts = collections.OrderedDict()
            for k in ks:
                ts.setdefault(entries[k]["i386"], []).append(k[1])
            if len(ts) < 2:
                continue
            item = (s, kind, ts)
            if len(set(norm_abi(t) for t in ts)) > 1:
                abi.append(item)
                rel = set(norm_abi(t) for t, cs in ts.items() if any(c in relevant for c in cs))
                if len(rel) > 1:
                    game_abi.append(item)
            else:
                cosmetic.append(item)

    def dump(items):
        for s, kind, ts in items:
            r.append("  %s%s" % ("+" if kind else "-", s))
            for t, cs in ts.items():
                r.append("      %-40s %s" % (t, ", ".join(c + ("*" if c in relevant else "") for c in cs)))

    r.append("Containers marked * are classes in hr-classrefs.txt or their SDK superclasses.")
    r.append("")
    r.append("== i386 layout conflicts between two of those game classes (%d) ==" % len(game_abi))
    dump(game_abi)
    r.append("")
    r.append("== i386 conflicts across containers that change the argument layout (%d) ==" % len(abi))
    r.append("   (use objc_sig_lookup_inherited() with the receiver's class for these)")
    dump(abi)
    r.append("")
    r.append("== i386 differences that do not change argument passing (struct tags, const,")
    r.append("   pointee types, Class vs id, block vs id) (%d) ==" % len(cosmetic))
    dump(cosmetic)
    r.append("")
    r.append("== i386 redeclarations of the same container/selector with different types (%d) ==" % len(redecl_conflicts))
    for key, a, b in redecl_conflicts:
        r.append("  %s[%s %s]: kept %s, also %s" % ("+" if key[2] else "-", key[1], key[0], a, b))
    with open(OUT_REPORT, "w") as f:
        f.write("\n".join(r) + "\n")
    print("resolved %d/%d selectors, %d entries, %d ABI conflicts; wrote %s and %s" % (
        len(resolved), len(selectors), len(keys), len(abi),
        os.path.relpath(OUT_C, ROOT), os.path.relpath(OUT_REPORT, ROOT)))


if __name__ == "__main__":
    main()
