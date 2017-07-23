#!/bin/bash
PCOM_DIR=../../

CSV_OUTPUT="NO"
POWER_FREQ=50

while [ "${1:0:1}" = "-" ]; do
	case "$1" in
	'-c')
		CSV_OUTPUT="YES"
		;;
	'-u')
		POWER_FREQ="0"
		;;
	*)
		echo "Unknown option '$1'"
		;;
	esac
	shift
done

WAV_FILE=$1
if [ ! -f "$WAV_FILE" ]; then
	echo "usage: $0 <input.wav> [<threshold>]"
	exit 1
fi

MACHINE_NAME=`basename "$WAV_FILE" | cut -d '-' -f 2 | tr '_' ' '`
CARRIER_FREQ=`basename "$WAV_FILE" | cut -d '-' -f 3 | sed -nr 's/^([[:digit:]]+)hz_([[:digit:]]+)ppb.*/\1/p'`
PERIODS_PER_BIT=`basename "$WAV_FILE" | cut -d '-' -f 3 | sed -nr 's/^([[:digit:]]+)hz_([[:digit:]]+)ppb.*/\2/p'`

if [ -z "$MACHINE_NAME" -o -z "$CARRIER_FREQ" -o -z "$CARRIER_FREQ" ]; then
	echo "Unable to determine parameters from file name"
	exit 1
fi

THRESHOLD=0
if [ -n "$2" ]; then
	THRESHOLD="$2"
fi

TMPDIR=`mktemp -d`

sox "$WAV_FILE" -b 32 -e float -t raw - 2> "$TMPDIR"/sox_error.txt | nc -l -q 1 12345 &
SOX_PID=$!

stdbuf -oL "$PCOM_DIR"/powercom_decoder_packet.py -t > "$TMPDIR"/decoder.txt &
PCOM_DECODER_PID=$!

sleep 0.1
"$PCOM_DIR"/powercom_demod_bpsk.py -s localhost -p "$PERIODS_PER_BIT" -c "$CARRIER_FREQ" -t "$THRESHOLD" -f "$POWER_FREQ"  > /dev/null &
PCOM_DEMOD_PID=$!

wait "$SOX_PID"

sleep 1

kill "$PCOM_DEMOD_PID"
wait "$PCOM_DEMOD_PID" 2>/dev/null

sleep 1

kill "$PCOM_DECODER_PID"
wait "$PCOM_DECODER_PID" 2>/dev/null

# Compare result to expected result
for (( i=0; i < 256; i++ )); do
	echo $i
done >> "$TMPDIR"/expected.txt

diff -a -d "$TMPDIR"/expected.txt "$TMPDIR"/decoder.txt > "/tmp/analyses_result.txt"

# Calculate packet loss/corruption
LOST=`cat /tmp/analyses_result.txt | grep '^<' | wc -l`
CORRUPT=`cat /tmp/analyses_result.txt | grep '^>' | wc -l`

if [ "$CORRUPT" -lt 2 ]; then
	echo "Missing connect and/or disconnect lines!!!!!"
fi

CORRUPT=$((CORRUPT - 2))
LOST=$((LOST - CORRUPT))

# Get clipping count
CLIPPED_SAMPLES=`sed -nr 's/^.* clipped ([[:digit:]]+) samples.*$/\1/p' "$TMPDIR"/sox_error.txt`
if [ -z "$CLIPPED_SAMPLES" ]; then
	CLIPPED_SAMPLES=0
fi

# Report
BAUD=$((CARRIER_FREQ / PERIODS_PER_BIT))
if [ "$CSV_OUTPUT" == "YES" ]; then
	echo "$MACHINE_NAME,$CARRIER_FREQ,$BAUD,$THRESHOLD,$CLIPPED_SAMPLES,$LOST,$CORRUPT"
else
	echo "DUT = $MACHINE_NAME"
	echo "Carrier = $CARRIER_FREQ"
	echo "Baud rate = $BAUD"
	echo "Threshold = $THRESHOLD"
	echo "Clipped samples = $CLIPPED_SAMPLES"
	echo "Packets lost = $LOST"
	echo "Packets corrupted = $CORRUPT"
fi

rm -fr "$TMPDIR"
