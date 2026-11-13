#!/system/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# The virtual Wi-Fi station sees one open network, VirtWifi. Saving it once
# lets the platform join it whenever the user's Wi-Fi switch is on. This neither
# turns Wi-Fi on nor connects: the switch, and forgetting the network, stay the
# user's.
#
# The marker is 1 once a boot has saved the network and 2 once the station has
# joined it. Only 2 ends the script's work: at 2 a forgotten network stays
# forgotten and the script asks the Wi-Fi service nothing, because saving
# replaces the network's settings (metered, auto-connect), which the user may
# have chosen since. Until a join was seen the saved list decides, on every
# boot: a listed network is left alone and one that is not listed is saved.
# The marker cannot decide, because the Wi-Fi service writes its config store
# some time after a save: a guest that stops in between keeps marker 1 and
# loses the network. Two consequences are accepted. A network the user forgot
# before it ever joined is saved again, at each boot until it first joins. And
# data of an earlier release that holds marker 1 and no network (stopped before
# the store wrote it, or forgotten, where 1 was the only value) gets the network
# back on its first boot of this release; the script logs it when it does.
#
# A listing that fails (the Wi-Fi service busy) prints the same as an empty one.
# At marker 1 the network may be stored and the user may have set it metered
# after a join this script never saw, so such a boot saves it only when
# $looks looks, five seconds apart, all miss it: saving a stored network again
# would reset those settings. A boot with no marker has nothing to lose and
# saves at once; one that cannot save (the Wi-Fi service is not up) leaves the
# marker unset, and the next boot tries again.
#
# The platform no longer disables the network for lack of internet
# (rro/KernariumWifiOverlay), but a data disk written by an earlier release can
# still hold it disabled, and a disabled network is never joined again by
# itself. So until the marker is 2, a boot gives the station $settle seconds to
# join. If Wi-Fi is on and nothing has joined by then, saving the network
# again enables it (or puts it back, if the service dropped it) and the
# platform's own scan joins it; the script waits up to $rejoin seconds for that
# join, so the same boot ends at 2. Only a join the script sees sets 2. Wi-Fi
# that is off leaves the marker alone, so the next boot with Wi-Fi on still
# repairs.

marker=persist.sys.kernarium.virtwifi_saved
settle=45
rejoin=180
looks=3

# Sets $state: saved when the listing holds VirtWifi (one line: id, name,
# security type), absent when it does not.
look() {
    if cmd wifi list-networks 2>/dev/null | grep -Eq '^[0-9]+ +VirtWifi +open'; then
        state=saved
    else
        state=absent
    fi
}

# Succeeds when each of $looks looks finds the network absent.
missing() {
    seen=0
    while [ "$seen" -lt "$looks" ]; do
        look
        [ "$state" = saved ] && return 1
        seen=$((seen + 1))
        [ "$seen" -lt "$looks" ] && sleep 5
    done
    return 0
}

# Saves the network until the listing holds it; fails after 30 tries.
save() {
    tries=0
    while [ "$tries" -lt 30 ]; do
        # Saving the same network again updates it, so a retry adds no duplicate.
        cmd wifi add-network VirtWifi open >/dev/null 2>&1
        look
        [ "$state" = saved ] && return 0
        tries=$((tries + 1))
        sleep 2
    done
    return 1
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

current=$(getprop $marker)
[ "$current" = 2 ] && exit 0

look
if [ "$state" = absent ] && { [ -z "$current" ] || missing; }; then
    save || exit 1
    [ -z "$current" ] || log -t kernarium_wifi_network "VirtWifi was not stored: saved again" 2>/dev/null
fi
[ "$current" = 1 ] || setprop "$marker" 1

if wait_joined "$settle"; then
    setprop "$marker" 2
    exit 0
fi
# Wi-Fi off, or its service not up: nothing to repair on this boot.
case "$status" in
    *"Wifi is enabled"*) ;;
    *) exit 0 ;;
esac
cmd wifi add-network VirtWifi open >/dev/null 2>&1
log -t kernarium_wifi_network "VirtWifi saved again: Wi-Fi is on and nothing joined" 2>/dev/null
wait_joined "$rejoin" || exit 0
setprop "$marker" 2
exit 0
