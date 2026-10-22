#!/bin/bash

set -e

CURRENT_PATH=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SDK_PATH=$(CDPATH= cd -- "$CURRENT_PATH/.." && pwd)
BOARD=${2:-cv1815ja_bf01_spinand}
APP_INSTALL_DIR="$CURRENT_PATH/install"
DATA_DIR="$SDK_PATH/install/soc_$BOARD/data"

if [ "${1-}" != pack_data ]; then
	echo "Usage: $0 pack_data [board]" >&2
	exit 2
fi

if [ "$BOARD" != cv1815ja_bf01_spinand ]; then
	echo "Unsupported BF01 board: $BOARD" >&2
	exit 2
fi

if [ ! -x "$APP_INSTALL_DIR/ipcamera" ]; then
	echo "Missing installed IPCamera at $APP_INSTALL_DIR/ipcamera; run make ipcamera install first." >&2
	exit 1
fi

mkdir -p "$DATA_DIR"
find "$DATA_DIR" -mindepth 1 -maxdepth 1 -exec rm -rf -- {} +
cp -a "$APP_INSTALL_DIR"/. "$DATA_DIR"/

cd "$SDK_PATH"
source build/envsetup_soc.sh
defconfig "$BOARD"
pack_data
