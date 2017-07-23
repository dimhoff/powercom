/**
 * apc_ups_logger.c - Log APC UPS load at constant interval
 *
 * Copyright (c) 2017 David Imhoff <dimhoff.devel@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#define _GNU_SOURCE 1
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <getopt.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>

#include <linux/hiddev.h>

#define SEC_AS_NSEC (1000 * 1000 * 1000)
#define DEFAULT_HIDDEV_PATH "/dev/usb/hiddev0"
#define DEFAULT_RATE 90

sig_atomic_t terminate = false;
sig_atomic_t timer_triggered = false;

void timer_cb(int sig)
{
	timer_triggered = true;
}

void terminate_cb(int sig)
{
	terminate = true;
}

void usage(const char *name)
{
	printf("Usage: %s [-brh] [hiddev path]\n", name);
	printf("\n");
	printf("Where:\n");
	printf(" -b           Binary (float32) output\n");
	printf(" -r <RATE>    Sampling rate in Hz (default: %d)\n", DEFAULT_RATE);
	printf(" -t <SEC>     Exit after SEC seconds\n");
	printf(" -h           Display this help message\n");
	printf("\n");
	printf("Default hiddev path: %s\n", DEFAULT_HIDDEV_PATH);
}

int main(int argc, char *argv[])
{
	const int report_type = 3;
	const int report_id = 44;

	bool cfg_binary = false;
	unsigned int cfg_rate = DEFAULT_RATE;
	const char *cfg_hiddev_path = DEFAULT_HIDDEV_PATH;

	time_t endtime = 0;

	int opt;
	char *endp;
	while ((opt = getopt(argc, argv, "br:t:h")) != -1) {
		switch (opt) {
		case 'b':
			cfg_binary = true;
			break;
		case 'r':
			cfg_rate = strtoul(optarg, &endp, 10);
			if (endp == optarg || *endp != '\0' || cfg_rate == 0) {
				fprintf(stderr, "Invalid argument to '-r' option\n");
				exit(EXIT_FAILURE);
			}
			break;
		case 't': {
			unsigned long int runtime = strtoul(optarg, &endp, 10);
			if (endp == optarg || *endp != '\0') {
				fprintf(stderr, "Invalid argument to '-t' option\n");
				exit(EXIT_FAILURE);
			}

			if (runtime != 0) {
				endtime = time(NULL) + runtime;
			}
			break;
		}
		case 'h':
			usage(argv[0]);
			exit(EXIT_SUCCESS);
			break;
		default: /* '?' */
			usage(argv[0]);
			exit(EXIT_FAILURE);
		}
	}

	if (argc - optind == 1) {
		cfg_hiddev_path = argv[optind];
	} else if (argc - optind != 0) {
		fprintf(stderr, "Incorrect amount of arguments\n");
		usage(argv[0]);
		exit(EXIT_FAILURE);
	}

	int fd;
	if ((fd = open(cfg_hiddev_path, O_RDWR)) == -1) {
		perror("open()");
		exit(EXIT_FAILURE);
	}

	// HIDIOCGREPORTINFO - struct hiddev_report_info (read/write)
	// Fills in a hiddev_report_info structure for the user. The report is
	// looked up by type (input, output or feature) and id, so these fields
	// must be filled in by the user. The ID can be absolute -- the actual
	// report id as reported by the device -- or relative --
	// HID_REPORT_ID_FIRST for the first report, and (HID_REPORT_ID_NEXT |
	// report_id) for the next report after report_id. Without a-priori
	// information about report ids, the right way to use this ioctl is to
	// use the relative IDs above to enumerate the valid IDs. The ioctl
	// returns non-zero when there is no more next ID. The real report ID is
	// filled into the returned hiddev_report_info structure.
	struct hiddev_report_info rinfo;
	rinfo.report_type = report_type;
	rinfo.report_id = report_id;
	if (ioctl(fd, HIDIOCGREPORTINFO, &rinfo) != 0) {
		perror("ioctl(HIDIOCGREPORTINFO)");
		exit(EXIT_FAILURE);
	}

	// HIDIOCGREPORT - struct hiddev_report_info (write)
	// Instructs the kernel to get a feature or input report from the device,
	// in order to selectively update the usage structures (in contrast to
	// INITREPORT).
	if (ioctl(fd, HIDIOCGREPORT, &rinfo) < 0) {
		perror("ioctl(HIDIOCGREPORT)");
		exit(EXIT_FAILURE);
	}

	// HIDIOCGUCODE - struct hiddev_usage_ref (read/write)
	// Returns the usage_code in a hiddev_usage_ref structure, given that
	// given its report type, report id, field index, and index within the
	// field have already been filled into the structure.
	struct hiddev_usage_ref uref;
	uref.report_type = report_type;
	uref.report_id = report_id;
	uref.field_index = 0;
	uref.usage_index = 0;
	if (ioctl(fd, HIDIOCGUCODE, &uref) < 0) {
		perror("ioctl(HIDIOCGUCODE)");
		exit(EXIT_FAILURE);
	}

	// HIDIOCGUSAGE - struct hiddev_usage_ref (read/write)
	// Returns the value of a usage in a hiddev_usage_ref structure. The
	// usage to be retrieved can be specified as above, or the user can
	// choose to fill in the report_type field and specify the report_id as
	// HID_REPORT_ID_UNKNOWN. In this case, the hiddev_usage_ref will be
	// filled in with the report and field information associated with this
	// usage if it is found.
	if (ioctl(fd, HIDIOCGUSAGE, &uref) < 0) {
		perror("ioctl(HIDIOCGUSAGE)");
		exit(EXIT_FAILURE);
	}

	// Setup interval timer
	timer_t timer;
	struct itimerspec alarm_time;

	signal(SIGALRM, &timer_cb);

	if (timer_create(CLOCK_REALTIME, NULL, &timer) == -1) {
		perror("timer_create");
		exit(EXIT_FAILURE);
	}

	if (cfg_rate > 1) {
		alarm_time.it_interval.tv_sec = 0;
		alarm_time.it_interval.tv_nsec = SEC_AS_NSEC / cfg_rate;
	} else {
		alarm_time.it_interval.tv_sec = 1;
		alarm_time.it_interval.tv_nsec = 0;
	}

	alarm_time.it_value = alarm_time.it_interval;

	// Start transmitting
	if (timer_settime(timer, 0, &alarm_time, NULL) == -1) {
		perror("timer_settime");
		exit(EXIT_FAILURE);
	}

	signal(SIGINT, &terminate_cb);

	int sample_cnt = 0;
	while (!terminate) {
		while (!timer_triggered) {
			sleep(10);
		}
		timer_triggered = false;
		sample_cnt++;

		if (ioctl(fd, HIDIOCGREPORT, &rinfo) < 0) {
			perror("ioctl(HIDIOCGREPORT)");
			exit(EXIT_FAILURE);
		}

		if (ioctl(fd, HIDIOCGUSAGE, &uref) < 0) {
			perror("ioctl(HIDIOCGUSAGE)");
			exit(EXIT_FAILURE);
		}

		if (cfg_binary) {
			float value = (float) uref.value / 1000.0;
			// Use scale from 0.0-1.0 to allow sox to handle data
			fwrite(&value, sizeof(value), 1, stdout);

			if (sample_cnt % cfg_rate == 0) {
				fflush(stdout);
			}
		} else {
			printf("load = %.2f %%\n", uref.value / 10.0);
		}

		if (endtime) {
			time_t now = time(NULL);
			if (now >= endtime) {
				terminate = true;
			}
		}

		if (timer_triggered) {
			fprintf(stderr, "WARNING: Can't keep up with rate\n");
		}
	}

	if (timer_delete(timer) == -1) {
		perror("timer_delete");
	}

	close(fd);
	return 0;
}
