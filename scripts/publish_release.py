#!/usr/bin/env python3
"""Publish a built artifact to a GitHub Release.

Called by the Jenkins pipeline on tag builds. Reads the token from GH_TOKEN.

Written in Python rather than curl+sed because the first version parsed GitHub's
JSON with grep and sed, failed to notice an already-uploaded asset, and then died
on "already_exists" when it tried to upload over it. Parsing JSON with a JSON
parser is the fix.

Idempotent on purpose: re-running a build for a tag that has already been
released is a normal thing to do (a retry, a re-scan, a Jenkins restart) and must
not fail. An existing release is reused; an existing asset of the same name is
deleted and replaced.
"""
import argparse
import json
import os
import sys
import urllib.error
import urllib.request

API = "https://api.github.com"
UPLOADS = "https://uploads.github.com"


def call(url, token, method="GET", body=None, content_type="application/json"):
    req = urllib.request.Request(url, method=method)
    req.add_header("Authorization", f"Bearer {token}")
    req.add_header("Accept", "application/vnd.github+json")
    if body is not None:
        req.add_header("Content-Type", content_type)
    try:
        with urllib.request.urlopen(req, body, timeout=120) as r:
            raw = r.read()
            return r.status, (json.loads(raw) if raw else {})
    except urllib.error.HTTPError as e:
        raw = e.read()
        try:
            return e.code, json.loads(raw)
        except Exception:
            return e.code, {"raw": raw[:400].decode("utf-8", "replace")}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", required=True, help="owner/name")
    ap.add_argument("--tag", required=True)
    ap.add_argument("--asset", required=True, action="append",
                    help="file to upload; repeat for several")
    args = ap.parse_args()

    token = os.environ.get("GH_TOKEN")
    if not token:
        sys.exit("GH_TOKEN is not set")
    for a in args.asset:
        if not os.path.isfile(a):
            sys.exit(f"asset not found: {a}")

    first = args.asset[0]
    size = os.path.getsize(first)

    # 1. Reuse the release for this tag if it already exists.
    status, rel = call(f"{API}/repos/{args.repo}/releases/tags/{args.tag}", token)
    if status == 200:
        release_id = rel["id"]
        print(f"  reusing existing release {args.tag} (id {release_id})")
    else:
        status, rel = call(
            f"{API}/repos/{args.repo}/releases",
            token,
            method="POST",
            body=json.dumps({
                "tag_name": args.tag,
                "name": args.tag,
                "body": f"Automated build from Jenkins.\n\n`{name}` — {size:,} bytes",
            }).encode(),
        )
        if status not in (200, 201):
            sys.exit(f"  creating release failed: HTTP {status} {rel}")
        release_id = rel["id"]
        print(f"  created release {args.tag} (id {release_id})")

    # 2 + 3. For each asset: replace any same-named one, then upload.
    status, assets = call(f"{API}/repos/{args.repo}/releases/{release_id}/assets", token)
    existing = {a["name"]: a["id"] for a in assets} if status == 200 else {}

    for path in args.asset:
        name = os.path.basename(path)
        if name in existing:
            print(f"  deleting existing asset {name} (id {existing[name]})")
            call(f"{API}/repos/{args.repo}/releases/assets/{existing[name]}", token, method="DELETE")

        with open(path, "rb") as fh:
            data = fh.read()
        status, out = call(
            f"{UPLOADS}/repos/{args.repo}/releases/{release_id}/assets?name={name}",
            token,
            method="POST",
            body=data,
            content_type="application/octet-stream",
        )
        if status != 201:
            sys.exit(f"  upload of {name} failed: HTTP {status} {out}")
        print(f"  published {name} ({len(data):,} bytes)")
        print(f"  {out.get('browser_download_url')}")


if __name__ == "__main__":
    main()
