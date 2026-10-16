#!/bin/sh
# KWin raises its compositor threads to real-time priority (SCHED_RR 1).
# Do not give kwin_wayland CAP_SYS_NICE for that: a file capability makes it
# a secure-execution (AT_SECURE) process, and musl's issetugid() then makes
# libdbus ignore DBUS_SESSION_BUS_ADDRESS. KWin loses its session bus, and
# with it org.kde.KWin and global shortcuts such as the volume keys.
# The user session gets RLIMIT_RTPRIO 1 instead (user@.service drop-in), so
# KWin still gains real-time priority. Device r23-r36 set the capability;
# remove it at boot and from an apk trigger after packages change /usr/bin.
k=/usr/bin/kwin_wayland

[ -x "$k" ] || exit 0
command -v setcap >/dev/null 2>&1 || exit 0
getcap "$k" 2>/dev/null | grep -q cap_sys_nice || exit 0
setcap -r "$k" ||
	echo "gta3xlwifi: could not drop the file capabilities of $k" >&2
exit 0
