#!/bin/bash
HOST=$1
CARRIER=$2
BIT_PERIODS=$3

RATE=90

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

./apc_ups_logger -b -r "$RATE" > "channel_quality_ups-${MACHINE_NAME}-${CARRIER}hz_${BIT_PERIODS}ppb.dat" &
RECEIVER_PID=$!

sleep 6

if [ "$HOST" = 'localhost' ]; then
	./send_idx_packets.py "$CARRIER" "$BIT_PERIODS"
else
	ssh "$HOST" sudo stdbuf -oL ./send_idx_packets.py "$CARRIER" "$BIT_PERIODS"
fi

sleep 10

kill "$RECEIVER_PID"
wait "$RECEIVER_PID" 2> /dev/null

sox -r "$RATE" -b 32 -e float -t raw "channel_quality_ups-${MACHINE_NAME}-${CARRIER}hz_${BIT_PERIODS}ppb.dat" -t wav "channel_quality_ups-${MACHINE_NAME}-${CARRIER}hz_${BIT_PERIODS}ppb.wav" rate 2000 stat
