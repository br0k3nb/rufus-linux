#!/bin/sh
# Boot a disk image in QEMU (BIOS or UEFI), wait, and save a screenshot.
# Usage: qemu-shot.sh <image> <bios|uefi> <seconds> <out.png>
# Used to verify that drives written by the port actually boot.
set -e
img=$1; mode=$2; secs=$3; out=$4
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fw=""
if [ "$mode" = "uefi" ]; then
    cp /usr/share/OVMF/OVMF_VARS_4M.fd "$tmp/vars.fd"
    fw="-drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd -drive if=pflash,format=raw,file=$tmp/vars.fd"
fi
mkfifo "$tmp/mon.in" "$tmp/mon.out"
# shellcheck disable=SC2086
qemu-system-x86_64 $( [ -w /dev/kvm ] && echo -enable-kvm -cpu host ) -m ${QEMU_MEM:-512} -display none $fw -snapshot \
    -drive file="$img",format=raw,if=ide -monitor pipe:"$tmp/mon" -no-reboot &
pid=$!
cat "$tmp/mon.out" >/dev/null &
exec 3>"$tmp/mon.in"
sleep "$secs"
echo "screendump $tmp/shot.ppm" >&3
sleep 1
echo "quit" >&3
wait $pid 2>/dev/null || true
python3 -c "from PIL import Image; import sys; Image.open(sys.argv[1]).save(sys.argv[2])" "$tmp/shot.ppm" "$out"
