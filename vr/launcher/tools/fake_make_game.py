"""make_game.py's @@ lines without building anything: for trying the launcher's build screens.

    hookshotvr-launcher --fake-build vr/launcher/tools/fake_make_game.py
    FAKE_FAIL=1: stops in the compile with an @@error line
    FAKE_GAME=1: leaves a stand-in game in --out (romfs/ and a hookshot that prints its environment), for the self-test's update
    FAKE_ARGS=<file>: its arguments written there, one a line, then TMPDIR=<its TMPDIR> (the self-test's check of the check)
"""
import os
import sys
import time

args = sys.argv[1:]
if os.environ.get("FAKE_ARGS"):
    with open(os.environ["FAKE_ARGS"], "w", encoding="utf-8") as f:
        f.write("".join(a + "\n" for a in args) + "TMPDIR=%s\n" % os.environ.get("TMPDIR", ""))
if "--check" in args:
    print("@@check ok Ocarina of Time 1.1.1", flush=True)
    sys.exit(0)
out = args[args.index("--out") + 1] if "--out" in args else args[args.index("--update") + 1]
speed = float(os.environ.get("FAKE_SPEED", "1"))
for stage, seconds in (("read", 2), ("lift", 4), ("compile", 6), ("finish", 1), ("shaders", 3)):
    print("@@stage %s %s" % (stage, stage), flush=True)
    steps = 20
    for i in range(steps + 1):
        print("@@progress %.3f" % (i / steps), flush=True)
        if stage == "compile":
            print("@@eta %.0f" % ((steps - i) * seconds / steps * 60), flush=True)
            if os.environ.get("FAKE_FAIL") and i == steps // 2:
                print("@@error The compiler stopped.", flush=True)
                sys.exit(1)
        time.sleep(seconds / steps / speed)
print("@@times 16 6 -1", flush=True)
if os.environ.get("FAKE_GAME"):
    os.makedirs(os.path.join(out, "romfs"), exist_ok=True)
    with open(os.path.join(out, "hookshot"), "w") as f:
        f.write("#!/bin/sh\necho cwd=$(pwd)\nenv | sort\n")
    os.chmod(os.path.join(out, "hookshot"), 0o755)
print("@@done %s" % out, flush=True)
