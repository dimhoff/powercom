#!/bin/bash
printf '\xff' > /tmp/sweep_test_data.dat

for (( i=10; i < 1000; i+=10 )); do
	echo $i Hz
	sudo ./powercom_send -c $i -p $((i/10)) -M ask -E none -f /tmp/sweep_test_data.dat
done

rm /tmp/sweep_test_data.dat
