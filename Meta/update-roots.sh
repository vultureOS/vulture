#!/usr/bin/env bash
#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Refreshes Base/etc/ssl/cert.pem, the certificate authorities the system trusts, from a
# bundle of them (by default this machine's), leaving out any that have expired. These are
# public documents the authorities themselves publish: data, not code.
#
#   Meta/update-roots.sh [bundle.pem]

set -euo pipefail

source_dir="$(cd "$(dirname "$0")/.." && pwd)"
bundle="${1:-/etc/ssl/cert.pem}"
out="$source_dir/Base/etc/ssl/cert.pem"
now="$(date -u +%s)"

python3 -I - "$bundle" "$out" "$now" <<'PYTHON'
import re, subprocess, sys
bundle, out, now = sys.argv[1], sys.argv[2], int(sys.argv[3])
text = open(bundle).read()
blocks = re.findall(r"-----BEGIN CERTIFICATE-----.*?-----END CERTIFICATE-----", text, re.S)
kept = 0
with open(out, "w") as f:
    f.write("# The certificate authorities vultureOS trusts. See Meta/update-roots.sh.\n\n")
    for block in blocks:
        details = subprocess.run(["openssl", "x509", "-noout", "-subject", "-enddate", "-dateopt", "iso_8601", "-nameopt", "oneline"],
                                 input=block.encode(), capture_output=True).stdout.decode()
        subject = re.search(r"subject=(.*)", details).group(1).strip()
        end = re.search(r"notAfter=(.*)", details).group(1).strip()
        year = int(end[:4])
        if year < 2027:
            continue
        f.write(f"# {subject}\n{block}\n\n")
        kept += 1
print(f"update-roots: {kept} of {len(blocks)} kept in {out}")
PYTHON
