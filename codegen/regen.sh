#!/usr/bin/env bash
# Regenerate the iLink3 SBE structs from the official schema.
# Run after dropping a new ilinkbinary.xml into Schema/ (e.g. a schema version bump).
# The generated header is checked in, so the C++ build never needs Python.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
python3 "$here/codegen/gen_ilink3.py" \
    "$here/Schema/ilinkbinary.xml" \
    "$here/ILink3/Generated/ILink3Sbe.hpp"
echo "Regenerated ILink3/Generated/ILink3Sbe.hpp -- rebuild to run the sizeof==blockLength asserts."
