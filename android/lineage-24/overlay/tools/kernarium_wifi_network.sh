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
# The marker is 1 once the network is saved and 2 once the station has joined
# it. The platform no longer disables the network for lack of internet
# (rro/KernariumWifiOverlay), but a data disk written by an earlier release can
# still hold it disabled, and a disabled network is never joined again by
# itself. So until the marker is 2, a boot gives the station $settle seconds to
# join. If Wi-Fi is on, the network is still saved and nothing has joined by
# then, saving it again enables it and the platform's own scan joins it; the
# script waits up to $rejoin seconds for that join, so the same boot ends at 2.
# Only a join the script sees sets 2. A forgotten network is not saved again
# and leaves the marker as it is, because a listing that fails (the Wi-Fi
# service busy) prints the same as one without the network. Wi-Fi that is off
# leaves the marker alone, so the next boot with Wi-Fi on still repairs. At 2
# the script does nothing more: saving replaces the network's settings
# (metered, auto-connect), which the user may have chosen since.

marker=persist.sys.kernarium.virtwifi_saved
settle=45
rejoin=180

# One line of the listing: id, name, security type.
saved() {
    cmd wifi list-networks 2>/dev/null | grep -Eq '^[0-9]+ +VirtWifi +open'
}

# Waits up to $1 seconds for the station to join; $status keeps the last answer.
wait_joined() {
    waited=0
    while :; do
        status=$(cmd wifi status 2>/dev/null)
        case "$status" in
            *"Wifi is connected to"*) return 0 ;;
        esac
        [ "$waited" -ge "$1" ] && return 1
        sleep 5
        waited=$((waited + 5))
    done
}

case "$(getprop $marker)" in
    1|2) ;;
    *)
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
        ;;
esac
[ "$(getprop $marker)" = 2 ] && exit 0

if wait_joined "$settle"; then
    setprop "$marker" 2
    exit 0
fi
# Wi-Fi off, or its service not up: nothing to repair on this boot.
case "$status" in
    *"Wifi is enabled"*) ;;
    *) exit 0 ;;
esac
saved || exit 0
cmd wifi add-network VirtWifi open >/dev/null 2>&1
log -t kernarium_wifi_network "VirtWifi saved again: Wi-Fi is on and nothing joined" 2>/dev/null
wait_joined "$rejoin" || exit 0
setprop "$marker" 2
exit 0
