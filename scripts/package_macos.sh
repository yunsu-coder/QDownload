#!/bin/zsh
set -euo pipefail

project_dir=${0:A:h:h}
build_dir="$project_dir/build-release"
dist_dir="$project_dir/dist"
stage_dir="$project_dir/build-package"
qt_prefix=${QT_PREFIX:-/Users/gzhysu/Qt/6.11.1/macos}
cmake_bin=${CMAKE_BIN:-/Users/gzhysu/Qt/Tools/CMake/CMake.app/Contents/bin/cmake}
ctest_bin=${CTEST_BIN:-/Users/gzhysu/Qt/Tools/CMake/CMake.app/Contents/bin/ctest}
ninja_bin=${NINJA_BIN:-/Users/gzhysu/Qt/Tools/Ninja/ninja}

"$cmake_bin" \
  -S "$project_dir" \
  -B "$build_dir" \
  -G Ninja \
  -DCMAKE_MAKE_PROGRAM="$ninja_bin" \
  -DCMAKE_PREFIX_PATH="$qt_prefix" \
  -DCMAKE_BUILD_TYPE=Release
"$cmake_bin" --build "$build_dir" --parallel
"$ctest_bin" --test-dir "$build_dir" --output-on-failure

"$cmake_bin" --install "$build_dir" --prefix "$stage_dir"
mkdir -p "$dist_dir"
app_path="$stage_dir/QuickDL.app"
version=$(
  "$cmake_bin" -LA -N "$build_dir" |
    awk -F= '/CMAKE_PROJECT_VERSION:STATIC=/{print $2}'
)

if [[ ! -d "$app_path" ]]; then
  echo "QuickDL.app was not installed" >&2
  exit 1
fi

if [[ -n "${QUICKDL_CODESIGN_IDENTITY:-}" ]]; then
  codesign --force --deep --options runtime \
    --timestamp --sign "$QUICKDL_CODESIGN_IDENTITY" "$app_path"
  codesign --verify --deep --strict --verbose=2 "$app_path"
fi

ln -sfn /Applications "$stage_dir/Applications"
dmg_path="$dist_dir/QuickDL-${version}-macOS-Beta.dmg"
hdiutil create -quiet -ov \
  -volname "QuickDL ${version}" \
  -srcfolder "$stage_dir" \
  -format UDZO "$dmg_path"

if [[ -n "${QUICKDL_CODESIGN_IDENTITY:-}" ]]; then
  codesign --force --timestamp \
    --sign "$QUICKDL_CODESIGN_IDENTITY" "$dmg_path"
fi

if [[ -n "${APPLE_ID:-}" && -n "${APPLE_TEAM_ID:-}" \
      && -n "${APPLE_APP_PASSWORD:-}" ]]; then
  xcrun notarytool submit "$dmg_path" \
    --apple-id "$APPLE_ID" \
    --team-id "$APPLE_TEAM_ID" \
    --password "$APPLE_APP_PASSWORD" \
    --wait
  xcrun stapler staple "$dmg_path"
fi

(
  cd "$dist_dir"
  shasum -a 256 "$(basename "$dmg_path")" > SHA256SUMS
)

echo "Package written to $dist_dir"
