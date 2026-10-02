#!/bin/sh
# Builds the static Alpine nshtestherd image and extracts the compiled
# binary to disk -- see Dockerfile and docker/compile_alpine_static.sh
# for what the build itself does.
#
# Usage:
#   ./build.sh                       # build image, extract binary to ./nshtestherd
#   ./build.sh ./out/nshtestherd     # ...to a specific path instead
#
# IMAGE_TAG=myregistry/nshtestherd:1.0 ./build.sh   # override the image tag
set -eu

IMAGE_TAG="${IMAGE_TAG:-nshtestherd:latest}"
SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
OUT="${1:-$SCRIPT_DIR/nshtestherd}"

docker build --no-cache --progress=plain -t "$IMAGE_TAG" "$SCRIPT_DIR"
echo "built image: $IMAGE_TAG"

CID="$(docker create "$IMAGE_TAG")"
docker cp "$CID:/nshtestherd" "$OUT"
docker rm "$CID" >/dev/null
echo "extracted binary to $OUT"
