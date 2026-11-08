#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -ne 4 ]; then
  echo "Usage: $0 OWNER/REPO TAG ASSET_NAME OUTPUT_PATH" >&2
  exit 2
fi

REPOSITORY="$1"
TAG="$2"
ASSET="$3"
OUTPUT="$4"
API="https://api.github.com/repos/$REPOSITORY"
CURL_RETRY=(--fail --location --retry 6 --retry-all-errors --retry-delay 2 --connect-timeout 20)

asset_id="$(curl --silent --show-error "${CURL_RETRY[@]}" "$API/releases/tags/$TAG" \
  | python3 -c 'import json,sys; name=sys.argv[1]; assets=json.load(sys.stdin).get("assets", []); matches=[a["id"] for a in assets if a.get("name")==name]; print(matches[0] if matches else "")' "$ASSET")"
[ -n "$asset_id" ] || { echo "Release asset not found: $REPOSITORY@$TAG/$ASSET" >&2; exit 1; }

mkdir -p "$(dirname "$OUTPUT")"
curl --silent --show-error "${CURL_RETRY[@]}" -H 'Accept: application/octet-stream' \
  "$API/releases/assets/$asset_id" --output "$OUTPUT"
