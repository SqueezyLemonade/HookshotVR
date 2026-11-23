"""hookshot_vk.dll exports every VR function the host finds in it by name (vr/vk_vr.h HOOKSHOT_VR_EXPORT).

The host's VR bridges (host/, vr/**/*_host.c) find the renderer side's functions with GetProcAddress(vk_module,
"hookshot_vr_..."). On Linux every function with default visibility is in hookshot_vk.so's dynamic symbol table; on Windows
a DLL that marks any function __declspec(dllexport) exports only those, so a name declared without it is NULL to the host
and its bridge silently does nothing. Ten were (the bow, the hookshot's ray, the radial wheel, the equipment, the wrist,
the menu screen): on Windows the bow, the hookshot, the wheel and the wrist HUD would have done nothing.

This reads every such lookup in vr/, host/ and gpu/ and checks that vr/vk_vr.h declares each name with HOOKSHOT_VR_EXPORT,
that the macro is __declspec(dllexport) on Windows and default visibility elsewhere, and that each declared name has an
extern "C" definition in vr/. With --cross <llvm-mingw clang++> --headers <Vulkan headers> it also builds a DLL of stand-in
definitions against the header and checks its export table (llvm-readobj --coff-exports beside that compiler).
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "vr" / "vk_vr.h"
LOOKUP = re.compile(r'GetProcAddress\(\s*\w+\s*,\s*"(hookshot_vr_\w+)"')
DECL = re.compile(r'^HOOKSHOT_VR_EXPORT\s+[\w\s\*]+?\b(hookshot_vr_\w+)\s*\(', re.M)
DEFN = re.compile(r'extern\s+"C"\s+[\w\s\*]+?\b(hookshot_vr_\w+)\s*\([^;{]*\)\s*\{', re.S)


def sources(*tops, exts=(".c", ".cpp", ".h", ".inc")):
    for top in tops:
        for d, ds, fs in os.walk(ROOT / top):
            ds[:] = [x for x in ds if x not in ("generated", "third_party", "__pycache__")]
            for f in fs:
                if f.endswith(exts):
                    yield Path(d) / f


def looked_up():
    names = {}
    for p in sources("vr", "host", "gpu"):
        for m in LOOKUP.finditer(p.read_text(encoding="utf-8", errors="replace")):
            names.setdefault(m.group(1), str(p.relative_to(ROOT)))
    return names


def declared(text):
    return set(DECL.findall(text))


def defined():
    return {m.group(1) for p in sources("vr", exts=(".cpp", ".inc")) for m in DEFN.finditer(p.read_text(encoding="utf-8"))}


def check_macro(text):
    m = re.search(r'#ifdef _WIN32\s*\n#define HOOKSHOT_VR_EXPORT __declspec\(dllexport\)\s*\n#else\s*\n'
                  r'#define HOOKSHOT_VR_EXPORT __attribute__\(\(visibility\("default"\)\)\)\s*\n#endif', text)
    return bool(m)


def cross_check(cxx, headers, names, text):
    """A DLL of stand-ins: the header, then a definition of each name (as vk_vr.cpp's own carry no attribute). Its exports"""
    with tempfile.TemporaryDirectory() as t:
        src = Path(t) / "exports.cpp"
        # (each stand-in has its declaration's own signature: extern "C" names one symbol, so a different one would not build)
        decls = re.findall(r'^(HOOKSHOT_VR_EXPORT\s+[^;]+);', text, re.M)
        bodies = []
        for d in decls:
            sig = d[len("HOOKSHOT_VR_EXPORT"):].strip()
            ret = re.match(r'(.*?)\bhookshot_vr_', sig).group(1).strip()
            bodies.append('extern "C" %s { %s }' % (re.sub(r"\s+", " ", sig), "" if ret == "void" else "return 0;"))
        src.write_text('#include <stdint.h>\n#include "vk_vr.h"\n' + "\n".join(bodies) + "\n", encoding="utf-8")
        dll = Path(t) / "exports.dll"
        inc = ["-I", os.path.join(headers, "include")] if os.path.isdir(os.path.join(headers or "", "include")) else ["-I", headers or "."]
        r = subprocess.run([cxx, "-std=c++17", "-shared", "-I", str(HEADER.parent)] + inc + [str(src), "-o", str(dll)],
                           capture_output=True, text=True)
        if r.returncode != 0:
            raise SystemExit("the stand-in DLL did not build:\n" + r.stdout + r.stderr)
        readobj = Path(cxx).with_name("llvm-readobj" + (".exe" if cxx.endswith(".exe") else ""))
        r = subprocess.run([str(readobj), "--coff-exports", str(dll)], capture_output=True, text=True, check=True)
        return set(re.findall(r"Name:\s*(hookshot_vr_\w+)", r.stdout))


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--cross", help="llvm-mingw's x86_64-w64-mingw32-clang++: also check a built DLL's export table")
    p.add_argument("--headers", help="with --cross: the Vulkan headers (a folder with include/vulkan/vulkan.h)")
    a = p.parse_args()
    text = HEADER.read_text(encoding="utf-8")
    bad = []
    if not check_macro(text):
        bad.append("vr/vk_vr.h: HOOKSHOT_VR_EXPORT is not __declspec(dllexport) on _WIN32 and default visibility elsewhere")
    looks, decl, defs = looked_up(), declared(text), defined()
    if len(looks) < 30:
        bad.append("only %d hookshot_vr_ lookups found: the pattern no longer matches the host's calls" % len(looks))
    for name, where in sorted(looks.items()):
        if name not in decl:
            bad.append("%s looks up %s, which vr/vk_vr.h does not declare with HOOKSHOT_VR_EXPORT" % (where, name))
    for name in sorted(decl - defs):
        bad.append("vr/vk_vr.h declares %s, which no extern \"C\" definition in vr/ makes" % name)
    if a.cross and not bad:
        exported = cross_check(a.cross, a.headers, looks, text)
        for name in sorted(set(looks) - exported):
            bad.append("a DLL built against vr/vk_vr.h does not export %s" % name)
    if bad:
        print("\n".join(bad))
        return 1
    print("%d names the host looks up, all exported%s" % (len(looks), " (and in a built DLL's export table)" if a.cross else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
