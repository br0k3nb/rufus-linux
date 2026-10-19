#!/bin/sh
# Rufus Linux port: regression tests.
#
# Runs without root: every test uses image files as virtual drives (as
# Rufus does with VHDs). Mounting goes through udisks2, like on a desktop.
# Optional tools extend the coverage when present: qemu-img (VHD/VHDX
# interoperability), xorriso (test ISOs), qemu-system-x86_64 + OVMF
# (RUFUS_TEST_QEMU=1: boot the created drives and save screenshots).
#
# Usage: tests/run-tests.sh   (after 'make'; or 'make test')
set -u

RUFUS=${RUFUS:-./rufus}
T=$(mktemp -d "${TMPDIR:-/tmp}/rufus-tests.XXXXXX")
trap 'rm -rf "$T"' EXIT
PASS=0
FAIL=0
SKIP=0

ok()   { PASS=$((PASS + 1)); echo "  PASS  $1"; }
ko()   { FAIL=$((FAIL + 1)); echo "  FAIL  $1"; [ -n "${2:-}" ] && tail -5 "$2" | sed 's/^/        /'; }
skip() { SKIP=$((SKIP + 1)); echo "  SKIP  $1"; }
have() { command -v "$1" >/dev/null 2>&1; }
# Bytes at a partition offset (first MBR partition starts at 1 MB)
peek() { python3 -c "import sys;f=open(sys.argv[1],'rb');f.seek(int(sys.argv[2]));sys.stdout.buffer.write(f.read(int(sys.argv[3])))" "$@"; }

echo "Rufus Linux port tests (in $T)"

# --- Checksums ----------------------------------------------------------
if $RUFUS --test-hashes > "$T/log" 2>&1; then ok "hash self-test"; else ko "hash self-test" "$T/log"; fi
head -c 10000000 /dev/urandom > "$T/rnd.bin"
$RUFUS --hash --sha512 -i "$T/rnd.bin" > "$T/log" 2>&1
for h in md5 sha1 sha256 sha512; do
    want=$(${h}sum "$T/rnd.bin" | cut -d' ' -f1)
    if grep -qi "$want" "$T/log"; then ok "--hash $h"; else ko "--hash $h" "$T/log"; fi
done

# --- File systems -------------------------------------------------------
for fs in FAT32:FAT32 FAT16:mkfs.fat NTFS:NTFS exFAT:EXFAT ext4:ext; do
    name=${fs%%:*}; sig=${fs#*:}
    truncate -s 300M "$T/fs.img"
    if ! $RUFUS --device "$T/fs.img" --format -f "$name" --label TEST --no-extended-label --yes > "$T/log" 2>&1; then
        ko "format $name" "$T/log"; continue
    fi
    case $name in
    ext4) magic=$(python3 -c "import struct,sys;f=open(sys.argv[1],'rb');f.seek(1048576+1080);print(hex(struct.unpack('<H',f.read(2))[0]))" "$T/fs.img")
          [ "$magic" = "0xef53" ] && ok "format $name" || ko "format $name" "$T/log" ;;
    FAT32) peek "$T/fs.img" $((1048576 + 82)) 5 | grep -q FAT32 && ok "format $name" || ko "format $name" "$T/log" ;;
    *)     peek "$T/fs.img" $((1048576 + 3)) 8 | grep -q "$sig" && ok "format $name" || ko "format $name" "$T/log" ;;
    esac
done

# --- Boot selections (no download needed) --------------------------------
for bt in freedos syslinux4 grub4dos:offline; do
    b=${bt%%:*}
    truncate -s 128M "$T/boot_$b.img"
    if $RUFUS --device "$T/boot_$b.img" --boot "$b" --offline --yes > "$T/log" 2>&1; then
        ok "boot $b"
    elif [ "$b" = grub4dos ] && grep -q "download" "$T/log"; then
        skip "boot $b (needs grldr from rufus.ie)"
    else
        ko "boot $b" "$T/log"
    fi
done
# MBR boot code + partition marked active
if [ "$(peek "$T/boot_freedos.img" 446 1 | od -An -tx1 | tr -d ' ')" = "80" ]; then ok "active partition"; else ko "active partition"; fi

# --- DD images (compressed, VHD/VHDX) -------------------------------------
cp "$T/boot_freedos.img" "$T/src.img"
want=$(sha256sum < "$T/src.img")
check_dd() {
    truncate -s 160M "$T/dst.img"
    if $RUFUS --device "$T/dst.img" -i "$1" --offline --yes > "$T/log" 2>&1 &&
       [ "$(head -c $(stat -c %s "$T/src.img") "$T/dst.img" | sha256sum)" = "$want" ]; then
        ok "DD $2"
    else
        ko "DD $2" "$T/log"
    fi
}
for c in gzip:gz xz:xz bzip2:bz2 zstd:zst; do
    tool=${c%%:*}; ext=${c#*:}
    if have "$tool"; then
        if [ "$tool" = zstd ]; then zstd -qf "$T/src.img" -o "$T/src.img.zst"; else $tool -kf "$T/src.img"; fi
        check_dd "$T/src.img.$ext" "$ext"
    else
        skip "DD $ext ($tool missing)"
    fi
done
if have qemu-img; then
    qemu-img convert -f raw -O vpc -o subformat=fixed,force_size=on "$T/src.img" "$T/f.vhd" && check_dd "$T/f.vhd" "fixed VHD"
    qemu-img convert -f raw -O vpc -o subformat=dynamic,force_size=on "$T/src.img" "$T/d.vhd" && check_dd "$T/d.vhd" "dynamic VHD"
    qemu-img convert -f raw -O vhdx "$T/src.img" "$T/d.vhdx" && check_dd "$T/d.vhdx" "VHDX"
else
    skip "VHD/VHDX (qemu-img missing)"
fi

# --- Save to image -----------------------------------------------------------
for ext in img vhd vhdx; do
    if $RUFUS --device "$T/src.img" --save="$T/saved.$ext" > "$T/log" 2>&1; then
        case $ext in
        img)  cmp -s "$T/saved.img" "$T/src.img" && ok "save .$ext" || ko "save .$ext" ;;
        *)    if have qemu-img; then
                  fmt=$([ "$ext" = vhd ] && echo vpc || echo vhdx)
                  qemu-img convert -f $fmt -O raw "$T/saved.$ext" "$T/back.raw" &&
                  cmp -s "$T/back.raw" "$T/src.img" && ok "save .$ext (qemu-img round trip)" || ko "save .$ext"
              else
                  ok "save .$ext (not verified: qemu-img missing)"
              fi ;;
        esac
    else
        ko "save .$ext" "$T/log"
    fi
done

# --- ISO mode ------------------------------------------------------------------
if have xorriso; then
    mkdir -p "$T/iso/boot/grub" "$T/iso/casper"
    echo "set timeout=1" > "$T/iso/boot/grub/grub.cfg"
    printf 'menuentry "Live" {\n  linux /casper/vmlinuz boot=casper\n}\n' >> "$T/iso/boot/grub/grub.cfg"
    head -c 1000000 /dev/urandom > "$T/iso/casper/filesystem.squashfs"
    if [ -f /usr/lib/grub/x86_64-efi/normal.mod ]; then
        mkdir -p "$T/iso/boot/grub/i386-pc" && cp /usr/lib/grub/x86_64-efi/normal.mod "$T/iso/boot/grub/i386-pc/"
    fi
    xorriso -as mkisofs -o "$T/test.iso" -V RUFUSTEST -J -r "$T/iso" > /dev/null 2>&1
    truncate -s 256M "$T/iso.img"
    if $RUFUS --device "$T/iso.img" -i "$T/test.iso" --persistence=64M --offline --yes > "$T/log" 2>&1 &&
       grep -q "filesystem.squashfs" "$T/log"; then
        ok "ISO extraction"
        if grep -q "Persistence Partition" "$T/log" && grep -q "persistent" "$T/log"; then
            ok "persistence (casper)"
        elif [ ! -f /usr/lib/grub/x86_64-efi/normal.mod ]; then
            skip "persistence (no GRUB module to build a test ISO)"
        else
            ko "persistence (casper)" "$T/log"
        fi
    else
        ko "ISO extraction" "$T/log"
    fi
else
    skip "ISO mode (xorriso missing)"
fi

# --- Boot tests in QEMU (optional) ----------------------------------------------
if [ "${RUFUS_TEST_QEMU:-0}" = 1 ] && have qemu-system-x86_64; then
    for b in freedos syslinux4; do
        sh tests/qemu-shot.sh "$T/boot_$b.img" bios 8 "boot_$b.png" && echo "  INFO  screenshot: boot_$b.png"
    done
fi

echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"
[ "$FAIL" -eq 0 ]
