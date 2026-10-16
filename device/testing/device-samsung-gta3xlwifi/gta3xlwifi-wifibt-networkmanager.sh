#!/bin/sh
# Start Samsung WLBT and configure Wi-Fi interfaces for NetworkManager.
# Usage: wifibt-networkmanager start

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
	local i state scan_result firmware_status pmu_state ctrl_ns ctrl_s wifi_status bt_controllers

	ebegin "Starting Samsung Wi-Fi and refreshing NetworkManager"
	if [ -e /sys/class/net/p2p0 ]; then
		nmcli device set p2p0 managed no || {
			eend 1 "could not leave the P2P interface unmanaged"
			return 1
		}
	fi
	if [ ! -e /sys/class/net/wlan0 ]; then
		eend 1 "wlan0 did not register"
		return 1
	fi
	if [ ! -r "$dev/state" ] || [ ! -r "$dev/firmware_status" ] ||
	   [ ! -r "$dev/pmu_state" ]; then
		eend 1 "WLBT start status is unavailable"
		return 1
	fi

	state=$(cat "$dev/state") || state=
	if [ "$state" = inert ]; then
		firmware_status=$(cat "$dev/firmware_status") || firmware_status=
		case "$firmware_status" in
			*"staged=1 memory_ready=1 config_ready=1"*"executed=0"*) ;;
			*)
				eend 1 "firmware is not fully prepared; leaving WLBT start gated"
				return 1
				;;
		esac

		pmu_state=$(cat "$dev/pmu_state") || pmu_state=
		ctrl_ns=$(printf '%s\n' "$pmu_state" | awk '$1 == "0140:" { print $2 }')
		ctrl_s=$(printf '%s\n' "$pmu_state" | awk '$1 == "0144:" { print $2 }')
		wifi_status=$(printf '%s\n' "$pmu_state" | awk '$1 == "0148:" { print $2 }')
		# The driver prepares the voltage and releases WLBT itself when
		# wlan0 opens; it must start from the powered-off state that
		# wifibt leaves (PWRON and START clear, no WLBT status).
		if [ -z "$ctrl_ns" ] || [ -z "$ctrl_s" ] || [ -z "$wifi_status" ] ||
		   [ $((0x$ctrl_ns & 0x2)) -ne 0 ] ||
		   [ $((0x$ctrl_s & 0x8)) -ne 0 ] ||
		   [ $((0x$wifi_status)) -ne 0 ]; then
			eend 1 "WLBT is not powered off; leaving start gated"
			return 1
		fi

		# Let pending rfkill events (systemd-rfkill restores) finish first.
		udevadm settle --timeout=5 >/dev/null 2>&1 || true
		# Unblocking Bluetooth registers hci0, which opens it at once.
		# Restoring the saved rfkill state may have done so before the
		# start was armed; that open failed and BlueZ never sees hci0.
		# Drop it now and register it again once the firmware runs.
		rfkill block bluetooth || {
			eend 1 "could not close the Bluetooth controller before firmware start"
			return 1
		}
		for i in 1 2 3 4 5 6 7 8 9 10; do
			[ -e /sys/class/bluetooth/hci0 ] || break
			sleep 0.2
		done

		nmcli device set wlan0 managed no || {
			eend 1 "could not release wlan0 before firmware start"
			return 1
		}
		ip link set wlan0 down || {
			nmcli device set wlan0 managed yes >/dev/null 2>&1 || true
			eend 1 "could not keep wlan0 down before firmware start"
			return 1
		}
		printf '1\n' > "$dev/arm_start" || {
			nmcli device set wlan0 managed yes >/dev/null 2>&1 || true
			eend 1 "one-shot WLBT start could not be armed"
			return 1
		}
		ip link set wlan0 up || {
			nmcli device set wlan0 managed yes >/dev/null 2>&1 || true
			eend 1 "could not open wlan0 to start WLBT firmware"
			return 1
		}
		for i in 1 2 3 4 5 6 7 8 9 10; do
			state=$(cat "$dev/state") || state=
			[ "$state" = running-or-starting ] && break
			sleep 0.2
		done
	fi
	if [ "$state" != running-or-starting ]; then
		nmcli device set wlan0 managed yes >/dev/null 2>&1 || true
		eend 1 "WLBT firmware did not start"
		return 1
	fi
	nmcli device set wlan0 managed yes || {
		eend 1 "could not hand wlan0 back to NetworkManager"
		return 1
	}

	rfkill unblock bluetooth || {
		eend 1 "could not unblock the Samsung Bluetooth controller"
		return 1
	}
	for i in 1 2 3 4 5 6 7 8 9 10; do
		[ -e /sys/class/bluetooth/hci0 ] && break
		sleep 0.2
	done
	if [ ! -e /sys/class/bluetooth/hci0 ]; then
		eend 1 "Bluetooth controller did not register after rfkill unblock"
		return 1
	fi
	for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
		bt_controllers=$(bluetoothctl list 2>/dev/null) || bt_controllers=
		printf '%s\n' "$bt_controllers" | grep -q '^Controller ' && break
		sleep 0.25
	done
	if ! printf '%s\n' "$bt_controllers" | grep -q '^Controller '; then
		eend 1 "BlueZ did not discover the Samsung Bluetooth controller"
		return 1
	fi
	bluetoothctl power on || {
		eend 1 "BlueZ could not power on the Samsung Bluetooth controller"
		return 1
	}

	for i in 1 2 3 4 5 6 7 8 9 10; do
		state=$(nmcli -g GENERAL.STATE device show wlan0 2>/dev/null) || state=
		case "$state" in
			*disconnected*) break ;;
		esac
		sleep 0.2
	done

	nmcli device wifi rescan ifname wlan0 || {
		eend 1 "NetworkManager could not scan wlan0"
		return 1
	}
	for i in 1 2 3 4 5 6 7 8 9 10; do
		scan_result=$(nmcli -t -f SSID device wifi list ifname wlan0 2>/dev/null) || scan_result=
		[ -n "$scan_result" ] && break
		sleep 0.5
	done
	eend 0
}

case "$1" in
	start) start ;;
	*) echo "usage: $0 start" >&2; exit 2 ;;
esac
