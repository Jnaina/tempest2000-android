#!/bin/sh
# Fetches and builds everything the Android app needs that is not stored in this repository (works on macOS, Linux and
# Windows under WSL2 / Git Bash; needs git, make and a C compiler):
#
#   1. Jeff Minter's Tempest 2000 source          (github.com/mwenge/tempest2k, pinned commit)
#   2. his assembler tools rmac and rln            (built here; both pinned)
#   3. the game itself, t2000.abs, assembled from that source and checked to be byte-identical to the 1994 build
#   4. the Virtual Jaguar emulator core            (github.com/libretro/virtualjaguar-libretro, pinned commit, with patches/ applied)
#   5. t2000.abs copied into the app's assets
#
# Everything lands in ./deps (git-ignored). Safe to run again: finished steps are skipped.
set -e
cd "$(dirname "$0")"
DEPS=deps

GAME_URL=https://github.com/mwenge/tempest2k.git;              GAME_SHA=8b50b467ce7dd7eee475aaf5ae86804d95401ae7
RMAC_URL=https://github.com/mwenge/rmac.git;                   RMAC_SHA=50e66eaba10807de992792daaf700b6575ef43a6
RLN_URL=http://tiddly.mooo.com:5000/rln/rln.git;               RLN_SHA=a617009d7a41c2ddf10d5fcec21e0d324c26ce4e
CORE_URL=https://github.com/libretro/virtualjaguar-libretro.git; CORE_SHA=dd332ea34ef62106af599b7d9267571843873c8e
GAME_MD5=44e71799ee06615a59ff57b2c8a1ef52

need() { command -v "$1" >/dev/null 2>&1 || { echo "setup.sh: '$1' is required but was not found ($2)"; exit 1; }; }
need git "install git"
need make "macOS: xcode-select --install; Debian/Ubuntu: sudo apt install build-essential"
CC_BIN=${CC:-cc}; command -v "$CC_BIN" >/dev/null 2>&1 || CC_BIN=gcc
command -v "$CC_BIN" >/dev/null 2>&1 || { echo "setup.sh: no C compiler found (macOS: xcode-select --install; Debian/Ubuntu: sudo apt install build-essential)"; exit 1; }

# fetch <dir> <url> <commit>: a checkout of exactly that commit (shallow when the server allows fetching by hash)
fetch() {
    if [ ! -d "$1/.git" ]; then git init -q "$1"; git -C "$1" remote add origin "$2"; fi
    if [ "$(git -C "$1" rev-parse HEAD 2>/dev/null)" != "$3" ]; then
        echo "downloading $2 ..."
        git -C "$1" fetch -q --depth 1 origin "$3" 2>/dev/null || git -C "$1" fetch -q origin
        git -C "$1" checkout -q "$3" 2>/dev/null || git -C "$1" checkout -q FETCH_HEAD
    fi
    [ "$(git -C "$1" rev-parse HEAD)" = "$3" ] || { echo "setup.sh: could not check out $3 from $2"; exit 1; }
}

md5_of() { if command -v md5sum >/dev/null 2>&1; then md5sum "$1" | cut -d' ' -f1; elif command -v md5 >/dev/null 2>&1; then md5 -q "$1"; else openssl md5 "$1" | sed 's/.*= //'; fi; }

mkdir -p "$DEPS"

# 1-2. the game source and Jeff's assembler tools (his Makefile expects rmac/ and rln/ inside the source tree)
fetch "$DEPS/tempest2k"      "$GAME_URL" "$GAME_SHA"
fetch "$DEPS/tempest2k/rmac" "$RMAC_URL" "$RMAC_SHA"
fetch "$DEPS/tempest2k/rln"  "$RLN_URL"  "$RLN_SHA"
[ -x "$DEPS/tempest2k/rmac/rmac" ] || { echo "building rmac ..."; make -C "$DEPS/tempest2k/rmac" >/dev/null; }
[ -x "$DEPS/tempest2k/rln/rln" ]   || { echo "building rln ...";  make -C "$DEPS/tempest2k/rln"  >/dev/null; }

# 3. assemble the game. (Jeff's Makefile ends with an md5sum check that some systems lack, so the check is repeated below.)
echo "assembling Tempest 2000 ..."
make -C "$DEPS/tempest2k" t2000.abs >"$DEPS/assemble.log" 2>&1 || true
ABS="$DEPS/tempest2k/t2000.abs"
[ -f "$ABS" ] || { echo "setup.sh: assembling failed, see $DEPS/assemble.log"; tail -5 "$DEPS/assemble.log"; exit 1; }
if [ "$(md5_of "$ABS")" = "$GAME_MD5" ]; then echo "t2000.abs: byte-identical to the original 1994 build"
else echo "WARNING: t2000.abs differs from the original 1994 build (md5 $(md5_of "$ABS"))"; fi

# 4. the emulator core, with our blitter patch
fetch "$DEPS/virtualjaguar-libretro" "$CORE_URL" "$CORE_SHA"
PATCH="$PWD/patches/virtualjaguar-blitter.patch"
if git -C "$DEPS/virtualjaguar-libretro" apply --check --whitespace=nowarn "$PATCH" 2>/dev/null; then
    git -C "$DEPS/virtualjaguar-libretro" apply --whitespace=nowarn "$PATCH"; echo "applied patches/virtualjaguar-blitter.patch"
elif git -C "$DEPS/virtualjaguar-libretro" apply --reverse --check --whitespace=nowarn "$PATCH" 2>/dev/null; then
    echo "core patch already applied"
else echo "setup.sh: the core patch does not apply"; exit 1; fi

# 5. the game goes into the app
mkdir -p app/src/main/assets
cp "$ABS" app/src/main/assets/t2000.abs
echo "ready: app/src/main/assets/t2000.abs ($(wc -c < app/src/main/assets/t2000.abs | tr -d ' ') bytes), emulator core in $DEPS/virtualjaguar-libretro"
