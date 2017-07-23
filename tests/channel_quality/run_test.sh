#!/bin/bash
HOST=$1
CARRIER=$2
BIT_PERIODS=$3

if [ -z "$HOST" -o -z "$CARRIER" -o -z "$BIT_PERIODS" ]; then
	echo "usage: $0 <host> <carrier> <periods per bit>"
	exit 1
fi

case "$HOST" in
'pcom-test')
	MACHINE_NAME='Custom_Desktop'
	;;
'vostro')
	MACHINE_NAME='Dell_Vostro_3550'
	;;
'd1950')
	MACHINE_NAME='Dell_Poweredge_1950'
	;;
'localhost')
	MACHINE_NAME='Intel_NUC7i5BNH'
	;;
*)
	echo "Invalid host name"
	exit 1
	;;
esac

ssh raspberrypi export AUDIODEV=hw:1,0 \; sox "'|rec -c 1 -r 48000 -p'" -b 32 -e float "'channel_quality-${MACHINE_NAME}-${CARRIER}hz_${BIT_PERIODS}ppb.wav'" rate 2000 &
SOX_PID=$!

sleep 6

if [ "$HOST" = 'localhost' ]; then
	./send_idx_packets.py "$CARRIER" "$BIT_PERIODS"
else
	ssh "$HOST" sudo stdbuf -oL ./send_idx_packets.py "$CARRIER" "$BIT_PERIODS"
fi

sleep 10

kill "$SOX_PID"
wait "$SOX_PID" 2> /dev/null
