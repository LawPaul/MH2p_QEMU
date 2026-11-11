#!/bin/ksh
# SPDX-License-Identifier: GPL-2.0-or-later
# Run inside the diagnostic MMX guest before starting application services.

fail()
{
    echo "MH2P HMI runtime: $*" >&2
    exit 1
}

[ "$#" -eq 0 ] || fail "No arguments are supported"
[ -d /mnt/persist_new/persistence ] ||
    fail "Mount the writable diagnostic persistence filesystem first"

for path in /dev/rdsys0 /dev/rdmediadb0 /ramdisk/pps; do
    [ ! -e "$path" ] || fail "Runtime already present at $path; refusing duplicate startup"
done

TIMEOUT=10
info()
{
    echo "MH2P HMI runtime: $*"
}

. /etc/boot/functions/create_ramdisk ||
    fail "Cannot load the original RAM-disk helper"

create_ramdisk 20 rdsys /ramdisks/sys ||
    fail "Cannot create the original system RAM disk"
waitfor /dev/rdsys0t77 10 && waitfor /ramdisks/sys 10 ||
    fail "System RAM disk did not become available"

/bin/mkdir -m 0755 /ramdisks/sys/var &&
    /bin/mkdir -m 1777 /ramdisks/sys/var/run &&
    /bin/mkdir -m 0755 /ramdisks/sys/pps &&
    chown 108:108 /ramdisks/sys/pps ||
    fail "Cannot initialize system RAM-disk directories"

create_ramdisk 160 rdmediadb /ramdisks/mediadb 1031 1031 0755 ||
    fail "Cannot create the original media RAM disk"
waitfor /dev/rdmediadb0t77 10 && waitfor /ramdisks/mediadb 10 ||
    fail "Media RAM disk did not become available"

if [ ! -d /mnt/persist_new/srmstorage ]; then
    /bin/mkdir -m 0755 /mnt/persist_new/srmstorage ||
        fail "Cannot create persistent media storage"
fi
chown 1031:1031 /mnt/persist_new/srmstorage &&
    chmod 0755 /mnt/persist_new/srmstorage ||
    fail "Cannot set the original persistent media storage permissions"

/armle/usr/sbin/pps -U 108:108 -A /etc/pps.acl \
    -m /ramdisk/pps -p /ramdisks/sys/pps >/tmp/mh2p-pps.log 2>&1 &
PPS_PID=$!
if ! waitfor /ramdisk/pps 10 || ! kill -0 "$PPS_PID"; then
    cat /tmp/mh2p-pps.log >&2
    fail "Original PPS service did not become available"
fi

echo "MH2P HMI runtime: original system/media RAM disks and PPS are available"
