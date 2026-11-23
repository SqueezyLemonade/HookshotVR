# EB Garamond

EB Garamond by The EB Garamond Project Authors (github.com/octaviopardo/EBGaramond12), under the SIL Open Font License
1.1 (`OFL.txt`), which names no Reserved Font Name. From github.com/google/fonts, `ofl/ebgaramond` on 20 Nov 2026: the
variable fonts `EBGaramond[wght].ttf` and `EBGaramond-Italic[wght].ttf`.

The launcher's text engine (ImGui's stb_truetype) draws only a variable font's default weight, so these are static cuts
of them, made with fontTools 4.68.0:

    python -m fontTools.varLib.instancer "EBGaramond[wght].ttf" wght=500 --update-name-table -o EBGaramond-Medium.ttf
    python -m fontTools.varLib.instancer "EBGaramond[wght].ttf" wght=600 --update-name-table -o EBGaramond-SemiBold.ttf
    python -m fontTools.varLib.instancer "EBGaramond-Italic[wght].ttf" wght=400 --update-name-table -o EBGaramond-Italic.ttf
