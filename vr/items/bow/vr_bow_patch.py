"""Hookshot VR, the bow: three calls added to the game's own arrow-launch function (f_82e1f2d0, game 1.1.1) so the arrow
leaves from the VR bow, in the direction the hands aim. The called function, hos_oot_vr_bow_probe, is in
vr/items/bow/bow_host.c; it only acts while Link holds the bow in first person.

Applied to the translated game code by nx2/oot_patches.py (its VR HOOK). Changing it makes the next build translate the game
again (~40 minutes)."""

DECL = "extern void hos_oot_vr_bow_probe(Ctx *c, uint64_t address, int phase);\n"


def patch_source(source):
    edits = (
        ("void f_82e1f2d0(Ctx *__restrict A64_NOESC c) {\n",
         "    hos_oot_vr_bow_probe(c, c->x[0], 0); /* VR bow: shoot entry */\n"),
        ("L_82e235f0:;", "\n    hos_oot_vr_bow_probe(c, c->x[19], 1); /* VR bow: launch basis */"),
        ("L_82e236d0:;", "\n    hos_oot_vr_bow_probe(c, c->x[19], 2); /* VR bow: alternate launch basis */"),
    )
    changed = False
    for anchor, body in edits:
        if anchor in source and body not in source:
            if source.count(anchor) != 1:
                raise RuntimeError("ambiguous VR bow probe anchor: " + anchor)
            source = source.replace(anchor, anchor + body, 1)
            changed = True
    # Keep the owner token strictly inside the shoot function, including early exits and tail calls.
    start = source.find('void f_82e1f2d0(Ctx *__restrict A64_NOESC c) {\n')
    if start >= 0:
        end = source.find('\n}\n', start)
        # (Hookshot 2.3's lift also asks the patches of one function's text alone, nx2/oot_patches.py touches(): there the
        # function's closing brace is the text's last line, with no line end after it)
        if end < 0 and source.rstrip().endswith('\n}'):
            end = len(source.rstrip()) - 2
        if end < 0: raise RuntimeError('unterminated shoot function')
        body = source[start:end]
        marker = 'hos_oot_vr_bow_probe(c, 0, 3); /* VR bow scope exit */ '
        if marker not in body:
            import re
            body = re.sub(r'(?m)^(return;|TAILCALL\([^\n]+\);)', lambda m: marker + m[0], body)
            source = source[:start] + body + source[end:]
            changed = True
    if changed and DECL not in source:
        source = source.replace('#include "a64.h"\n', '#include "a64.h"\n' + DECL, 1)
    return source
