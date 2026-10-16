#!/bin/sh
# Helium (official glibc build) on musl through gcompat. Its binaries load
# /usr/lib/helium/libhelium-compat.so themselves; see the APKBUILD.
export CHROME_VERSION_EXTRA="postmarketOS"
export CHROME_WRAPPER="/usr/bin/helium"
exec /usr/lib/helium/helium --ozone-platform-hint=auto "$@"
