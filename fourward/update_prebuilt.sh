#!/bin/bash
# Rebuilds pre-built 4ward artifacts from a 4ward checkout.
#
# Usage:
#   ./fourward/update_prebuilt.sh /path/to/4ward
#
# Produces:
#   fourward/prebuilt/p4runtime_server.jar   — self-contained server JAR
#   fourward/prebuilt/p4c-4ward              — p4c backend plugin (C++ binary)
#   fourward/prebuilt/COMMIT                 — source commit + timestamp

set -euo pipefail

FOURWARD_DIR="${1:?Usage: $0 <path-to-4ward-repo>}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PREBUILT_DIR="$SCRIPT_DIR/prebuilt"

mkdir -p "$PREBUILT_DIR"

echo "Building 4ward artifacts from: $FOURWARD_DIR"
cd "$FOURWARD_DIR"

# Record the source commit.
COMMIT="$(git rev-parse HEAD)"
echo "$COMMIT $(date -u +%Y-%m-%dT%H:%M:%SZ)" > "$PREBUILT_DIR/COMMIT"
echo "Source commit: $COMMIT"

# Build the P4Runtime server deploy jar.
echo "Building p4runtime_server deploy jar..."
bazel build //p4runtime:p4runtime_server_deploy_deploy.jar
cp -f bazel-bin/p4runtime/p4runtime_server_deploy_deploy.jar \
      "$PREBUILT_DIR/p4runtime_server.jar"
echo "  -> $(du -h "$PREBUILT_DIR/p4runtime_server.jar" | cut -f1)"

# Build the p4c backend plugin.
echo "Building p4c-4ward..."
bazel build //p4c_backend:p4c-4ward
cp -f bazel-bin/p4c_backend/p4c-4ward "$PREBUILT_DIR/p4c-4ward"
echo "  -> $(du -h "$PREBUILT_DIR/p4c-4ward" | cut -f1)"

echo ""
echo "Updated fourward/prebuilt/ from $COMMIT"
echo "Don't forget to commit the changes."
