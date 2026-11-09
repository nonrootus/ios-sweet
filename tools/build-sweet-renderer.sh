#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -ne 1 ]; then
    echo 'usage: bash tools/build-sweet-renderer.sh OUTPUT_DIRECTORY' >&2
    exit 2
fi
repo=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$1"
out=$(cd "$1" && pwd)
work=$(mktemp -d "$out/.build.XXXXXX")
stage="$work/source"
topaz=e4772911e116ab4ab6c4ac1f38e6106fefe8b483
hakios=63fc2b4633bac3af8e57dadab9f5a8d4cb940438
mesa_version=25.0.7
mesa_sha=592272df3cf01e85e7db300c449df5061092574d099da275d19e97ef0510f8a6
sdk=$(xcrun --sdk iphoneos --show-sdk-path)
cc=$(xcrun --sdk iphoneos --find clang)
cxx=$(xcrun --sdk iphoneos --find clang++)

fetch() {
    mkdir -p "$(dirname "$2")"
    curl --fail --silent --show-error --location --retry 3 "$1" -o "$2"
}

for path in CREDITS.md userland/gpu/{eagl.c,gl_osmesa.c,redwood_gl.h} \
    userland/mesa/{freedreno-darwin.patch,osmesa-es-profile.patch} \
    userland/mesa/redwood-drm/{redwood_drm.c,xf86drm.h}; do
    fetch "https://raw.githubusercontent.com/MakrSas/ios-topaz/$topaz/$path" "$stage/$path"
done
for path in userland/mkeagl.py userland/src/eagl_slots.txt; do
    fetch "https://raw.githubusercontent.com/oosexgang/hakios-xnu-kernel/$hakios/$path" "$stage/$path"
done

git -C "$stage" apply "$repo/userland/gpu/eagl-b3a8.patch"
git -C "$stage" apply --include=iokit/Kernel/arm/sweet_b3a8.h \
    "$repo/xnu/patches/0177-sweet-b3a8-framebuffer.patch"
cp "$stage/iokit/Kernel/arm/sweet_b3a8.h" "$stage/userland/gpu/"

mesa="$work/mesa-$mesa_version"
tarball="$work/mesa-$mesa_version.tar.xz"
fetch "https://archive.mesa3d.org/mesa-$mesa_version.tar.xz" "$tarball"
printf '%s  %s\n' "$mesa_sha" "$tarball" | shasum -a 256 -c -
tar -xf "$tarball" -C "$work"
for patch in "$stage"/userland/mesa/*.patch; do
    patch -d "$mesa" -p1 --batch < "$patch"
done
cp -R "$stage/userland/mesa/redwood-drm" "$mesa/src/"
python3 - "$mesa/src/redwood-drm/redwood_drm.c" "$stage/userland/gpu/eagl.c" <<'PY'
from pathlib import Path
import sys

for path, before, after in (
    (sys.argv[1], '"RedwoodGPU"', '"SweetAdreno"'),
    (sys.argv[2], 'share_surfaces = strstr(arguments, "glcopy=1") == NULL;', 'share_surfaces = 0;'),
    (sys.argv[2], 'static int share_surfaces = 1;', 'static int share_surfaces = 0;'),
):
    source = Path(path)
    data = source.read_text()
    assert data.count(before) == 1, (path, before)
    source.write_text(data.replace(before, after))
PY

python3 -m venv "$work/venv"
"$work/venv/bin/python" -m pip -q install 'Mako==1.3.10' 'PyYAML==6.0.2' 'packaging==25.0'
export PATH="$work/venv/bin:$(brew --prefix bison)/bin:$PATH"
cat > "$work/ios-arm64.ini" <<EOF
[binaries]
c = ['$cc', '-target', 'arm64-apple-ios16.0', '-isysroot', '$sdk']
cpp = ['$cxx', '-target', 'arm64-apple-ios16.0', '-isysroot', '$sdk', '-fuse-cxa-atexit']
objc = ['$cc', '-target', 'arm64-apple-ios16.0', '-isysroot', '$sdk']
ar = '$(xcrun --find ar)'
ranlib = '$(xcrun --find ranlib)'
strip = '$(xcrun --find strip)'
pkg-config = 'false'

[host_machine]
system = 'darwin'
subsystem = 'ios'
cpu_family = 'aarch64'
cpu = 'arm64'
endian = 'little'

[properties]
needs_exe_wrapper = true
EOF

meson setup "$work/mesa-build" "$mesa" --cross-file "$work/ios-arm64.ini" --wrap-mode=nofallback \
    --prefix=/sweet --libdir=lib --buildtype=release -Db_ndebug=true \
    -Dplatforms= -Dgallium-drivers=softpipe,freedreno -Dfreedreno-kmds=msm \
    -Dvulkan-drivers= -Dglx=disabled -Degl=disabled -Dgbm=disabled \
    -Dosmesa=true -Dllvm=disabled -Dshared-glapi=enabled -Dgles1=disabled -Dgles2=enabled -Dopengl=true \
    -Dzstd=disabled -Dxmlconfig=disabled -Dexpat=disabled -Dshader-cache=disabled -Dvalgrind=disabled \
    -Dlibunwind=disabled -Dglvnd=disabled -Dvideo-codecs= -Dtools= -Dgallium-vdpau=disabled \
    -Dgallium-va=disabled -Dgallium-xa=disabled -Dlmsensors=disabled -Dbuild-tests=false
ninja -C "$work/mesa-build"

lib="$out/sweet/lib"
mkdir -p "$lib"
xcrun strip -x -o "$lib/libOSMesa.8.dylib" "$work/mesa-build/src/gallium/targets/osmesa/libOSMesa.8.dylib"
xcrun install_name_tool -id /sweet/lib/libOSMesa.8.dylib "$lib/libOSMesa.8.dylib"
xcrun nm -gU "$lib/libOSMesa.8.dylib" | awk '$3 ~ /^_gl/ { print substr($3, 2) }' > "$work/exported-gl.txt"
gpu="$stage/userland/gpu"
python3 "$stage/userland/mkeagl.py" "$stage/userland/src/eagl_slots.txt" \
    "$mesa/src/mapi/glapi/registry/gl.xml" "$work/exported-gl.txt" "$gpu/eagl.c" > "$work/eagl_gen.h"

target=(-target arm64-apple-ios16.0 -isysroot "$sdk" -O2 -dynamiclib)
"$cc" "${target[@]}" -Wno-deprecated-declarations -I "$work" \
    "$gpu/eagl.c" "$gpu/gl_osmesa.c" "$lib/libOSMesa.8.dylib" -lobjc \
    -framework IOSurface -framework CoreFoundation -framework OpenGLES -framework IOKit \
    -install_name /sweet/lib/libSweetEAGL.dylib -o "$lib/libSweetEAGL.dylib"
"$cc" "${target[@]}" -framework IOKit -framework CoreFoundation \
    "$repo/userland/gpu/sweet-renderer-loader.c" \
    -install_name /sweet/lib/libSweetRendererLoader.dylib -o "$lib/libSweetRendererLoader.dylib"
for binary in "$lib"/*.dylib; do
    codesign --force --sign - --timestamp=none "$binary"
    codesign --verify "$binary"
    [ "$(xcrun lipo -archs "$binary")" = arm64 ]
done
xcrun otool -L "$lib/libSweetEAGL.dylib" | grep -F '/sweet/lib/libOSMesa.8.dylib'
cp "$stage/CREDITS.md" "$out/CREDITS.md"
cp "$mesa/docs/license.rst" "$out/Mesa-license.rst"
printf 'ios-topaz %s\nhakios %s\nMesa %s %s\n' "$topaz" "$hakios" "$mesa_version" "$mesa_sha" > "$out/SOURCES.txt"
(cd "$out" && shasum -a 256 sweet/lib/*.dylib > SHA256SUMS)
