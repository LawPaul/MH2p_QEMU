#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu
state=/mnt/app/eso/hmi/lsd/.mh2p-carplay-multitouch
if [ ! -r "$state/dio_manager.json" ] || [ ! -x "$state/original/dio_manager" ]; then
    echo "CarPlay_Multitouch ERROR: native configuration or original executable missing" >&2
    exit 1
fi
export IPL_CONFIG_DIR_DIO_MANAGER="$state"
exec "$state/original/dio_manager" "$@"
