#!/system/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# The virtual Wi-Fi station sees one open network, VirtWifi. Saving it once
# lets the platform join it whenever the user's Wi-Fi switch is on. This neither
# turns Wi-Fi on nor connects: the switch, and forgetting the network, stay the
# user's. The marker keeps a forgotten network forgotten. A boot that cannot
# save the network (the Wi-Fi service is not up) leaves the marker unset, and
# the next boot tries again.
#
# Every boot then gives the station $settle seconds to join. The platform no
# longer disables the network for lack of internet (rro/KernariumWifiOverlay),
# but a data disk written by an earlier release can still hold it disabled, and
# a disabled network is never joined again by itself. If Wi-Fi is on, the
# network is still saved and nothing is joined by then, saving it again enables
# it and the platform's own scan joins it. The repair does not connect, so no
# later choice of the user's has anything to undo. A forgotten network is not
# saved again and Wi-Fi that is off stays off. Saving replaces the network's
# settings (metered, auto-connect), so nothing is saved unless the station has
# failed to join.

marker=persist.sys.kernarium.virtwifi_saved
settle=45

saved() {
    cmd wifi list-networks 2>/dev/null | grep -q VirtWifi
}

if [ "$(getprop $marker)" != 1 ]; then
    tries=0
    while [ "$tries" -lt 30 ]; do
        # Saving the same network again updates it, so a retry adds no duplicate.
        cmd wifi add-network VirtWifi open >/dev/null 2>&1
        if saved; then
            setprop "$marker" 1
            break
        fi
        tries=$((tries + 1))
        sleep 2
    done
    [ "$(getprop $marker)" = 1 ] || exit 1
fi

waited=0
while :; do
    status=$(cmd wifi status 2>/dev/null)
    case "$status" in
        *"Wifi is connected to"*) exit 0 ;;
    esac
    [ "$waited" -ge "$settle" ] && break
    sleep 5
    waited=$((waited + 5))
done

# Wi-Fi off, or its service not up: nothing to repair on this boot.
case "$status" in
    *"Wifi is enabled"*) ;;
    *) exit 0 ;;
esac
saved || exit 0
cmd wifi add-network VirtWifi open >/dev/null 2>&1
log -t kernarium_wifi_network "VirtWifi saved again: Wi-Fi is on and nothing joined" 2>/dev/null
exit 0
