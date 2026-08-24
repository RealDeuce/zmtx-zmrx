#!/bin/sh

set -eu

make_command=${MAKE:-make}
stage=$(mktemp -d "${TMPDIR:-/tmp}/zmtx-install.XXXXXX")
trap 'rm -rf "$stage"' EXIT HUP INT TERM

prefix=/opt/zmtx-test
install_root=$stage$prefix

"$make_command" DESTDIR="$stage" prefix="$prefix" install

check_mode()
{
	path=$1
	mode=$2

	if [ "$(find "$path" -prune -type f -perm "$mode" -print)" != "$path" ]; then
		echo "$path does not have mode $mode" >&2
		exit 1
	fi
}

check_mode "$install_root/bin/zmtx" 0555
check_mode "$install_root/bin/zmrx" 0555
check_mode "$install_root/share/man/man1/zmtx.1" 0444
check_mode "$install_root/share/man/man1/zmrx.1" 0444

"$make_command" DESTDIR="$stage" prefix="$prefix" uninstall

if [ -n "$(find "$stage" -type f -print)" ]; then
	echo "uninstall left installed files behind:" >&2
	find "$stage" -type f -print >&2
	exit 1
fi
