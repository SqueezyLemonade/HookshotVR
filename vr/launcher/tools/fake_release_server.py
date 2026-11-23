"""A stand-in for GitHub's release servers, for the launcher's self-test (tools/selftest.cpp): serves a folder on 127.0.0.1
the way the launcher's update reads GitHub when HOOKSHOTVR_DEV_RELEASES=http://127.0.0.1:<port> (development only):

    <folder>/api/repos/<owner>/<repo>/releases                      api.github.com's releases list (the query is ignored)
    <folder>/site/<owner>/<repo>/releases/download/<tag>/<file>     github.com's release files: answered with a redirect to
                                                                    /objects/..., as GitHub sends them to its file store
    <folder>/raw/<owner>/<repo>/<tag>/<path>                        raw.githubusercontent.com

A file <folder>/.ratelimited makes every /api/ request GitHub's rate-limit answer (403, X-RateLimit-Remaining: 0).

    python3 -I fake_release_server.py <folder>      prints "port <n>", then serves until killed
"""
import http.server
import os
import sys
import threading
import time

ROOT = os.path.abspath(sys.argv[1])


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def send(self, code, body, kind="application/octet-stream", extra=()):
        self.send_response(code)
        self.send_header("Content-Type", kind)
        self.send_header("Content-Length", str(len(body)))
        for k, v in extra:
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = self.path.split("?", 1)[0]
        if path.startswith("/api/") and os.path.exists(os.path.join(ROOT, ".ratelimited")):
            return self.send(403, b'{"message": "API rate limit exceeded for 127.0.0.1."}', "application/json",
                             (("X-RateLimit-Limit", "60"), ("X-RateLimit-Remaining", "0")))
        rel = os.path.normpath(path.lstrip("/"))
        if rel.startswith("..") or os.path.isabs(rel):
            return self.send(400, b"")
        if rel.startswith("site" + os.sep) and "%s%sdownload%s" % (os.sep + "releases", os.sep, os.sep) in rel:
            if os.path.isfile(os.path.join(ROOT, rel)):
                return self.send(302, b"", extra=(("Location", "/objects/" + rel[len("site/"):].replace(os.sep, "/")),))
        elif rel.startswith("objects" + os.sep):
            rel = "site" + rel[len("objects"):]
        file = os.path.join(ROOT, rel)
        if not os.path.isfile(file):
            return self.send(404, b'{"message": "Not Found"}', "application/json")
        with open(file, "rb") as f:
            self.send(200, f.read())


def orphaned():   # (the self-test ended without stopping it: so does the server)
    parent = os.getppid()
    while os.getppid() == parent:
        time.sleep(0.5)
    os._exit(0)


server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
print("port %d" % server.server_address[1], flush=True)
threading.Thread(target=orphaned, daemon=True).start()
server.serve_forever()
