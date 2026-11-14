#!/usr/bin/env bash
# CI helper: checks one built APK and copies it into a dist folder with its checksum.
#
#   scripts/stage_apk.sh APK APP_ID "LIBS" DIST_DIR BASENAME SUMS_FILE
#
# APK       the built release APK
# APP_ID    the application id it must have (so the apps install side by side)
# LIBS      native libraries every ABI in $ABIS must carry (space separated)
# DIST_DIR  where the renamed APK and SUMS_FILE go
# BASENAME  the APK's file name without ".apk" ("-debugsigned" is added without a release key)
#
# Needs ANDROID_HOME, BUILD_TOOLS_VERSION, ABIS and HAS_KEYSTORE from the workflow.
set -euo pipefail

apk="$1" app_id="$2" libs="$3" dist="$4" base="$5" sums="$6"
tools="$ANDROID_HOME/build-tools/$BUILD_TOOLS_VERSION"

test -f "$apk" || { echo "::error::$apk was not produced"; ls -R "$(dirname "$apk")" || true; exit 1; }
"$tools/apksigner" verify --verbose --print-certs "$apk"

badging="$([ -x "$tools/aapt2" ] && "$tools/aapt2" dump badging "$apk" 2>/dev/null || true)"
if [ -n "$badging" ]; then
  package_line="$(printf '%s\n' "$badging" | grep -m 1 '^package:' || true)"
  echo "$package_line"
  case "$package_line" in
    *"name='$app_id'"*) ;;
    *) echo "::error::$apk does not have the app id $app_id"; exit 1 ;;
  esac
else
  echo "::warning::aapt2 dump badging unavailable; app id of $apk not checked"
fi

# The app cannot start without these libraries for every ABI built.
unzip -l "$apk" 'lib/*'
for abi in ${ABIS//,/ }; do
  for lib in $libs; do
    unzip -l "$apk" "lib/$abi/$lib" > /dev/null || { echo "::error::lib/$abi/$lib missing from $apk"; exit 1; }
  done
done
"$tools/zipalign" -c -P 16 4 "$apk" \
  && echo "$base: native libraries are 16 KB page aligned" \
  || echo "::warning::$base: native libraries are not 16 KB page aligned (needed by some Android 15+ devices)"

mkdir -p "$dist"
suffix=""
[ "${HAS_KEYSTORE:-}" = "true" ] || suffix="-debugsigned"
cp "$apk" "$dist/${base}${suffix}.apk"
(cd "$dist" && sha256sum *.apk > "$sums" && cat "$sums")
