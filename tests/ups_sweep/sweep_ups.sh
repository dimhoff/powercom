#!/bin/bash
printf '\xff' > /tmp/sweep_test_data.dat

for (( i=1; i < 45; i+=1 )); do
	echo $i Hz
	sudo ./powercom_send -c $i -p $i -M ask -E none -f /tmp/sweep_test_data.dat
done

rm /tmp/sweep_test_data.dat
