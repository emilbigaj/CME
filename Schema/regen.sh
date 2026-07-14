#!/usr/bin/env bash
# Download the latest official CME SBE schemas, then regenerate the C++ structs:
#   iLink 3 (order entry) : sftpng.cmegroup.com /MSGW/<env>/Templates/ilinkbinary.xml   -> ILink3Sbe.hpp
#   MDP 3.0 (market data) : ftp.cmegroup.com  /SBEFix/<env>/Templates/templates_FixBinary.xml -> Mdp3Sbe.hpp
# CME refreshes schema files on the Sunday prior to market open.
# The generated headers are checked in, so the C++ build never needs Python or network.
#
# Usage: regen.sh [Production|NRCert|Cert]   (default Production)
set -euo pipefail
schemaDir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

env="${1:-Production}"

# CME's PUBLIC schema-distribution account (documented under "Schema and Network
# Configuration Distribution" on the CME wiki) — not a per-firm secret.
ilinkUser='cmeconfig:G3t(0nnect3d'

# Extract the schema version from the <messageSchema> tag ([^a-zA-Z] so semanticVersion doesn't match).
extractVersion() { grep -o '<[^>]*messageSchema[^>]*>' "$1" | sed -nE 's/.*[^a-zA-Z]version="([0-9]+)".*/\1/p' | head -1; }

# fetch <url> <auth|-> <marker> <dest> : download, sanity-check, install if changed.
fetch()
{
    local url="$1" auth="$2" marker="$3" dest="$4"
    local tmp
    tmp="$(mktemp /tmp/cme_schema.XXXXXX.xml)"

    echo "Downloading $url ..."
    if [[ "$auth" == "-" ]]; then
        curl -sS --fail --max-time 120 "$url" -o "$tmp"
    else
        curl -sS --fail --max-time 120 -k -u "$auth" "$url" -o "$tmp"
    fi

    [[ $(stat -c%s "$tmp") -gt 50000 ]] || { echo "ERROR: $url suspiciously small"; rm -f "$tmp"; exit 1; }
    xmllint --noout "$tmp" || { echo "ERROR: $url is not well-formed XML"; rm -f "$tmp"; exit 1; }
    grep -q "$marker" "$tmp" || { echo "ERROR: $url does not match '$marker'"; rm -f "$tmp"; exit 1; }

    local version
    version="$(extractVersion "$tmp")"
    [[ -n "$version" ]] || { echo "ERROR: could not extract schema version from $url"; rm -f "$tmp"; exit 1; }

    if [[ -f "$dest" ]] && cmp -s "$tmp" "$dest"; then
        echo "  unchanged: $(basename "$dest") (version $version, sha256 $(sha256sum "$dest" | cut -d' ' -f1))"
    else
        [[ -f "$dest" ]] && echo "  SCHEMA CHANGED: version $(extractVersion "$dest") -> $version. Review the regen diff carefully."
        cp "$tmp" "$dest"
        echo "  updated: $(basename "$dest") (version $version, sha256 $(sha256sum "$dest" | cut -d' ' -f1))"
    fi
    rm -f "$tmp"
}

fetch "sftp://sftpng.cmegroup.com/MSGW/$env/Templates/ilinkbinary.xml" "$ilinkUser" \
      'package="iLinkBinary" id="8"' "$schemaDir/ilinkbinary.xml"

fetch "ftp://ftp.cmegroup.com/SBEFix/$env/Templates/templates_FixBinary.xml" "-" \
      'package="mktdata" id="1"' "$schemaDir/templates_FixBinary.xml"

python3 "$schemaDir/gen_sbe.py" ilink3 "$schemaDir/ilinkbinary.xml"          "$schemaDir/ILink3Sbe.hpp"
python3 "$schemaDir/gen_sbe.py" mdp3   "$schemaDir/templates_FixBinary.xml" "$schemaDir/Mdp3Sbe.hpp"

echo "Regenerated Schema/ILink3Sbe.hpp + Schema/Mdp3Sbe.hpp -- rebuild to run the sizeof==blockLength asserts."
