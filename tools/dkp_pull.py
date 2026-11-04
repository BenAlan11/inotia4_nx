#!/usr/bin/env python3
"""Pull a Docker image through a registry mirror and extract only the paths we
need (devkitPro toolchain + portlibs) without Docker being installed.

usage: python dkp_pull.py <repo> <tag> <dest> [path-prefix ...]
"""
import json
import os
import shutil
import subprocess
import sys
import tarfile
import time
import urllib.error
import urllib.request

HOST = os.environ.get('DKP_MIRROR', 'docker.1ms.run')
BLOB_HOST = os.environ.get('DKP_BLOB_MIRROR', HOST)
REPO = sys.argv[1] if len(sys.argv) > 1 else 'devkitpro/devkita64'
TAG = sys.argv[2] if len(sys.argv) > 2 else 'latest'
DEST = sys.argv[3] if len(sys.argv) > 3 else 'dkp_rootfs'
PREFIXES = sys.argv[4:] or ['opt/devkitpro']

ACCEPT = ','.join([
    'application/vnd.docker.distribution.manifest.list.v2+json',
    'application/vnd.oci.image.index.v1+json',
    'application/vnd.docker.distribution.manifest.v2+json',
    'application/vnd.oci.image.manifest.v1+json',
])


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    """A registry blob URL 307s to a CDN that rejects the bearer token, so we
    resolve the redirect by hand and fetch the final URL unauthenticated."""
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


_OPENER = urllib.request.build_opener(_NoRedirect)


def open_url(url, headers=None, tries=8):
    last = None
    for i in range(tries):
        req = urllib.request.Request(url, headers=headers or {})
        try:
            return _OPENER.open(req, timeout=180)
        except urllib.error.HTTPError as e:
            if e.code in (301, 302, 303, 307, 308):
                loc = e.headers.get('Location')
                if loc:
                    return urllib.request.urlopen(loc, timeout=180)
            last = e
            print('   retry %d: %s' % (i + 1, e))
            time.sleep(2 + i)
        except Exception as e:  # mirror nodes fail at random
            last = e
            print('   retry %d: %s' % (i + 1, e))
            time.sleep(2 + i)
    raise SystemExit('give up on %s: %s' % (url, last))


def get_json(url, headers=None):
    with open_url(url, headers) as r:
        return json.loads(r.read().decode())


def download(url, headers, out, expect=0):
    # curl drops Authorization on the cross-host redirect by itself, which is
    # exactly what the CDN in front of the mirror needs. The links are flaky and
    # throttle after a few MB, so keep resuming until the size matches.
    for attempt in range(400):
        have = os.path.getsize(out) if os.path.exists(out) else 0
        if expect and have >= expect:
            return have
        cmd = ['curl', '-sSL', '-C', '-', '--retry', '3', '--retry-delay', '1',
               '-m', '300', '-o', out]
        for k, v in (headers or {}).items():
            if k != 'Accept':
                cmd += ['-H', '%s: %s' % (k, v)]
        cmd.append(url)
        subprocess.run(cmd)
        now = os.path.getsize(out) if os.path.exists(out) else 0
        if expect and now > expect:
            # the node ignored the Range header and resent the whole blob
            with open(out, 'r+b') as f:
                f.truncate(expect)
            return expect
        if expect and now == expect:
            return now
        if now == have:
            # no progress this round: bounce the connection and try again
            time.sleep(1 + attempt % 5)
            if attempt > 12 and now == 0:
                raise SystemExit('download stuck: %s' % url)
        if attempt % 10 == 9:
            print('    resumed to %.1f MB' % (now / 1048576))
    raise SystemExit('download incomplete: %s' % url)


def main():
    auth = {'Accept': ACCEPT}
    try:
        tok = get_json('https://%s/openapi/v1/auth/token?service=%s&scope=repository:%s:pull'
                       % (HOST, HOST, REPO))['access_token']
        auth['Authorization'] = 'Bearer %s' % tok
    except BaseException as e:
        print('anonymous registry access (%s)' % e)

    mf = get_json('https://%s/v2/%s/manifests/%s' % (HOST, REPO, TAG), auth)
    if 'manifests' in mf:
        cand = [m for m in mf['manifests']
                if m.get('platform', {}).get('architecture') == 'amd64']
        digest = (cand or mf['manifests'])[0]['digest']
        print('platform manifest', digest)
        mf = get_json('https://%s/v2/%s/manifests/%s' % (HOST, REPO, digest), auth)

    layers = mf.get('layers', [])
    print('%d layers, %.1f MB compressed' % (len(layers),
          sum(l.get('size', 0) for l in layers) / 1048576))
    os.makedirs(DEST, exist_ok=True)

    for i, l in enumerate(layers):
        dg, size = l['digest'], l.get('size', 0)
        fn = '_layer_%02d.tar.gz' % i
        if os.path.exists(fn) and abs(os.path.getsize(fn) - size) < size * 0.02:
            print('[%d/%d] cached %s' % (i + 1, len(layers), dg[:19]))
        else:
            print('[%d/%d] %s  %.1f MB' % (i + 1, len(layers), dg[:19], size / 1048576))
            bh = dict(auth)
            if BLOB_HOST != HOST:
                bh.pop('Authorization', None)   # blob CDN is anonymous
            download('https://%s/v2/%s/blobs/%s' % (BLOB_HOST, REPO, dg), bh, fn, size)
        extract(fn, DEST)
        os.remove(fn)
    print('done ->', DEST)


def extract(fn, dest):
    try:
        t = tarfile.open(fn)
    except Exception as e:
        print('   not a tar, skipped:', e)
        return
    n = 0
    for m in t:
        name = m.name.lstrip('./')
        if not any(name == p or name.startswith(p + '/') for p in PREFIXES):
            continue
        target = os.path.join(dest, name)
        if m.isdir():
            os.makedirs(target, exist_ok=True)
            continue
        if m.issym():
            if not os.path.lexists(target):
                try:
                    os.symlink(m.linkname, target)
                except OSError:
                    pass
            continue
        if not m.isfile():
            continue
        os.makedirs(os.path.dirname(target), exist_ok=True)
        src = t.extractfile(m)
        if src is None:
            continue
        with open(target, 'wb') as f:
            shutil.copyfileobj(src, f, 1 << 20)
        n += 1
    t.close()
    print('   extracted %d files' % n)


if __name__ == '__main__':
    main()
