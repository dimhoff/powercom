#!/bin/sh
DURATION=370
RATE=90

if [ -z "$1" ]; then
	echo "Please provide name of device under test as argument"
	exit 1
fi
NAME="$1"

./apc_ups_logger -b -r "$RATE" -t "$DURATION" > "sweep_ups-$NAME.dat"

sox -r "$RATE" -b 32 -e float -t raw "sweep_ups-$NAME.dat" -n spectrogram -t "Sweep on $NAME" -o "sweep_ups-spectrogram-$NAME.png" -c ''
