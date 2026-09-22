#!/usr/bin/env bash
# Public-source build from this checkout. Does not use saved/local base images.
set -Eeuo pipefail
script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_dir="$(cd -- "$script_dir/.." && pwd)"
wolf_image="${WOLF_OUTPUT_IMAGE:-wolf:nvcodec-hdr}"
kde_image="${WOLF_KDE_IMAGE:-wolf-kde:hdr}"
verify_gpu=false
deploy=false
extra=()
for argument in "$@"; do
  case "$argument" in
    --no-cache) extra+=(--no-cache) ;;
    --verify-gpu) verify_gpu=true ;;
    --deploy) deploy=true ;;
    -h|--help)
      echo 'Usage: bash scripts/build-hdr-nvcodec.sh [--no-cache] [--verify-gpu] [--deploy]'
      echo 'Builds Wolf and KDE HDR from this repository and public dependencies.'
      echo 'Optional: WOLF_BUILDER, BUILD_JOBS, WOLF_OUTPUT_IMAGE, WOLF_KDE_IMAGE.'
      echo '--deploy requires an explicit WOLF_COMPOSE_FILE; no deployment by default.'
      exit 0 ;;
    *) echo "Unknown argument: $argument" >&2; exit 2 ;;
  esac
done
if "$deploy"; then
  test -f "${WOLF_COMPOSE_FILE:?Set WOLF_COMPOSE_FILE explicitly before --deploy}"
fi
if [ -n "${WOLF_BUILDER:-}" ]; then extra+=(--builder "$WOLF_BUILDER"); fi
# Each Dockerfile is a complete graph rooted only in digest-pinned public images.
docker buildx build --load --pull --progress=plain "${extra[@]}" \
  --build-arg "BUILD_JOBS=${BUILD_JOBS:-4}" \
  -f "$repo_dir/docker/wolf.nvcodec-runtime.Dockerfile" -t "$wolf_image" "$repo_dir"
docker buildx build --load --pull --progress=plain "${extra[@]}" \
  --build-arg "BUILD_JOBS=${BUILD_JOBS:-4}" \
  -f "$repo_dir/docker/kde-hdr.Dockerfile" -t "$kde_image" "$repo_dir"
if "$verify_gpu"; then
  bash "$script_dir/verify-hdr-image.sh" "$wolf_image"
fi
echo "Built: $wolf_image and $kde_image"
if "$deploy"; then
  WOLF_IMAGE="$wolf_image" docker compose -f "$WOLF_COMPOSE_FILE" up -d --no-deps --force-recreate wolf
else
  echo 'Deployment skipped. Existing streams and running containers are unchanged.'
fi
