#!/bin/sh
AUDIODEV=hw:1,0

if [ -z "$1" ]; then
	echo "Please provide name of device under test as argument"
	exit 1
fi
NAME="$1"

export AUDIODEV
rec -c 1 -r 48000 -p | sox -p "tunes-$NAME.wav" rate 2000 highpass 200 highpass 200

# NOTE: generate spectrogram in seperate command, else it gets truncated at 8 sec.
sox "tunes-$NAME.wav" -n spectrogram -t "Sweep on $NAME" -o "tunes-spectrogram-$NAME.png" -c ''
