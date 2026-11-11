#!/bin/ksh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

fail() {
    echo "CarPlay_Multitouch ERROR: $*" >&2
    exit 1
}

cksum() {
    [ -f "$MOD_PATH/cksum" ] && [ ! -L "$MOD_PATH/cksum" ] &&
        [ -x "$MOD_PATH/cksum" ] || fail "Package checksum helper missing"
    "$MOD_PATH/cksum"
}

check_file() {
    [ -f "$1" ] && [ ! -L "$1" ] || fail "Expected regular file: $1"
    observed=$(cksum <"$1") || fail "Cannot checksum $1"
    [ "$observed" = "$2" ] || fail "File identity differs: $1"
}

copy_checked() {
    [ ! -L "$2" ] && { [ ! -e "$2" ] || [ -f "$2" ]; } ||
        fail "Refusing invalid copy target: $2"
    cp -p "$1" "$2" || fail "Cannot copy $1 to $2"
    check_file "$2" "$3"
}

check_platform() {
    [ "${MODKIT_VERSION:-}" = 2 ] || fail "Requires MH2P SD ModKit version 2"
    [ "${RELEASE_VERSION:-}" = MH2p_ER_POG35_P2865 ] ||
        fail "Only MH2p_ER_POG35_P2865 is supported"
    [ "${OEM:-}" = PO ] && [ "${TYPE:-}" = G35 ] &&
        [ "${REGION:-}" = ER ] && [ "${RELEASE_TYPE:-}" = P ] &&
        [ "${SOFTWARE_VERSION:-}" = 2865 ] || fail "Firmware metadata differs"
    [ -n "${MOD_PATH:-}" ] && [ -d "$MOD_PATH" ] || fail "ModKit MOD_PATH missing"
    [ -d /mnt/app/eso/hmi/lsd ] && [ -d /mnt/app/eso/bin/apps ] ||
        fail "APP filesystem is not mounted at /mnt/app"
    mount -uw /mnt/app || fail "Cannot remount APP read/write"
}

APP=/mnt/app
STATE="$APP/eso/hmi/lsd/.mh2p-carplay-multitouch"
LAUNCHER="$APP/eso/hmi/lsd/lsd.sh"
NATIVE="$APP/eso/bin/apps/dio_manager"

restore_file() {
    copy_checked "$1" "$2.mh2p-multitouch.tmp" "$3"
    mv -f "$2.mh2p-multitouch.tmp" "$2" || fail "Cannot restore $2"
    check_file "$2" "$3"
}
