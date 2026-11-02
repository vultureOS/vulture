#!/usr/bin/env bash
#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Networking from one end to the other, without needing the internet: DHCP and DNS from QEMU,
# and servers on this machine for HTTP, TLS (one for each thing there is to get right, or
# wrong) and a repository of signed packages. Tests/Network/network-client.sh runs in the
# system and says what it found.
#
#   Meta/network-test.sh [--build-dir DIR]

set -euo pipefail

source_dir="$(cd "$(dirname "$0")/.." && pwd)"
build_dir="$source_dir/build"
while [ $# -gt 0 ]; do
    case "$1" in
        --build-dir) build_dir="$2"; shift 2 ;;
        *) echo "usage: network-test.sh [--build-dir DIR]" >&2; exit 2 ;;
    esac
done

out="$build_dir/network-test"
rm -rf "$out"
mkdir -p "$out/www/repo" "$out/www/forged" "$out/www/old" "$out/certs"
log="$out/guest.log"
pids=()
cleanup() {
    for pid in ${pids[@]+"${pids[@]}"}; do
        kill "$pid" 2>/dev/null || true
    done
}
trap cleanup EXIT

# ---- Certificates: an authority, a chain from it, and each thing that should be refused.
cd "$out/certs"
quiet() { "$@" > /dev/null 2>&1; }
san="subjectAltName=IP:10.0.2.2,DNS:repo.vulture.test"
quiet openssl ecparam -name prime256v1 -genkey -noout -out ca.key
quiet openssl req -x509 -new -key ca.key -sha256 -subj "/O=Vulture Test/CN=Network Test CA" -days 3650 \
    -addext "basicConstraints=critical,CA:TRUE" -addext "keyUsage=critical,keyCertSign,cRLSign" -out ca.pem
make_certificate() { # name key-kind san days-back days-forward
    local name="$1" kind="$2" names="$3"
    case "$kind" in
        rsa) quiet openssl genrsa -out "$name.key" 2048 ;;
        ec) quiet openssl ecparam -name secp384r1 -genkey -noout -out "$name.key" ;;
    esac
    quiet openssl req -new -key "$name.key" -subj "/CN=$name" -out "$name.csr"
    printf 'basicConstraints=CA:FALSE\nkeyUsage=critical,digitalSignature\nextendedKeyUsage=serverAuth\n%s\n' "$names" > "$name.ext"
    quiet openssl x509 -req -in "$name.csr" -CA "${CA_PEM:-ca.pem}" -CAkey "${CA_KEY:-ca.key}" -CAcreateserial -sha256 ${VALIDITY:---days 365} -extfile "$name.ext" -out "$name.pem"
}
make_certificate good rsa "$san"
make_certificate ecdsa ec "$san"
make_certificate wrong rsa "subjectAltName=DNS:someone.else.example"
VALIDITY="-not_before 20200101000000Z -not_after 20210101000000Z" make_certificate expired rsa "$san"
quiet openssl ecparam -name prime256v1 -genkey -noout -out other-ca.key
quiet openssl req -x509 -new -key other-ca.key -sha256 -subj "/CN=Some Other CA" -days 3650 -addext "basicConstraints=critical,CA:TRUE" -out other-ca.pem
CA_PEM=other-ca.pem CA_KEY=other-ca.key make_certificate unknown rsa "$san"
cp ca.pem "$out/www/ca.pem"

# ---- A repository: two packages, one of which isn't what the index says.
vpkgsign="$build_dir/host/Tools/vpkgsign/vpkgsign"
"$vpkgsign" keygen "$out/repository.key" "$out/www/repository.pub"
payload="$build_dir/root/usr/tests/package-payload"
if [ ! -d "$payload/Applications/Hello.app" ]; then
    echo "network-test: $payload is missing; build first" >&2
    exit 1
fi
mkdir -p "$out/hello-root/Applications"
cp -R "$payload/Applications/Hello.app" "$out/hello-root/Applications/"
python3 -I "$source_dir/Meta/vpkg.py" build "$out/hello-root" -o "$out/www/repo/hello-1.0.vpkg" --name hello --version 1.0 --summary "A greeting, by way of the network"
mkdir -p "$out/tampered-root/Applications/Tampered.app/Contents"
echo tampered > "$out/tampered-root/Applications/Tampered.app/Contents/Info.plist"
python3 -I "$source_dir/Meta/vpkg.py" build "$out/tampered-root" -o "$out/www/repo/tampered-1.0.vpkg" --name tampered --version 1.0 --summary "Not what it seems"
python3 -I "$source_dir/Meta/vpkg.py" index "$out/www/repo" --key "$out/repository.key" --vpkgsign "$vpkgsign" --serial 100
# Swapped after signing.
printf 'x' | dd of="$out/www/repo/tampered-1.0.vpkg" bs=1 seek=100 conv=notrunc 2> /dev/null
# Signed by nobody: a copy with a change, and the old signature.
cp "$out/www/repo/index.json" "$out/www/repo/index.json.sig" "$out/www/forged/"
sed -i.bak 's/A greeting/A lie/' "$out/www/forged/index.json"
rm -f "$out/www/forged/index.json.bak"
# Signed, and older than what it replaces.
cp "$out/www/repo/hello-1.0.vpkg" "$out/www/old/"
python3 -I "$source_dir/Meta/vpkg.py" index "$out/www/old" --key "$out/repository.key" --vpkgsign "$vpkgsign" --serial 50

# ---- The servers.
cd "$out/www"
python3 -I "$source_dir/Tests/Network/server.py" "$out/www" 18081 18080 > "$out/server.log" 2>&1 &
pids+=($!)
serve_tls() { # port certificate extra-options...
    local port="$1" name="$2"
    shift 2
    # -WWW serves files, from here. The whole tree, so that /repo/ and /ok are there.
    openssl s_server -quiet -accept "$port" -cert "$out/certs/$name.pem" -key "$out/certs/$name.key" -WWW "$@" > "$out/tls-$port.log" 2>&1 &
    pids+=($!)
}
printf 'ok\n' > ok
serve_tls 18443 good -tls1_3 -ciphersuites TLS_AES_128_GCM_SHA256
serve_tls 18444 good -tls1_3 -ciphersuites TLS_AES_256_GCM_SHA384
serve_tls 18445 good -tls1_3 -ciphersuites TLS_CHACHA20_POLY1305_SHA256
serve_tls 18446 good -tls1_3 -groups P-256
serve_tls 18447 ecdsa -tls1_3
serve_tls 18448 wrong -tls1_3
serve_tls 18449 expired -tls1_3
serve_tls 18450 unknown -tls1_3
serve_tls 18451 good -tls1_2
for _ in $(seq 1 50); do
    grep -q ready "$out/server.log" && break
    sleep 0.1
done

cd "$source_dir"
status=0
Meta/run.sh --build-dir "$build_dir" --headless --boot-args nettest \
    --expect "net-test: PASS" --wait-for-exit --timeout 180 > "$log" 2>&1 || status=$?
grep -E "net-test: |run.sh: (PASS|FAIL|MISSING)" "$log" || true
if [ "$status" -ne 0 ]; then
    echo "network-test: FAIL; see $log" >&2
    exit 1
fi

# ---- And the desktop's way: Software, from a click on Get to the application running.
shots="$out/shots"
mkdir -p "$shots"
VULTURE_GPU_WIDTH=2560 VULTURE_GPU_HEIGHT=1600 VULTURE_SCREENSHOT_DEVICE=gpu0 VULTURE_SCREENSHOT_PREFIX="$shots/" \
    Meta/run.sh --build-dir "$build_dir" --headless --gui --fresh-disk --boot-args netguitest \
    --expect "repository: ready" --chat "$source_dir/Tests/Network/software.chat" --timeout 120 > "$out/software.log" 2>&1 || status=$?
if [ "$status" -ne 0 ]; then
    tail -3 "$out/software.log" >&2
    echo "network-test: Software: FAIL; see $out/software.log" >&2
    exit 1
fi
echo "network-test: PASS"
