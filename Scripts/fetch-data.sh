#!/usr/bin/env bash
# Refresh one environment's staged CME data files in /mnt/S/CME/<Environment>/Config:
#   secdef.dat  : the instrument universe (price bands and reference prices change DAILY
#                 after settlement — a stale file misprices band-anchored orders)
#   config.xml  : the market-data channel map (multicast groups and ports; changes rarely)
#
# Downloads are skipped when CME's copy has not changed: the compressed file is kept beside
# the extracted one, stamped with CME's own modification time, and curl's time-conditional
# transfer (-z) asks the server for the file only when its clock is newer — a header
# round trip instead of a 50MB pull. A real download is validated in a scratch directory
# and moved into place with atomic renames, so a running server never sees a partial file.
# CME regenerates secdef daily around 15:50 Chicago; run this in the 16:00-17:00
# maintenance window, before the server (re)starts.
#
# Usage: fetch-data.sh [NewRelease|Cert|Production]   (default NewRelease)

set -euo pipefail

environment="${1:-NewRelease}"

# Step 1: Map our environment names onto CME's FTP directory names.
case "$environment" in
	NewRelease) ftpEnv="NRCert" ;;
	Cert)       ftpEnv="Cert" ;;
	Production) ftpEnv="Production" ;;
	*) echo "usage: fetch-data.sh [NewRelease|Cert|Production]"; exit 2 ;;
esac
configDir="/mnt/S/CME/$environment/Config"
mkdir -p "$configDir"

# Fetch a url only if CME's copy is newer than our reference file. Prints the outcome;
# leaves the download (stamped with CME's modification time) at the given path.
# fetchIfNewer <url> <referenceFile> <downloadTo> -> returns 0 if a new file was downloaded
fetchIfNewer()
{
	local url="$1" reference="$2" downloadTo="$3"
	local timeCondition=()
	[[ -f "$reference" ]] && timeCondition=(-z "$reference")

	curl -sS --fail --max-time 600 -R "${timeCondition[@]}" "$url" -o "$downloadTo"
	if [[ ! -s "$downloadTo" ]]; then
		rm -f "$downloadTo"
		echo "  unchanged on CME's side: $(basename "$reference") (theirs: $(curl -sI "$url" | grep -i Last-Modified | cut -d' ' -f2-))"
		return 1
	fi
	return 0
}

scratch="$(mktemp -d /tmp/cme_fetch.XXXXXX)"
trap 'rm -rf "$scratch"' EXIT

# Step 2: secdef — the compressed file is the change reference; extract and sanity-check
# in scratch, then rename the pair into place.
echo "Checking secdef ($ftpEnv)..."
if fetchIfNewer "ftp://ftp.cmegroup.com/SBEFix/$ftpEnv/secdef.dat.gz" "$configDir/secdef.dat.gz" "$scratch/secdef.dat.gz"; then
	gunzip -k "$scratch/secdef.dat.gz"
	instruments=$(grep -c $'^35=d\x01' "$scratch/secdef.dat" || true)
	[[ "$instruments" -gt 10000 ]] || { echo "ERROR: secdef.dat looks wrong ($instruments definition lines)"; exit 1; }
	mv "$scratch/secdef.dat" "$configDir/secdef.dat"
	mv "$scratch/secdef.dat.gz" "$configDir/secdef.dat.gz"
	echo "  updated: secdef.dat ($instruments instruments, sha256 $(sha256sum "$configDir/secdef.dat.gz" | cut -c1-12)...)"
fi

# Step 3: config.xml — small, same mechanism for symmetry.
echo "Checking channel config ($ftpEnv)..."
if fetchIfNewer "ftp://ftp.cmegroup.com/SBEFix/$ftpEnv/Configuration/config.xml" "$configDir/config.xml" "$scratch/config.xml"; then
	xmllint --noout "$scratch/config.xml"
	grep -q "<channel" "$scratch/config.xml" || { echo "ERROR: config.xml has no channels"; exit 1; }
	mv "$scratch/config.xml" "$configDir/config.xml"
	echo "  updated: config.xml (sha256 $(sha256sum "$configDir/config.xml" | cut -c1-12)...)"
fi

echo "Done: $configDir"
