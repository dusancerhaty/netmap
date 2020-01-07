#!/bin/bash

function action() {
	local verbose=false
	local output=""

	if [ "$1" = "-v" ]; then
		verbose=true
		shift
	fi
	echo -n "$@ ... "
	output="$($@)"
	if [ $? -ne 0 ]; then
		echo "FAIL"
		echo "$output"
		exit 1
	fi
	echo "OK"
	if [ "$verbose" = "true" ] && ! [ -z "$output" ] ; then
		echo "$output"
	fi
}

action $1 make

action $1 make install

action $1 modprobe -r igbnetmap

action $1 modprobe igbnetmap

action $1 ~/workspace/prepareIfaces.sh eth1,eth2,eth3,eth4 1 igb false 4096
