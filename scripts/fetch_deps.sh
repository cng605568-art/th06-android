#!/usr/bin/env bash
# Downloads the pinned third-party sources into third_party/ and verifies every
# download against a SHA-256 hash. Safe to re-run: finished steps are skipped.
#
#   SDL2 2.32.10      (zlib)              windowing, input, audio, Android glue (Java + native)
#   zstd 1.5.7        (BSD)               decompression for New Classic PKGL archives
#   stb_image 2.30    (public domain/MIT) PNG/JPEG/BMP/TGA decoding
#   bcdec 0.985       (MIT/Unlicense)     BC1/BC2/BC3/BC7 (DDS) decoding
#
# Bumping a version means changing its URL *and* hash here; CI caches third_party/
# keyed on this file's hash, so the cache refreshes automatically.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TP="$ROOT/third_party"
DL="$TP/.downloads"
mkdir -p "$DL" "$TP/include"

SDL2_VERSION=2.32.10
SDL2_URL="https://github.com/libsdl-org/SDL/releases/download/release-${SDL2_VERSION}/SDL2-${SDL2_VERSION}.tar.gz"
SDL2_SHA256=5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165

ZSTD_VERSION=1.5.7
ZSTD_URL="https://github.com/facebook/zstd/releases/download/v${ZSTD_VERSION}/zstd-${ZSTD_VERSION}.tar.gz"
ZSTD_SHA256=eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3

STB_COMMIT=003501f6d7a8668ce8b6d875e34dd97ad0d5d5b5
STB_URL="https://raw.githubusercontent.com/nothings/stb/${STB_COMMIT}/stb_image.h"
STB_SHA256=594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3

BCDEC_COMMIT=80859ed3b7afb1c527a2a99d70c61457bea72d0c
BCDEC_URL="https://raw.githubusercontent.com/iOrange/bcdec/${BCDEC_COMMIT}/bcdec.h"
BCDEC_SHA256=134520764d96f70a27db814173616c89ad85db95fbd3328417c7a8c77e7ca189

sha256_of() {
  if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1
  else shasum -a 256 "$1" | cut -d' ' -f1; fi
}

# fetch URL DEST SHA256
fetch() {
  local url="$1" dest="$2" want="$3"
  if [ -f "$dest" ] && [ "$(sha256_of "$dest")" = "$want" ]; then
    return 0
  fi
  echo "Downloading $url"
  curl -fsSL --retry 4 --retry-delay 3 -o "$dest.part" "$url"
  local got
  got="$(sha256_of "$dest.part")"
  if [ "$got" != "$want" ]; then
    rm -f "$dest.part"
    echo "ERROR: checksum mismatch for $url" >&2
    echo "  expected $want" >&2
    echo "  got      $got" >&2
    exit 1
  fi
  mv "$dest.part" "$dest"
}

# extract TARBALL DESTDIR STAMP
extract() {
  local tarball="$1" dest="$2" stamp="$3"
  if [ -f "$dest/.stamp" ] && [ "$(cat "$dest/.stamp")" = "$stamp" ]; then
    return 0
  fi
  rm -rf "$dest"
  mkdir -p "$dest"
  tar -xzf "$tarball" -C "$dest" --strip-components=1
  echo "$stamp" > "$dest/.stamp"
}

fetch "$SDL2_URL" "$DL/SDL2-${SDL2_VERSION}.tar.gz" "$SDL2_SHA256"
extract "$DL/SDL2-${SDL2_VERSION}.tar.gz" "$TP/SDL2" "$SDL2_SHA256"
# SDL's sample Android project ships its own Gradle wrapper. This project never runs it
# (CI uses a pinned Gradle), and CI's wrapper-validation check rejects unknown wrapper
# jars anywhere in the checkout, so remove them on every run.
rm -rf "$TP/SDL2/android-project/gradle" "$TP/SDL2/android-project/gradlew" "$TP/SDL2/android-project/gradlew.bat"
find "$TP" -name 'gradle-wrapper.jar' -delete

fetch "$ZSTD_URL" "$DL/zstd-${ZSTD_VERSION}.tar.gz" "$ZSTD_SHA256"
extract "$DL/zstd-${ZSTD_VERSION}.tar.gz" "$TP/zstd" "$ZSTD_SHA256"

fetch "$STB_URL" "$TP/include/stb_image.h" "$STB_SHA256"
fetch "$BCDEC_URL" "$TP/include/bcdec.h" "$BCDEC_SHA256"

# SDL's Java glue must match the native library version exactly; Gradle compiles it
# straight from third_party/SDL2/android-project, so verify it is there.
test -f "$TP/SDL2/android-project/app/src/main/java/org/libsdl/app/SDLActivity.java" || {
  echo "ERROR: SDL2 Java sources missing from the release tarball" >&2; exit 1; }

echo "third_party ready: SDL2 $SDL2_VERSION, zstd $ZSTD_VERSION, stb_image @${STB_COMMIT:0:7}, bcdec @${BCDEC_COMMIT:0:7}"
