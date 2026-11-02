#!/usr/bin/env python3
#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Makes packages and signs repositories of them, on the build machine.
#
#   vpkg.py build ROOT -o OUT.vpkg --name NAME --version VERSION [--summary TEXT]
#       ROOT is laid out as the system is: ROOT/Applications/Hello.app/...
#   vpkg.py index DIRECTORY --key KEYFILE --vpkgsign PATH [--serial N]
#       For every *.vpkg in DIRECTORY: index.json, and index.json.sig signed with the key.

import argparse, gzip, hashlib, io, json, os, subprocess, sys, tarfile, time


def build(args):
    if not os.path.isdir(args.root):
        sys.exit(f"vpkg: {args.root} is not a directory")
    manifest = json.dumps({"name": args.name, "version": args.version, "summary": args.summary or ""}, indent=2).encode()
    raw = io.BytesIO()
    with tarfile.open(fileobj=raw, mode="w", format=tarfile.USTAR_FORMAT) as tar:
        info = tarfile.TarInfo("Manifest.json")
        info.size = len(manifest)
        info.mode = 0o644
        tar.addfile(info, io.BytesIO(manifest))
        for directory, directories, files in sorted(os.walk(args.root)):
            directories.sort()
            for name in sorted(files):
                path = os.path.join(directory, name)
                relative = os.path.relpath(path, args.root)
                member = tarfile.TarInfo("Payload/" + relative)
                if os.path.islink(path):
                    member.type = tarfile.SYMTYPE
                    member.linkname = os.readlink(path)
                    member.mode = 0o755
                    tar.addfile(member)
                else:
                    member.size = os.path.getsize(path)
                    member.mode = os.stat(path).st_mode & 0o755
                    with open(path, "rb") as source:
                        tar.addfile(member, source)
    with open(args.output, "wb") as out:
        out.write(gzip.compress(raw.getvalue(), mtime=0))
    print(f"vpkg: {args.output}: {os.path.getsize(args.output)} bytes")


def index(args):
    packages = []
    for name in sorted(os.listdir(args.directory)):
        if not name.endswith(".vpkg"):
            continue
        path = os.path.join(args.directory, name)
        with open(path, "rb") as f:
            data = f.read()
        with tarfile.open(fileobj=io.BytesIO(gzip.decompress(data))) as tar:
            manifest = json.load(tar.extractfile("Manifest.json"))
        packages.append({
            "name": manifest["name"],
            "version": manifest["version"],
            "summary": manifest.get("summary", ""),
            "file": name,
            "size": len(data),
            "sha256": hashlib.sha256(data).hexdigest(),
        })
    serial = args.serial if args.serial is not None else int(time.time())
    index_path = os.path.join(args.directory, "index.json")
    with open(index_path, "w") as f:
        json.dump({"serial": serial, "packages": packages}, f, indent=2)
    subprocess.run([args.vpkgsign, "sign", args.key, index_path, index_path + ".sig"], check=True)
    print(f"vpkg: {index_path}: {len(packages)} package(s), serial {serial}")


def main():
    parser = argparse.ArgumentParser()
    commands = parser.add_subparsers(dest="command", required=True)
    b = commands.add_parser("build")
    b.add_argument("root")
    b.add_argument("-o", "--output", required=True)
    b.add_argument("--name", required=True)
    b.add_argument("--version", required=True)
    b.add_argument("--summary")
    b.set_defaults(run=build)
    i = commands.add_parser("index")
    i.add_argument("directory")
    i.add_argument("--key", required=True)
    i.add_argument("--vpkgsign", required=True)
    i.add_argument("--serial", type=int)
    i.set_defaults(run=index)
    args = parser.parse_args()
    args.run(args)


if __name__ == "__main__":
    main()
