#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

IMAGES=../../../images
OUT_ROOTFS_TAR="$IMAGES"/alpine-rootfs.tar
OUT_ROOTFS_FLAT="$IMAGES"/alpine-rootfs-flat
OUT_FSJSON="$IMAGES"/alpine-fs.json
CONTAINER_NAME=alpine-v86
IMAGE_NAME=i386/alpine-v86

echo "* create image ..."
mkdir -p "$IMAGES"
docker build . --platform linux/386 --rm --tag "$IMAGE_NAME"
docker rm "$CONTAINER_NAME" || true
docker create --platform linux/386 -t -i --name "$CONTAINER_NAME" "$IMAGE_NAME"

echo "* dump driver names ..."
docker run --rm  --platform linux/386 i386/alpine-v86 sh -c "cupsd && sleep 1 && lpinfo -m" > lpinfo-models.txt

echo "* export files ..."
docker export "$CONTAINER_NAME" | ./tarfilter.py > "$OUT_ROOTFS_TAR" # -o "$OUT_ROOTFS_TAR"

#echo "* fix tar ..."
# https://github.com/iximiuz/docker-to-linux/issues/19#issuecomment-1242809707
#gtar --delete -f "$OUT_ROOTFS_TAR" .dockerenv etc/hosts
#gtar -Af "$OUT_ROOTFS_TAR" etc/hosts

echo "* JSONify ..."
../../../tools/fs2json.py --zstd --out "$OUT_FSJSON" "$OUT_ROOTFS_TAR"

# Note: Not deleting old files here
mkdir -p "$OUT_ROOTFS_FLAT"
../../../tools/copy-to-sha256.py --zstd "$OUT_ROOTFS_TAR" "$OUT_ROOTFS_FLAT"

echo "$OUT_ROOTFS_TAR", "$OUT_ROOTFS_FLAT" and "$OUT_FSJSON" created.
