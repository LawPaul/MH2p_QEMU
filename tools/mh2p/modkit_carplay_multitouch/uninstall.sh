#!/bin/ksh
# SPDX-License-Identifier: GPL-2.0-or-later
. "$MOD_PATH/common.sh"
check_platform
if [ ! -e "$STATE" ]; then
    echo "CarPlay_Multitouch: no installed backup; nothing to uninstall"
    exit 0
fi
[ -d "$STATE" ] && [ ! -L "$STATE" ] || fail "Invalid backup directory"
IDENTITY_CKSUM=$(cat "$MOD_PATH/identity.cksum") || fail "Missing package identity"
check_file "$MOD_PATH/identity.sh" "$IDENTITY_CKSUM"
. "$MOD_PATH/identity.sh"
check_file "$STATE/identity.sh" "$IDENTITY_CKSUM"
check_file "$STATE/lsd.original.sh" "$LAUNCHER_ORIGINAL_CKSUM"
check_file "$STATE/original/dio_manager" "$NATIVE_CKSUM"
[ ! -L "$LAUNCHER" ] && [ ! -L "$NATIVE" ] || fail "Refusing symlink targets"
current_launcher=$(cksum <"$LAUNCHER") || fail "Cannot read launcher"
current_native=$(cksum <"$NATIVE") || fail "Cannot read native launcher"
case "$current_launcher" in
    "$LAUNCHER_ORIGINAL_CKSUM"|"$LAUNCHER_PATCHED_CKSUM") ;;
    *) fail "Launcher modified by another change; refusing to restore over it" ;;
esac
case "$current_native" in
    "$NATIVE_CKSUM"|"$WRAPPER_CKSUM") ;;
    *) fail "Native launcher modified by another change; refusing to restore over it" ;;
esac
restore_file "$STATE/lsd.original.sh" "$LAUNCHER" "$LAUNCHER_ORIGINAL_CKSUM"
restore_file "$STATE/original/dio_manager" "$NATIVE" "$NATIVE_CKSUM"
sync || fail "Cannot sync restored launchers"
echo "CarPlay_Multitouch: uninstalled; originals restored, verified backups retained; reboot"
