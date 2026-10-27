#!/usr/bin/env bash
# Build a temporary fastboot image from a separately built XNU kernel.
set -euo pipefail
if [ "$#" -ne 1 ]; then
    echo "usage: $0 /path/to/kernel.development.sweet" >&2
    exit 2
fi
kernel=$(realpath "$1")
repo=$(cd "$(dirname "$0")/.." && pwd)
capture=/mnt/d/ios-sweet/research/sweet-k6a-live-capture-20261027
cd "$repo"
export PYTHONPATH="/mnt/d/ios-sweet/tools/python${PYTHONPATH:+:$PYTHONPATH}"
for tool in python3 aarch64-linux-gnu-gcc aarch64-linux-gnu-objcopy aarch64-linux-gnu-readelf aarch64-linux-gnu-nm mkbootimg lzfse gcc gawk cpio gzip; do
    command -v "$tool" >/dev/null || { echo "Missing tool: $tool" >&2; exit 1; }
done
[ -s "$kernel" ] || { echo "Kernel not found: $kernel" >&2; exit 1; }
[ -s "$capture/boot-unpacked/dtb" ] || { echo "Phone DTB missing" >&2; exit 1; }
[ -s "$capture/DeviceTree.d22ap.im4p" ] || { echo "iPhone X device tree missing" >&2; exit 1; }
mkdir -p dtb/boot_unpacked research/ipsw-x shim/out
cp "$capture/boot-unpacked/dtb" dtb/boot_unpacked/dtb
python3 tools/adt.py unwrap "$capture/DeviceTree.d22ap.im4p" research/ipsw-x/d22ap.adt
python3 tools/mkadt.py shim/out/sweet-k6a.adt --highmem
python3 tools/mkpayload.py "$kernel" shim/out/sweet-k6a.adt shim/out/payload-k6a.bin \
    --cmdline 'debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam -noprogress rd=ufsroot0 unentitled_ios_sim_launch=1 ignition_level=1 wdt_test=no_user_checkin ufsrw=1 rootrw=1 launchd_unsecure_cache=1 wdt=-1 sweetfb_hz=30 sweetk6a=1'
# Windows checkouts can contain CRLF. Keep a temporary LF copy next to the
# original, since the original script resolves paths relative to its location.
shim_script=shim/.build-sweet-k6a-tmp.sh
trap 'rm -f "$shim_script"' EXIT
sed 's/\r$//' shim/build.sh > "$shim_script"
bash "$shim_script" ../shim/out/payload-k6a.bin
cp shim/out/shim-boot.img shim/out/shim-boot-sweet-k6a.img
sha256sum shim/out/shim-boot-sweet-k6a.img
echo 'Build only. userdata must contain the intended iOS root before booting this image.'
