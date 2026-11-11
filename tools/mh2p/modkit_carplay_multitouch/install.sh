#!/bin/ksh
# SPDX-License-Identifier: GPL-2.0-or-later
. "$MOD_PATH/common.sh"
check_platform
IDENTITY_CKSUM=$(cat "$MOD_PATH/identity.cksum") || fail "Missing package identity"
check_file "$MOD_PATH/identity.sh" "$IDENTITY_CKSUM"
. "$MOD_PATH/identity.sh"

[ -f "$MOD_PATH/../enable-experimental.txt" ] ||
    fail "Create enable-experimental.txt in the mod folder to accept experimental status"
check_file "$MOD_PATH/contacts.jar" "$OVERLAY_CKSUM"
check_file "$MOD_PATH/dio_manager.json" "$CONFIG_CKSUM"
check_file "$MOD_PATH/lsd.sh" "$LAUNCHER_PATCHED_CKSUM"
check_file "$MOD_PATH/dio_manager.sh" "$WRAPPER_CKSUM"
check_file "$APP/eso/hmi/lsd/lsd.jar" "$ARCHIVE_CKSUM"
[ -f "$LAUNCHER" ] && [ ! -L "$LAUNCHER" ] &&
    [ -f "$NATIVE" ] && [ ! -L "$NATIVE" ] || fail "Invalid launcher targets"

if [ -e "$STATE" ]; then
    [ -d "$STATE" ] && [ ! -L "$STATE" ] || fail "Invalid backup directory"
    check_file "$STATE/identity.sh" "$IDENTITY_CKSUM"
    check_file "$STATE/lsd.original.sh" "$LAUNCHER_ORIGINAL_CKSUM"
    check_file "$STATE/original/dio_manager" "$NATIVE_CKSUM"
    current_launcher=$(cksum <"$LAUNCHER") || fail "Cannot read launcher"
    current_native=$(cksum <"$NATIVE") || fail "Cannot read native launcher"
    case "$current_launcher" in
        "$LAUNCHER_ORIGINAL_CKSUM"|"$LAUNCHER_PATCHED_CKSUM") ;;
        *) fail "Launcher changed since installation; refusing to overwrite" ;;
    esac
    case "$current_native" in
        "$NATIVE_CKSUM"|"$WRAPPER_CKSUM") ;;
        *) fail "Native launcher changed since installation; refusing to overwrite" ;;
    esac
else
    check_file "$LAUNCHER" "$LAUNCHER_ORIGINAL_CKSUM"
    check_file "$NATIVE" "$NATIVE_CKSUM"
    mkdir "$STATE.staging" || fail "Stale staging directory or cannot stage backup"
    mkdir "$STATE.staging/original" || fail "Cannot stage native backup"
    copy_checked "$LAUNCHER" "$STATE.staging/lsd.original.sh" "$LAUNCHER_ORIGINAL_CKSUM"
    copy_checked "$NATIVE" "$STATE.staging/original/dio_manager" "$NATIVE_CKSUM"
    copy_checked "$MOD_PATH/identity.sh" "$STATE.staging/identity.sh" "$IDENTITY_CKSUM"
    mv "$STATE.staging" "$STATE" || fail "Cannot commit backups"
fi

rollback() {
    code=$?
    trap - EXIT HUP INT TERM
    if [ "$code" -ne 0 ]; then
        echo "CarPlay_Multitouch ERROR: installation failed; restoring original launchers" >&2
        restore_file "$STATE/lsd.original.sh" "$LAUNCHER" "$LAUNCHER_ORIGINAL_CKSUM"
        restore_file "$STATE/original/dio_manager" "$NATIVE" "$NATIVE_CKSUM"
        sync || fail "Cannot sync restored launchers"
    fi
    exit "$code"
}
trap rollback EXIT
trap 'exit 1' HUP INT TERM

copy_checked "$MOD_PATH/contacts.jar" "$STATE/contacts.jar" "$OVERLAY_CKSUM"
copy_checked "$MOD_PATH/dio_manager.json" "$STATE/dio_manager.json" "$CONFIG_CKSUM"
copy_checked "$MOD_PATH/dio_manager.sh" "$NATIVE.mh2p-multitouch.tmp" "$WRAPPER_CKSUM"
chmod 755 "$NATIVE.mh2p-multitouch.tmp" || fail "Cannot make wrapper executable"
mv -f "$NATIVE.mh2p-multitouch.tmp" "$NATIVE" || fail "Cannot install wrapper"
copy_checked "$MOD_PATH/lsd.sh" "$LAUNCHER.mh2p-multitouch.tmp" "$LAUNCHER_PATCHED_CKSUM"
chmod 755 "$LAUNCHER.mh2p-multitouch.tmp" || fail "Cannot make launcher executable"
mv -f "$LAUNCHER.mh2p-multitouch.tmp" "$LAUNCHER" || fail "Cannot install launcher"
check_file "$LAUNCHER" "$LAUNCHER_PATCHED_CKSUM"
check_file "$NATIVE" "$WRAPPER_CKSUM"
sync || fail "Cannot sync installed modification"
trap - EXIT HUP INT TERM
echo "CarPlay_Multitouch: installed experimental two-contact patch; reboot and reconnect iPhone"
