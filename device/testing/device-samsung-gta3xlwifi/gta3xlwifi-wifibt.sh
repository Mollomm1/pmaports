#!/bin/sh
# Prepare and start Samsung Exynos WLBT.
# Usage: wifibt start|stop

# Status helpers with the OpenRC names, so the same steps run from the
# OpenRC script and the systemd unit.
ebegin() {
	echo "$*"
}

eend() {
	local ret=$1

	shift
	[ "$ret" -eq 0 ] || echo "ERROR: $*" >&2
	return "$ret"
}

start() {
	local dev=/sys/bus/platform/devices/120c0000.wifibt
	local acpm_attach=/sys/devices/platform/firmware:power-management/attach_fvp
	local action ctrl_ns ctrl_s i pmu_state state wifi_status

	ebegin "Preparing Samsung Wi-Fi and Bluetooth"
	modprobe exynos7885_acpm_mbox || {
		eend 1 "loading ACPM mailbox controller failed"
		return 1
	}
	modprobe acpm_protocol || {
		eend 1 "loading ACPM protocol failed"
		return 1
	}

	modprobe exynos_scsc_wifibt || {
		eend 1 "loading WLBT resource provider failed"
		return 1
	}

	for i in 1 2 3 4 5 6 7 8 9 10; do
		[ -d "$dev" ] && break
		sleep 0.1
	done
	if [ ! -d "$dev" ]; then
		eend 1 "WLBT device did not bind"
		return 1
	fi

	state=$(cat "$dev/state") || {
		eend 1 "could not read WLBT state"
		return 1
	}
	if [ "$state" != inert ]; then
		eend 1 "expected inert WLBT state, found $state"
		return 1
	fi

	pmu_state=$(cat "$dev/pmu_state") || {
		eend 1 "could not read bounded WLBT PMU state"
		return 1
	}
	ctrl_ns=$(printf '%s\n' "$pmu_state" | awk '$1 == "0140:" { print $2 }')
	ctrl_s=$(printf '%s\n' "$pmu_state" | awk '$1 == "0144:" { print $2 }')
	wifi_status=$(printf '%s\n' "$pmu_state" | awk '$1 == "0148:" { print $2 }')
	if [ -z "$ctrl_ns" ] || [ -z "$ctrl_s" ] || [ -z "$wifi_status" ]; then
		eend 1 "bounded WLBT PMU state was incomplete"
		return 1
	fi
	if [ $((0x$ctrl_ns & 0x2)) -ne 0 ] ||
	   [ $((0x$ctrl_s & 0x8)) -ne 0 ] ||
	   [ $((0x$wifi_status)) -ne 0 ]; then
		printf '1\n' > "$dev/quiesce_stale" || {
			eend 1 "stale WLBT state could not be safely quiesced"
			return 1
		}
	fi

	if [ ! -r /usr/lib/firmware/exynos7885_acpm_fvp.fw ]; then
		eend 1 "Exynos7885 ACPM FVP firmware is missing"
		return 1
	fi
	for i in 1 2 3 4 5 6 7 8 9 10; do
		[ -e "$acpm_attach" ] && break
		sleep 0.1
	done
	if [ ! -e "$acpm_attach" ]; then
		eend 1 "ACPM FVP attach control did not appear"
		return 1
	fi
	printf '1\n' > "$acpm_attach" || {
		eend 1 "attaching Exynos7885 ACPM FVP plugin failed"
		return 1
	}

	for action in stage_firmware prepare_memory prepare_config; do
		printf '1\n' > "$dev/$action" || {
			eend 1 "$action failed; WLBT firmware remains gated"
			return 1
		}
	done

	modprobe scsc_mx || {
		eend 1 "loading SCSC core failed"
		return 1
	}
	modprobe scsc_wlan || {
		eend 1 "loading SCSC WLAN failed"
		return 1
	}
	modprobe scsc_bt || {
		eend 1 "loading SCSC Bluetooth failed"
		return 1
	}

	for i in 1 2 3 4 5 6 7 8 9 10; do
		[ -e /sys/class/net/wlan0 ] && break
		sleep 0.1
	done
	if [ ! -e /sys/class/net/wlan0 ]; then
		eend 1 "WLAN interface did not register; WLBT firmware remains gated"
		return 1
	fi

	# The one-shot firmware start is armed by wifibt-networkmanager while
	# NetworkManager leaves wlan0 alone. Arming it here let NetworkManager
	# open wlan0, start the firmware and then take wlan0 down to change
	# its MAC, which stops the firmware for the rest of the boot.
	eend 0
}

stop() {
	local dev=/sys/bus/platform/devices/120c0000.wifibt
	local ctrl_ns ctrl_s i pmu_state state wifi_status

	ebegin "Stopping Samsung Wi-Fi and Bluetooth before shutdown"
	if [ -e /sys/class/net/wlan0 ]; then
		ip link set wlan0 down || {
			eend 1 "could not stop the WLAN interface"
			return 1
		}
	fi
	if [ -e /sys/class/net/p2p0 ]; then
		ip link set p2p0 down || {
			eend 1 "could not stop the P2P interface"
			return 1
		}
	fi
	rfkill block bluetooth || {
		eend 1 "could not close the Bluetooth controller"
		return 1
	}

	for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30; do
		state=$(cat "$dev/state" 2>/dev/null) || state=
		[ "$state" = inert ] && break
		sleep 0.1
	done
	if [ "$state" != inert ]; then
		eend 1 "WLBT did not quiesce; refusing to claim a clean radio shutdown"
		return 1
	fi

	# Reset-held is not power-off: leaving PWRON asserted lets the central
	# sequencer come back in its stale 0x2d state on a warm host reboot. Once
	# both clients are stopped and the MIF is unmapped, use the driver's
	# bounded stale-state path to power WLBT down before the PMU shuts down.
	pmu_state=$(cat "$dev/pmu_state") || {
		eend 1 "could not read bounded WLBT PMU state"
		return 1
	}
	ctrl_ns=$(printf '%s\n' "$pmu_state" | awk '$1 == "0140:" { print $2 }')
	ctrl_s=$(printf '%s\n' "$pmu_state" | awk '$1 == "0144:" { print $2 }')
	wifi_status=$(printf '%s\n' "$pmu_state" | awk '$1 == "0148:" { print $2 }')
	if [ -z "$ctrl_ns" ] || [ -z "$ctrl_s" ] || [ -z "$wifi_status" ]; then
		eend 1 "bounded WLBT PMU state was incomplete"
		return 1
	fi
	if [ $((0x$ctrl_ns & 0x2)) -ne 0 ] ||
	   [ $((0x$ctrl_s & 0x8)) -ne 0 ] ||
	   [ $((0x$wifi_status)) -ne 0 ]; then
		printf '1\n' > "$dev/quiesce_stale" || {
			eend 1 "WLBT did not reach powered-off state before shutdown"
			return 1
		}
	fi
	eend 0
}

case "$1" in
	start) start ;;
	stop) stop ;;
	*) echo "usage: $0 start|stop" >&2; exit 2 ;;
esac
