#!/bin/bash
# Script to reserve all but the main core for powercom process
cd /sys/fs/cgroup/cpuset

mkdir sys                                   # create sub-cpuset for system processes
/bin/echo 0 > sys/cpuset.cpus               # assign core 0 for system processes
cat cpuset.mems > sys/cpuset.mems           # assign memory for system processes

mkdir powercom                              # create sub-cpuset for powercom
cat cpuset.cpus > powercom/cpuset.cpus      # assign all cores for powercom
cat cpuset.mems > powercom/cpuset.mems      # assign memory for powercom

# move all processes from the default cpuset to the sys-cpuset (may generate error messages)
for T in `cat tasks`; do echo "Moving " $T; /bin/echo $T > sys/tasks; done

powercom_pid=`pgrep powercom`
if [ -z "$powercom_pid" ]; then
	echo "Powercom not running. Add manualy to powercom  cpuset"
else
	/bin/echo "$powercom_pid" > powercom/tasks
fi
