#!/bin/bash
set -euo pipefail

SDK=${1:?SDK directory required}
OUTPUT=${2:?output directory required}
CROSS=${3:?cross compiler prefix required}
KERNEL=${BF01_KERNEL_BUILD:-$SDK/linux_5.10/build/cv1815ja_bf01_spinand}
SOURCE="$SDK/osdrv/extdrv/wireless/realtek/atbm6461"
test -s "$KERNEL/Module.symvers"
mkdir -p "$OUTPUT"
PRIVATE_SOURCE=$(mktemp -d "$OUTPUT/source.XXXXXX")
tar -C "$SOURCE" --exclude=.git --exclude=driver_install --exclude='*.o' \
    --exclude='*.ko' --exclude='*.mod*' --exclude='.*.cmd' \
    --exclude=Module.symvers --exclude=modules.order -cf - . | tar -C "$PRIVATE_SOURCE" -xf -
require_driver_feature()
{
    local source_file=$1
    local marker=$2
    local feature=$3
    if ! grep -Fq -- "$marker" "$PRIVATE_SOURCE/hal_apollo/$source_file"; then
        printf 'BF01 Wi-Fi: SDK driver is missing %s (%s).\n' "$feature" "$source_file" >&2
        printf 'Apply the latest bf01_bsp/patches/0042-osdrv--feat-wifi-add-bidirectional-host-integration.patch to the SDK before rebuilding.\n' >&2
        exit 1
    fi
}
require_driver_feature wsm.c 'static int wsm_direct_trans_confirm' 'bounded Direct Buffer replies'
require_driver_feature atbm_ioctl.c 'int atbm_bf01_host_indication(' 'unsolicited 0x083B receive support'
require_driver_feature sbus.h '#define ATBM_BF01_STARTUP_RETRY' 'background startup retry support'
require_driver_feature apollo_sdio.c 'static void atbm_bf01_startup_work(' 'background WSM handshake worker'
require_driver_feature main.c 'startup_deadline = startup_started + msecs_to_jiffies(1000)' '1000 ms WSM handshake timeout'
require_driver_feature apollo_sdio.c 'retry_in_ms=1000; driver remains bound' '1000 ms handshake retry policy'
require_driver_feature apollo_sdio.c 'static void atbm_bf01_startup_abort(' 'startup transport failure latch'
require_driver_feature main.c 'stop startup I/O before retry cleanup' 'startup I/O quiescence before retry cleanup'
make -C "$KERNEL" M="$PRIVATE_SOURCE" ARCH=arm CROSS_COMPILE="$CROSS" modules
install -m 644 "$PRIVATE_SOURCE/hal_apollo/atbm6461_wifi_sdio.ko" "$OUTPUT/"
printf 'Candidate only; no device deployment or SDIO startup changes: %s\n' "$OUTPUT/atbm6461_wifi_sdio.ko"
