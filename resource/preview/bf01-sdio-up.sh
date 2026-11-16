#!/bin/sh

set -eu
umask 077
BASE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
RUN=/run/atbm_wifi
ACTION=${1:-network}
case "$ACTION" in up|network|network-start|status) ;; *) exit 2 ;; esac
[ "$(id -u)" = 0 ] || exit 1
mkdir -p "$RUN"
chmod 700 "$RUN"
exec 9>"$RUN/control.lock"
flock -n 9 || { echo 'Wi-Fi controller busy' >&2; exit 1; }

log_stage()
{
    read -r STAGE_UPTIME STAGE_REST < /proc/uptime
    echo "[wifi-sdio uptime=${STAGE_UPTIME}s] $*"
}

check_shutdown()
{
    [ ! -e /run/bf01-poweroff/requested ] || {
        log_stage 'Poweroff requested; SDIO startup cancelled'
        exit 1
    }
}

check_shutdown

find_sdio()
{
    for CARD in /sys/bus/sdio/devices/mmc1:*:1; do
        [ -r "$CARD/vendor" ] || continue
        [ "$(cat "$CARD/vendor")" = 0x007a ] || continue
        [ "$(cat "$CARD/device")" = 0x6011 ] || continue
        return 0
    done
    return 1
}

COUNT=0
until find_sdio; do
    check_shutdown
    [ "$ACTION" != status ] || exit 1
    [ ! -d /sys/module/atbm6461_wifi_sdio ] || {
        log_stage 'Driver loaded without matching SDIO function; leaving driver untouched'
        exit 1
    }
    [ "$COUNT" -lt 60 ] || { log_stage 'Timed out waiting for SDIO function 007a:6011'; exit 1; }
    [ "$COUNT" -ne 0 ] || log_stage 'Waiting for SDIO function 007a:6011; no fixed startup delay'
    if [ -w /sys/class/mmc_host/mmc1/rescan ] && [ "$((COUNT % 2))" -eq 0 ]; then
        echo 1 > /sys/class/mmc_host/mmc1/rescan
    fi
    sleep 1
    COUNT=$((COUNT + 1))
done
log_stage 'SDIO function 007a:6011 enumerated'
if [ ! -d /sys/module/cfg80211 ]; then
    check_shutdown
    [ "$ACTION" != status ] || exit 1
    insmod "$BASE/cfg80211.ko"
fi
if [ ! -d /sys/module/atbm6461_wifi_sdio ]; then
    check_shutdown
    [ "$ACTION" != status ] || exit 1
    log_stage 'Loading SDIO driver'
    insmod "$BASE/atbm6461_wifi_sdio.ko"
fi
[ -d "$CARD/driver" ] || {
    log_stage "SDIO probe failed or has not bound: $CARD; module loaded is not driver ready, inspect dmesg WSM startup; no automatic rebind/reload"
    exit 1
}
COUNT=0
if [ -r "$CARD/bf01_startup_state" ]; then
    while :; do
        check_shutdown
        read -r DRIVER_STATE DRIVER_DETAILS < "$CARD/bf01_startup_state" || {
            log_stage 'Cannot read driver handshake state'
            exit 1
        }
        case "$DRIVER_STATE" in
            ready) log_stage "Real WSM handshake ready: $DRIVER_DETAILS"; break ;;
            retry|handshaking)
                [ "$COUNT" -ne 0 ] || log_stage "Driver loaded; background handshake $DRIVER_STATE: $DRIVER_DETAILS"
                [ "$ACTION" != status ] || exit 1
                [ "$COUNT" -lt 60 ] || { log_stage 'Driver still retrying handshake; leaving worker active for next startup check'; exit 1; }
                sleep 1
                COUNT=$((COUNT + 1))
                ;;
            *) log_stage "Driver handshake unavailable: $DRIVER_STATE $DRIVER_DETAILS"; exit 1 ;;
        esac
    done
fi
[ -c /dev/atbm_ioctl ] && [ -d /sys/class/net/wlan0 ] || {
    log_stage 'Driver interface not ready; no automatic driver reload'
    exit 1
}
check_shutdown
timeout 8 "$BASE/atbm_wifi_ctl" status
log_stage 'Driver control interface ready'
case "$ACTION" in network|network-start) ;; *) exit 0 ;; esac
if pidof atbm_iot_supplicant >/dev/null 2>&1; then
    echo 'Refusing to change a connection owned by another supplicant' >&2
    exit 1
fi

WATCH_PID=
if [ -r "$RUN/watch.pid" ]; then
    read -r WATCH_PID < "$RUN/watch.pid" || WATCH_PID=
fi
case "$WATCH_PID" in ''|*[!0-9]*) WATCH_PID= ;; esac
if [ -n "$WATCH_PID" ] && kill -0 "$WATCH_PID" 2>/dev/null; then
    tr '\000' '\n' < "/proc/$WATCH_PID/cmdline" | grep -Fxq "$BASE/atbm_wifi_ctl" || {
        echo 'Existing watcher has a different owner; leaving it untouched' >&2
        exit 1
    }
else
    if [ -r "$RUN/watcher.ready" ]; then
        read -r OWNER < "$RUN/watcher.ready" || OWNER=
        case "$OWNER" in
            ''|*[!0-9]*) ;;
            *) if kill -0 "$OWNER" 2>/dev/null; then
                   echo 'Existing watcher is still running; leaving it untouched' >&2
                   exit 1
               fi ;;
        esac
    fi
    FLAGS=$(cat /sys/class/net/wlan0/flags)
    [ "$((FLAGS & 1))" -ne 0 ] || ifconfig wlan0 up
    if [ ! -f /run/resolv.conf ]; then
        cp /etc/resolv.conf /run/resolv.conf
    fi
    if [ ! -L /etc/resolv.conf ] &&
       ! awk '$2 == "/etc/resolv.conf" { found=1 } END { exit !found }' /proc/mounts; then
        mount -o bind /run/resolv.conf /etc/resolv.conf
    fi
    rm -f "$RUN/watcher.ready" "$RUN/network.ready"
    check_shutdown
    nohup "$BASE/atbm_wifi_ctl" watch </dev/null >"$RUN/wifi.log" 2>&1 9>&- &
    WATCH_PID=$!
    printf '%s\n' "$WATCH_PID" > "$RUN/watch.pid"
fi
[ "$ACTION" != network-start ] || {
    COUNT=0
    while [ ! -f "$RUN/watcher.ready" ]; do
        check_shutdown
        kill -0 "$WATCH_PID" 2>/dev/null || { log_stage 'Network watcher exited before becoming ready'; exit 1; }
        [ "$COUNT" -lt 5 ] || { log_stage 'Network watcher did not become ready'; exit 1; }
        sleep 1
        COUNT=$((COUNT + 1))
    done
    log_stage "Network watcher active: $WATCH_PID; IP synchronization continues in background"
    exit 0
}
COUNT=0
while [ ! -f "$RUN/watcher.ready" ] || [ ! -f "$RUN/network.ready" ]; do
    check_shutdown
    kill -0 "$WATCH_PID" 2>/dev/null || exit 1
    [ "$COUNT" -lt 15 ] || { echo 'Module has no IP yet; watcher remains active' >&2; exit 1; }
    sleep 1
    COUNT=$((COUNT + 1))
done
log_stage 'Module-owned Wi-Fi: Linux address, route and DNS are synchronized.'
