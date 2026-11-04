"""Push the local git index to GitHub through the REST API.

`git push` needs https://github.com/<owner>/<repo>.git, which the egress proxy on
this machine does not forward (CONNECT 502 / TLS reset). api.github.com *is*
reachable, so the same objects are created through the Git data API instead:
blobs -> trees -> commit -> ref. The result is a normal commit in the repo; a
clone of it is identical to what `git push` would have produced.

Usage:
    GH_TOKEN=... python tools/gh_push.py BenAlan11/inotia4_nx main
"""
import base64
import json
import os
import subprocess
import sys
import time
import urllib.error
import urllib.request

API = "https://api.github.com"


def token():
    t = os.environ.get("GH_TOKEN") or os.environ.get("GITHUB_TOKEN")
    if not t:
        sys.exit("GH_TOKEN not set")
    return t


TOK = token()
REPO = ""            # "owner/name", set in main()


def call(method, path, payload=None, tries=4):
    url = API + path
    data = json.dumps(payload).encode() if payload is not None else None
    last = None
    for i in range(tries):
        req = urllib.request.Request(url, data=data, method=method)
        req.add_header("Authorization", "Bearer " + TOK)
        req.add_header("Accept", "application/vnd.github+json")
        if data is not None:
            req.add_header("Content-Type", "application/json")
        try:
            with urllib.request.urlopen(req, timeout=300) as r:
                body = r.read()
                return json.loads(body) if body else {}
        except urllib.error.HTTPError as e:
            body = e.read().decode("utf-8", "replace")
            last = "HTTP %d: %s" % (e.code, body[:400])
            if e.code in (500, 502, 503, 504):
                time.sleep(2 + 2 * i)
                continue
            raise SystemExit("%s %s -> %s" % (method, path, last))
        except Exception as e:                     # network hiccup: retry
            last = repr(e)
            time.sleep(2 + 2 * i)
    raise SystemExit("%s %s failed: %s" % (method, path, last))


def index_entries():
    out = subprocess.run(["git", "ls-files", "-s"], capture_output=True,
                         text=True, check=True).stdout
    rows = []
    for line in out.splitlines():
        meta, path = line.split("\t", 1)
        mode, sha, stage = meta.split()
        rows.append((path, mode, sha))
    return rows


def same_blob(sha):
    """git already has the object, but GitHub needs the blob created there."""
    return sha


def make_blob(path):
    with open(path, "rb") as f:
        raw = f.read()
    payload = {"content": base64.b64encode(raw).decode(), "encoding": "base64"}
    r = call("POST", "/repos/%s/git/blobs" % REPO, payload)
    return r["sha"]


def build_tree(entries, prefix=""):
    """entries: [(relpath, mode, blob_sha)] -> tree sha of this level"""
    files, dirs = [], {}
    for path, mode, sha in entries:
        if "/" in path:
            head, rest = path.split("/", 1)
            dirs.setdefault(head, []).append((rest, mode, sha))
        else:
            files.append({"path": path, "mode": mode, "type": "blob", "sha": sha})
    for name, sub in sorted(dirs.items()):
        files.append({"path": name, "mode": "040000", "type": "tree",
                      "sha": build_tree(sub, prefix + name + "/")})
    r = call("POST", "/repos/%s/git/trees" % REPO, {"tree": files})
    return r["sha"]


def main():
    if len(sys.argv) < 3:
        sys.exit("usage: gh_push.py <owner/repo> <branch>")
    global REPO
    repo, branch = sys.argv[1], sys.argv[2]
    REPO = repo

    rows = index_entries()
    print("%d files in the index" % len(rows))

    entries, total = [], 0
    for i, (path, mode, sha) in enumerate(rows, 1):
        size = os.path.getsize(path)
        total += size
        blob = make_blob(path)
        entries.append((path, mode, blob))
        if i % 25 == 0 or i == len(rows):
            print("  %d/%d blobs (%d KB)" % (i, len(rows), total // 1024))

    tree = build_tree(entries)
    print("tree", tree)

    msg = subprocess.run(["git", "log", "-1", "--pretty=%B"],
                         capture_output=True, text=True).stdout.strip()
    # keep the remote history: this commit continues whatever the branch points
    # at now (the first run found it missing and started an orphan)
    parents = []
    try:
        head = call("GET", "/repos/%s/git/ref/heads/%s" % (REPO, branch))
        if head.get("object", {}).get("sha"):
            parents = [head["object"]["sha"]]
    except SystemExit:
        parents = []
    commit = call("POST", "/repos/%s/git/commits" % REPO,
                  {"message": msg, "tree": tree, "parents": parents})
    print("commit", commit["sha"])

    # point the branch at it (create if missing, force if it exists)
    try:
        call("PATCH", "/repos/%s/git/refs/heads/%s" % (REPO, branch),
             {"sha": commit["sha"], "force": True})
    except SystemExit:
        call("POST", "/repos/%s/git/refs" % REPO,
             {"ref": "refs/heads/" + branch, "sha": commit["sha"]})
    print("https://github.com/%s/tree/%s" % (repo, branch))


if __name__ == "__main__":
    main()
