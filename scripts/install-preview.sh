#!/bin/sh

set -eu
umask 077
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
if [ -f "$SCRIPT_DIR/ipcamera" ]; then
    SOURCE=$SCRIPT_DIR
    RESOURCE=$SCRIPT_DIR
else
    SOURCE="$SCRIPT_DIR/../install"
    RESOURCE="$SCRIPT_DIR/../resource/preview"
fi
usage()
{
    echo 'Usage: install-preview.sh ROOTFS [--boot off|sta|ap] [--ap-config PRIVATE_FILE]' >&2
    exit 1
}
[ "$#" -ge 1 ] || usage
ROOTFS=$1
shift
BOOT_MODE=off
AP_CONFIG=
while [ "$#" -gt 0 ]; do
    [ "$#" -ge 2 ] || usage
    case "$1" in
        --boot) BOOT_MODE=$2 ;;
        --ap-config) AP_CONFIG=$2 ;;
        *) usage ;;
    esac
    shift 2
done
case "$BOOT_MODE" in off|sta|ap) ;; *) usage ;; esac
[ -d "$ROOTFS/etc/init.d" ] || { echo 'Target must be an existing staged rootfs' >&2; exit 1; }
ROOTFS=$(CDPATH= cd -- "$ROOTFS" && pwd)
[ "$ROOTFS" != / ] || { echo 'Refusing to install into host /' >&2; exit 1; }
[ ! -e "$ROOTFS/usr/bin/bf01-preview" ] || [ -L "$ROOTFS/usr/bin/bf01-preview" ] || {
    echo 'Refusing to replace an existing /usr/bin/bf01-preview file' >&2
    exit 1
}
for FILE in ipcamera param_config.ini bf01_atbm_ap bf01-preview; do
    [ -f "$SOURCE/$FILE" ] || { echo "Missing $SOURCE/$FILE; build and install IPC first" >&2; exit 1; }
done
if [ -n "$AP_CONFIG" ]; then
    [ -r "$AP_CONFIG" ] || { echo 'Cannot read AP config' >&2; exit 1; }
    awk 'NR == 1 { if (length < 1 || length > 32) exit 1 }
         NR == 2 { if (length < 8 || length > 63) exit 1 }
         NR == 3 { if ($0 !~ /^([1-9]|1[0-3])$/) exit 1 }
         END { if (NR != 3) exit 1 }' "$AP_CONFIG" || { echo 'Invalid AP config' >&2; exit 1; }
fi
DEST="$ROOTFS/mnt/system/bf01-preview"
install -d -m 755 "$DEST" "$ROOTFS/usr/bin"
install -m 755 "$SOURCE/ipcamera" "$SOURCE/bf01_atbm_ap" "$SOURCE/bf01-preview" "$DEST/"
install -m 644 "$SOURCE/param_config.ini" "$DEST/param_config.ini"
if [ -n "$AP_CONFIG" ]; then install -m 600 "$AP_CONFIG" "$DEST/ap.conf"; fi
printf '%s\n' "$BOOT_MODE" > "$DEST/boot.mode"
install -m 755 "$RESOURCE/S99zpreview" "$ROOTFS/etc/init.d/S99zpreview"
ln -sfn /mnt/system/bf01-preview/bf01-preview "$ROOTFS/usr/bin/bf01-preview"
echo "Staged preview under $DEST; factory boot=$BOOT_MODE"
echo 'Existing S99user and auto.sh are unchanged. Run after rootfs-prepare; rerun after rootfs-clean.'
