#!/bin/sh
AUDIODEV=hw:1,0
DURATION=100

if [ -z "$1" ]; then
	echo "Please provide name of device under test as argument"
	exit 1
fi
NAME="$1"

export AUDIODEV
rec -c 1 -r 48000 -p trim 0 "$DURATION" | sox -p "sweep-$NAME.wav" rate 2000

# NOTE: generate spectrogram in seperate command, else it gets truncated at 8 sec.
sox "sweep-$NAME.wav" -n spectrogram -t "Sweep on $NAME" -o "sweep-spectrogram-$NAME.png" -c ''
