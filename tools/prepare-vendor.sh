#!/bin/sh
# Rufus Linux port: prepare the vendored (upstream) source trees for a
# Linux build.
#
# The Windows build compiles these libraries straight out of src/. They are
# mostly portable C, but a few files include Windows-only headers or use
# Win32 types, so we build them from a generated copy under vendor/
# where those includes are redirected to the port's compatibility headers.
#
# Every transformation is listed below and annotated; the script is
# idempotent and always re-copies from src/ so the vendored code stays
# verifiably identical to upstream (except for the documented patches).
#
# Usage: tools/prepare-vendor.sh [repo_root]
set -e

ROOT="${1:-..}"
SRC="$ROOT/src"
VEN="vendor"

echo "Preparing vendored sources from $SRC"

# ---------------------------------------------------------------------
# ms-sys: MBR/PBR analysis and writing (vendored, compiled unchanged
# except for the rufus.h include path)
# ---------------------------------------------------------------------
rm -rf "$VEN/ms-sys"
mkdir -p "$VEN/ms-sys/inc"
cp "$SRC"/ms-sys/*.c "$VEN/ms-sys/"
cp "$SRC"/ms-sys/inc/*.h "$VEN/ms-sys/inc/"
# Patch: "../rufus.h" (the Windows core header) -> our compatibility core
sed -i 's|#include "\.\./rufus.h"|#include "rfus.h"|' "$VEN/ms-sys/file.c"

# ---------------------------------------------------------------------
# Provide the header the vendored trees expect one level up. We create a
# shim rather than editing each file.
# ---------------------------------------------------------------------
cat > "$VEN/rufus.h" <<'EOF'
/* Rufus Linux port: shim for vendored files that include "../rufus.h" */
#include "rfus.h"
EOF

# ---------------------------------------------------------------------
# bled: compressed image support (gzip, xz, bzip2, lzma, zstd, zip, Z,
# vtsi). Compiled unchanged except for libbb.h, its platform header, which
# upstream restricts to Windows: it is replaced with include/bled-libbb.h,
# the same header with the Windows CRT specifics swapped for POSIX ones.
# ---------------------------------------------------------------------
rm -rf "$VEN/bled"
mkdir -p "$VEN/bled"
cp "$SRC"/bled/*.c "$SRC"/bled/*.h "$VEN/bled/"
cp include/bled-libbb.h "$VEN/bled/libbb.h"
# Patch: transformer_switch_file() (bb_archive.h) converts the extraction
# path to Windows '\\' separators: keep the POSIX ones
perl -0pi -e "s/\t\tif \(dst\[i\] == '\/'\)\n\t\t\tdst\[i\] = '\\\\\\\\';\n\t\tif \(dst\[i\] == '\\\\\\\\'\)\n/\t\tif (dst[i] == '\/')\n/" "$VEN/bled/bb_archive.h"

# ---------------------------------------------------------------------
# ezxml (xml.c): XML parser used for the WIM XML data. Compiled unchanged
# except for the core header includes.
# ---------------------------------------------------------------------
rm -rf "$VEN/ezxml"
mkdir -p "$VEN/ezxml"
cp "$SRC"/xml.c "$SRC"/xml.h "$VEN/ezxml/"
sed -i 's|#include "rufus.h"|#include "rfus.h"|; s|#include "msapi_utf8.h"||' "$VEN/ezxml/xml.c"
# Patch: entity decoding uses MSVC's strncpy_s(s, c, ent, _TRUNCATE), which
# copies c-1 characters and a NUL terminator into the middle of the string.
# Use the original ezxml code (copy exactly c characters, no terminator).
sed -i 's|strncpy_s(s, c, ent\[b\], _TRUNCATE);|strncpy(s, ent[b], c);|' "$VEN/ezxml/xml.c"

echo "Vendor tree ready under $VEN/"
