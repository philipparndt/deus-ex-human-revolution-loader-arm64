#!/usr/bin/env python3
"""Generate mechanical OpenGL thunks.

Reads the OpenGL functions imported by the game (docs/analysis/hr-imports.tsv,
first column "OpenGL") plus a few extras, looks up their prototypes in the SDK
headers (gl3.h, gl3ext.h, gl.h, glext.h; the first header that declares a
function wins) and writes

  src/host/api/gl_gen.h  prototypes for every function (no GL headers needed,
                         so legacy and core entry points can be mixed)
  src/host/api/gl_gen.c  one THUNK per function that is not hand-written in
                         gl_special.c

Usage: tools/gen_gl_thunks.py            (run from the repository root)
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IMPORTS = os.path.join(ROOT, "docs/analysis/hr-imports.tsv")
OUT_C = os.path.join(ROOT, "src/host/api/gl_gen.c")
OUT_H = os.path.join(ROOT, "src/host/api/gl_gen.h")
HEADERS = ["gl3.h", "gl3ext.h", "gl.h", "glext.h"]

# Not imported by the game, but useful for tests / other titles / internal use.
EXTRA = [
    "glGetAttribLocation", "glGetBufferParameteriv", "glGetIntegeri_v", "glGetInteger64v",
    "glIsBuffer", "glIsProgram", "glIsShader", "glDrawRangeElements", "glMultiDrawArrays",
    "glMultiDrawElements", "glGetPointerv", "glGetUniformIndices", "glGetActiveUniform",
    "glGetActiveAttrib", "glGetActiveUniformBlockiv", "glGetActiveUniformsiv",
    "glUniform1f", "glUniform2f", "glUniform3f", "glUniform4f", "glUniformMatrix4fv",
    "glGetTexLevelParameteriv", "glTexParameteri", "glVertexAttrib4f",
    "glGetBufferPointerv", "glWaitSync", "glGetSynciv", "glIsSync",
]

# Hand-written in gl_special.c (still get a prototype in gl_gen.h).
SPECIAL = {
    "glGetString", "glGetStringi", "glShaderSource", "glTransformFeedbackVaryings",
    "glGetUniformIndices", "glMultiDrawElements", "glGetPointerv", "glGetBufferPointerv",
    "glBindBuffer", "glBindBufferARB", "glBindBufferBase", "glBindBufferRange", "glBindVertexArray", "glDeleteBuffers",
    "glDeleteVertexArrays", "glMapBuffer", "glMapBufferRange", "glUnmapBuffer",
    "glFlushMappedBufferRange", "glFlushMappedBufferRangeAPPLE", "glBufferParameteriAPPLE",
    "glFenceSync", "glClientWaitSync", "glDeleteSync", "glWaitSync", "glGetSynciv", "glIsSync",
}

# Untyped data pointers that may be buffer offsets instead (see gl_internal.h).
#   ptr     always a guest pointer
#   array   offset if an ARRAY_BUFFER is bound (gl*Pointer)
#   elem    offset if an ELEMENT_ARRAY_BUFFER is bound (index data)
#   unpack  offset if a PIXEL_UNPACK_BUFFER is bound (texture uploads)
#   pack    offset if a PIXEL_PACK_BUFFER is bound (readbacks)
VOIDPTR_KIND = {}
for f in ["glVertexAttribPointer", "glVertexAttribIPointer", "glColorPointer", "glNormalPointer",
          "glTexCoordPointer", "glVertexPointer", "glFogCoordPointerEXT", "glFogCoordPointer",
          "glSecondaryColorPointerEXT", "glSecondaryColorPointer"]:
    VOIDPTR_KIND[f] = "array"
for f in ["glDrawElements", "glDrawElementsBaseVertex", "glDrawElementsInstanced",
          "glDrawElementsInstancedBaseVertex", "glDrawRangeElements", "glDrawRangeElementsBaseVertex",
          "glDrawElementsInstancedARB"]:
    VOIDPTR_KIND[f] = "elem"
for f in ["glTexImage1D", "glTexImage2D", "glTexImage3D", "glTexSubImage1D", "glTexSubImage2D",
          "glTexSubImage3D", "glCompressedTexImage1D", "glCompressedTexImage2D",
          "glCompressedTexImage2DARB", "glCompressedTexImage3D", "glCompressedTexImage3DARB",
          "glCompressedTexSubImage1D", "glCompressedTexSubImage2D", "glCompressedTexSubImage2DARB",
          "glCompressedTexSubImage3D", "glCompressedTexSubImage3DARB"]:
    VOIDPTR_KIND[f] = "unpack"
for f in ["glReadPixels", "glGetTexImage", "glGetCompressedTexImage"]:
    VOIDPTR_KIND[f] = "pack"
for f in ["glBufferData", "glBufferSubData", "glGetBufferSubData", "glBufferDataARB",
          "glBufferSubDataARB", "glProgramStringARB", "glGetProgramStringARB"]:
    VOIDPTR_KIND[f] = "ptr"

SCALAR32 = {"GLenum", "GLuint", "GLint", "GLsizei", "GLbitfield", "GLboolean", "GLbyte", "GLubyte",
            "GLshort", "GLushort", "GLfixed", "GLhalf", "GLhalfARB", "GLchar", "GLcharARB"}
FLOAT = {"GLfloat", "GLclampf"}
DOUBLE = {"GLdouble", "GLclampd"}
INT64 = {"GLint64", "GLuint64", "GLint64EXT", "GLuint64EXT"}
INTPTR = {"GLintptr", "GLsizeiptr", "GLintptrARB", "GLsizeiptrARB"}


def sdk_headers():
    sdk = subprocess.check_output(["xcrun", "--show-sdk-path"], text=True).strip()
    return os.path.join(sdk, "System/Library/Frameworks/OpenGL.framework/Headers")


DECL_RE = re.compile(r"^(?:extern|GLAPI)\s+(.*?)\b(gl[A-Z]\w*)\s*\((.*?)\)")


def parse_header(path):
    protos = {}
    with open(path) as f:
        text = f.read()
    # Join continuation lines of declarations.
    buf = ""
    for line in text.splitlines():
        s = line.strip()
        if buf:
            buf += " " + s
        elif (s.startswith("extern ") and not s.startswith('extern "C"')) or s.startswith("GLAPI "):
            buf = s
        else:
            continue
        if ";" not in buf:
            continue
        m = DECL_RE.match(buf)
        buf = ""
        if not m:
            continue
        ret = m.group(1).replace("APIENTRY", "").strip()
        name, args = m.group(2), m.group(3).strip()
        if name not in protos:
            protos[name] = (ret, args)
    return protos


def split_param(p):
    """'const GLfloat *v' -> ('const GLfloat *', 'v')"""
    p = re.sub(r"\s+", " ", p.strip())
    m = re.match(r"^(.*?)([A-Za-z_]\w*)$", p)
    if not m or m.group(1).strip() == "":
        return p, None  # unnamed parameter
    typ = m.group(1).strip()
    return typ, m.group(2)


def norm_type(t):
    t = re.sub(r"\s*\*\s*", " *", t).strip()
    return t.replace("* *", "**")


def base_type(t):
    return t.replace("const", "").replace("*", "").strip()


def gen_thunk(name, ret, params, header):
    lines = []
    args = []
    slot = 0
    for typ, pname in params:
        t = norm_type(typ)
        b = base_type(t)
        stars = t.count("*")
        if stars >= 2:
            raise SystemExit(f"{name}: pointer-to-pointer parameter '{typ} {pname}' needs gl_special.c")
        if stars == 1:
            if b in ("GLvoid", "void"):
                kind = VOIDPTR_KIND.get(name)
                if kind is None:
                    raise SystemExit(f"{name}: untyped pointer '{typ} {pname}' not classified")
                args.append({"ptr": f"ARGP({slot})", "array": f"GLPTR_ARRAY(ARG({slot}))",
                             "elem": f"GLPTR_ELEM(ARG({slot}))", "unpack": f"GLPTR_UNPACK(ARG({slot}))",
                             "pack": f"GLPTR_PACK(ARG({slot}))"}[kind])
            elif b in INTPTR or b in ("GLsync", "GLhandleARB"):
                raise SystemExit(f"{name}: pointer to host-sized type '{typ}' needs gl_special.c")
            else:
                args.append(f"({t})ARGP({slot})")
            slot += 1
        elif b in FLOAT:
            args.append(f"ARGF({slot})")
            slot += 1
        elif b in DOUBLE:
            args.append(f"ARGD({slot})")
            slot += 2
        elif b in INT64:
            args.append(f"({b})ARG64({slot})")
            slot += 2
        elif b in INTPTR:
            args.append(f"ARGIPTR({slot})")
            slot += 1
        elif b in SCALAR32:
            args.append(f"({b})ARG({slot})")
            slot += 1
        else:
            raise SystemExit(f"{name}: unsupported parameter type '{typ}'")
    call = f"{name}({', '.join(args)})"
    r = norm_type(ret)
    if r == "void":
        body = f"{call};"
    elif base_type(r) in SCALAR32 and "*" not in r:
        body = f"RET({call});"
    else:
        raise SystemExit(f"{name}: unsupported return type '{ret}'")
    return f"THUNK({name}) {{ {body} }} // {header}"


def main():
    hdir = sdk_headers()
    protos = {}
    for h in HEADERS:
        for name, (ret, args) in parse_header(os.path.join(hdir, h)).items():
            protos.setdefault(name, (ret, args, h))

    wanted = []
    with open(IMPORTS) as f:
        for line in f:
            cols = line.rstrip("\n").split("\t")
            if len(cols) >= 2 and cols[0] == "OpenGL" and cols[1].startswith("_gl"):
                wanted.append(cols[1][1:])
    imported = set(wanted)
    wanted = sorted(set(wanted) | set(EXTRA))

    missing = [n for n in wanted if n not in protos]
    if missing:
        raise SystemExit("no prototype for: " + ", ".join(missing))

    hdr = [
        "// Generated by tools/gen_gl_thunks.py from the macOS SDK OpenGL headers. Do not edit.",
        "// Prototypes of every OpenGL function the thunks call (legacy and core mixed).",
        "#pragma once",
        "#include <OpenGL/gltypes.h>",
        "",
    ]
    out = [
        "// Generated by tools/gen_gl_thunks.py from the macOS SDK OpenGL headers. Do not edit.",
        "// Mechanical OpenGL thunks; special cases live in gl_special.c.",
        "#include \"gl_internal.h\"",
        "#include \"gl_gen.h\"",
        "",
    ]
    nspecial = ngen = 0
    for name in wanted:
        ret, args, header = protos[name]
        params = [] if args in ("", "void") else [split_param(p) for p in args.split(",")]
        decl_params = ", ".join(norm_type(t) + (f" {n}" if n else "") for t, n in params) or "void"
        hdr.append(f"extern {norm_type(ret)} {name}({decl_params});")
        tag = header + ("" if name in imported else ", extra")
        if name in SPECIAL:
            out.append(f"// {name}: gl_special.c ({tag})")
            nspecial += 1
            continue
        out.append(gen_thunk(name, ret, params, tag))
        ngen += 1
    out.append("")
    hdr.append("")
    with open(OUT_H, "w") as f:
        f.write("\n".join(hdr))
    with open(OUT_C, "w") as f:
        f.write("\n".join(out))
    print(f"{len(wanted)} functions ({len(imported)} imported): {ngen} generated, {nspecial} special",
          file=sys.stderr)


if __name__ == "__main__":
    main()
