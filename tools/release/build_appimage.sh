#!/bin/sh
# Build the portable AppImage + signed update manifest for one release (docs/UPDATES.md "Releasing").
#   tools/release/build_appimage.sh X.Y.Z [OWNER/REPO]
# Output: out/release-X.Y.Z/ (AppImage, bbtime, manifest.json, manifest.json.sig, source tarball, SHA256SUMS).
# Builds inside an Ubuntu 24.04 container (glibc 2.39) so the AppImage runs on distros of that age and newer.
set -eu
VER=$1
REPO=${2:-sergen213/bugs-bunny-time-linux}
KEY=${BBTIME_RELEASE_KEY:-$HOME/.local/share/bbtime-release/update-ed25519.key}
cd "$(dirname "$0")/../.."
OUT=out/release-$VER
IMG=bbtime-appimage-toolchain:1
B=build-appimage

grep -q "project(bbtime VERSION $VER " CMakeLists.txt || { echo "CMakeLists.txt project VERSION is not $VER" >&2; exit 1; }
docker build -q -t $IMG tools/release/appimage >/dev/null
rm -rf "$OUT" $B/AppDir && mkdir -p "$OUT"
docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -v "$PWD:/src" -w /src $IMG sh -euc "
    CXX=g++-14 CC=gcc-14 cmake -S . -B $B -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/opt/ikv-sdl3 >/dev/null
    cmake --build $B -j2 --target bbtime bbtime-launcher bbtime-modkit
    cd $B
    mkdir -p AppDir/usr/share/licenses/fidelityfx AppDir/usr/share/licenses/stb
    cp /src/third_party/fidelityfx-sdk/LICENSE.txt AppDir/usr/share/licenses/fidelityfx/LICENSE.txt
    cp /src/third_party/stb/LICENSE AppDir/usr/share/licenses/stb/LICENSE
    # Wayland GL (launcher model viewer): linuxdeploy-plugin-qt skips the client buffer integrations; without
    # wayland-egl Qt has no OpenGL on Wayland. Copied before linuxdeploy so their libraries get deployed too.
    mkdir -p AppDir/usr/plugins/wayland-graphics-integration-client
    cp /usr/lib/x86_64-linux-gnu/qt6/plugins/wayland-graphics-integration-client/libqt-plugin-wayland-egl.so \
        AppDir/usr/plugins/wayland-graphics-integration-client/
    LDAI_OUTPUT=bbtime-$VER-x86_64.AppImage linuxdeploy --appdir AppDir \
        -e apps/launcher/bbtime-launcher -e apps/bbtime/bbtime -e tools/bbtime-modkit \
        --deploy-deps-only AppDir/usr/plugins/wayland-graphics-integration-client \
        -d /src/tools/release/appimage/bbtime.desktop -i /src/tools/release/appimage/bbtime.svg \
        --plugin qt --output appimage"
mv $B/bbtime-$VER-x86_64.AppImage "$OUT/"
cp $B/apps/bbtime/bbtime "$OUT/bbtime"

# Update channel: the game binary (the launcher prefers app/current/bbtime) and the AppImage as
# app/current/bbtime-launcher, which an older launcher restarts into (docs/UPDATES.md).
python3 tools/release/make_manifest.py --version "$VER" \
    --base-url "https://github.com/$REPO/releases/download/v$VER" \
    --exec bbtime --exec bbtime-launcher -o "$OUT/manifest.json" \
    "$OUT/bbtime" "bbtime-launcher=$OUT/bbtime-$VER-x86_64.AppImage"
python3 tools/release/sign_manifest.py --key "$KEY" "$OUT/manifest.json"
python3 tools/release/sign_manifest.py --verify "$OUT/manifest.json"
git archive --format=tar.gz --prefix="bbtime-$VER/" -o "$OUT/bbtime-$VER-source.tar.gz" HEAD
(cd "$OUT" && sha256sum -- * > SHA256SUMS)
ls -la "$OUT"
