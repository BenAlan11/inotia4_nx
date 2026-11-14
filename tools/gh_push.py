"""Push the local git index to GitHub through the REST API.

`git push` needs https://github.com/<owner>/<repo>.git, which the egress proxy on
this machine does not forward (CONNECT 502 / TLS reset). api.github.com *is*
reachable, so the same objects are created through the Git data API instead:
blobs -> trees -> commit -> ref. The result is a normal commit in the repo; a
clone of it is identical to what `git push` would have produced.

The push is INCREMENTAL: the remote branch's tree is listed first, and only
paths whose blob differs are uploaded. The new tree is created with
`base_tree` pointing at the old one, so untouched files are not re-sent.
That matters here -- a handful of snapshot NROs and source zips are ~9 MB and
~5.5 MB each, and re-uploading all of them over the proxy takes minutes.

Blob contents always come from the git object store (`git cat-file blob`),
never from the working tree, so what lands on the remote is exactly what the
local commit points at even when files have unstaged edits.

Usage:
    GH_TOKEN=... python tools/gh_push.py BenAlan11/inotia4_nx main
    GH_TOKEN=... python tools/gh_push.py BenAlan11/inotia4_nx main --full
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


def blob_bytes(sha):
    """Exact bytes of the git object -- not what the working tree holds now."""
    return subprocess.run(["git", "cat-file", "blob", sha],
                          capture_output=True, check=True).stdout


def make_blob(sha):
    raw = blob_bytes(sha)
    payload = {"content": base64.b64encode(raw).decode(), "encoding": "base64"}
    r = call("POST", "/repos/%s/git/blobs" % REPO, payload)
    return r["sha"], len(raw)


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


def remote_state(branch):
    """-> (commit_sha or None, root_tree_sha or None, {path: blob sha})"""
    try:
        head = call("GET", "/repos/%s/git/ref/heads/%s" % (REPO, branch))
    except SystemExit:
        return None, None, {}
    sha = head.get("object", {}).get("sha")
    if not sha:
        return None, None, {}
    commit = call("GET", "/repos/%s/git/commits/%s" % (REPO, sha))
    root = commit["tree"]["sha"]
    listing = call("GET", "/repos/%s/git/trees/%s?recursive=1" % (REPO, root))
    if listing.get("truncated"):
        print("warning: remote tree listing was truncated")
    blobs = {e["path"]: e["sha"] for e in listing.get("tree", [])
             if e["type"] == "blob"}
    return sha, root, blobs


def main():
    if len(sys.argv) < 3:
        sys.exit("usage: gh_push.py <owner/repo> <branch> [--full]")
    global REPO
    repo, branch = sys.argv[1], sys.argv[2]
    full = "--full" in sys.argv[3:]
    REPO = repo

    rows = index_entries()
    print("%d files in the index" % len(rows))

    parent, base_tree, remote_blobs = remote_state(branch)
    print("remote %s at %s" % (branch, parent or "(missing)"))

    known = {p: s for p, _m, s in rows}
    changed = [r for r in rows if remote_blobs.get(r[0]) != r[2]]
    removed = [p for p in remote_blobs if p not in known]
    if full or not base_tree:
        changed = list(rows)
        removed = []

    print("%d to upload, %d deleted, %d unchanged"
          % (len(changed), len(removed), len(rows) - len(changed)))

    entries, total = [], 0
    for i, (path, mode, sha) in enumerate(changed, 1):
        blob, size = make_blob(sha)
        total += size
        entries.append({"path": path, "mode": mode, "type": "blob", "sha": blob})
        print("  %2d/%d %-58s %8.1f KB" % (i, len(changed), path, size / 1024.0))
    for path in removed:
        entries.append({"path": path, "mode": "100644", "type": "blob",
                        "sha": None})

    if base_tree and not full:
        try:
            tree = call("POST", "/repos/%s/git/trees" % REPO,
                        {"base_tree": base_tree, "tree": entries})["sha"]
            print("tree %s (from base %s)" % (tree, base_tree[:8]))
        except SystemExit as e:
            print("incremental tree rejected (%s); falling back to a full tree" % e)
            base_tree = None
    if not base_tree:
        tree = build_tree([(p, m, s) for p, m, s in rows])
        print("tree %s (full)" % tree)

    msg = subprocess.run(["git", "log", "-1", "--pretty=%B"],
                         capture_output=True, text=True).stdout.strip()
    # keep the remote history: this commit continues whatever the branch points
    # at now (the first run found it missing and started an orphan)
    commit = call("POST", "/repos/%s/git/commits" % REPO,
                  {"message": msg, "tree": tree,
                   "parents": [parent] if parent else []})
    print("commit", commit["sha"], "(%.1f MB uploaded)" % (total / 1048576.0))

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
