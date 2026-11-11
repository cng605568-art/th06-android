#!/bin/bash
# Finishes the iOS app bundle CMake built and packs it as an unsigned .ipa (sideloading tools
# such as AltStore, SideStore or Sideloadly sign it with the player's own Apple ID).
#   scripts/package_ios.sh <path/TH07Port.app> <iphoneos|iphonesimulator> [out.ipa]
# Adds th07/ios/Settings.bundle (the options in the Settings app) and the compiled icon
# (th07/ios/Assets.xcassets). Without an output path only the bundle is finished.
set -euo pipefail

APP="$1"
PLATFORM="$2"
OUT="${3:-}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IOS="$ROOT/th07/ios"

test -d "$APP" || { echo "ERROR: $APP is not an app bundle" >&2; exit 1; }
test -f "$APP/Info.plist" || { echo "ERROR: $APP has no Info.plist" >&2; exit 1; }

rm -rf "$APP/Settings.bundle"
cp -R "$IOS/Settings.bundle" "$APP/Settings.bundle"
printf 'APPL????' > "$APP/PkgInfo"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
xcrun actool "$IOS/Assets.xcassets" --compile "$APP" --platform "$PLATFORM" \
  --minimum-deployment-target 16.3 --app-icon AppIcon --target-device iphone --target-device ipad \
  --output-partial-info-plist "$TMP/assets.plist" --output-format human-readable-text --notices --warnings

if [ "$PLATFORM" = "iphonesimulator" ]; then
  /usr/libexec/PlistBuddy -c "Set :CFBundleSupportedPlatforms:0 iPhoneSimulator" "$APP/Info.plist"
fi
plutil -lint "$APP/Info.plist" "$APP/Settings.bundle/Root.plist"
EXE="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$APP/Info.plist")"
test -x "$APP/$EXE" || { echo "ERROR: $APP/$EXE missing" >&2; exit 1; }
lipo -info "$APP/$EXE"
ls -la "$APP"

if [ -n "$OUT" ]; then
  mkdir -p "$(dirname "$OUT")"
  rm -rf "$TMP/Payload" && mkdir -p "$TMP/Payload"
  cp -R "$APP" "$TMP/Payload/"
  OUT_ABS="$(cd "$(dirname "$OUT")" && pwd)/$(basename "$OUT")"
  rm -f "$OUT_ABS"
  (cd "$TMP" && zip -qry "$OUT_ABS" Payload)
  ls -la "$OUT_ABS"
fi
