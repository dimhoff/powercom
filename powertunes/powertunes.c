/**
 * powertunes.c - Generate Music on the power line!
 * 
 * Just a fun experiment, only works with Dell Vostro 3550.
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
#include <string.h>
#include <termios.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <sys/stat.h>
#include <stdatomic.h>

// includes needed for the timers
#include <signal.h>
#include <time.h>
#include <sys/types.h>

#include <pthread.h>

#include "notes.h"

//#define DEBUG_BITS 1

#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define USEC_AS_NSEC 1000
#define MSEC_AS_NSEC (1000 * 1000)
#define SEC_AS_NSEC (1000 * 1000 * 1000)
#define HZ_TO_NSEC(F) (SEC_AS_NSEC / F)

#define MAX_THREADS 32

// TODO:
// Should use ATOMIC_BOOL_LOCK_FREE instead of __GCC_ATOMIC_BOOL_LOCK_FREE, but
// definition is in gcc 4.9 doesn't allow it // to be used in preprocessor compares
#if __GCC_ATOMIC_BOOL_LOCK_FREE != 2
# error "No, atomic bool available"
#endif

static bool abort_transmit = false;
static bool terminate = false;

static int cfg_core_cnt = -1;

typedef struct {
	float pitch;
	unsigned int duration_ms;
} note_t;

struct {
	// Variable used to communicate back to main program
	sig_atomic_t have_lock;	// If main thread currently has a lock
	sig_atomic_t done;	// Note duration has passed

	int cnt;		// amount of period to play note
} state = { false };

/********************** Powercom transmission functions ***********************/
void send_terminate_cb(int sig)
{
	abort_transmit = true;
}

void timer_cb(int sig)
{
	if (state.cnt == 0) {
		state.have_lock = true;
		state.done = true;
	} else {
		state.have_lock = ! state.have_lock;
		state.cnt--;
	}
}

struct thread_state {
	pthread_mutex_t mutex;		// Mutex, if locked by master thread, child thread will be idle
	unsigned int id;		// number of thread
	atomic_bool *stop;		// If true the child thread should terminate
	const struct sched_param *sched_param;
};

void *helper_thread(void *arg)
{
	struct thread_state *s = (struct thread_state *) arg;

	// Set schedular
	// TODO: Set CPU affinity starting at the last core??
	cpu_set_t cpu_set;
	CPU_ZERO(&cpu_set);
	CPU_SET(s->id, &cpu_set);
	if (sched_setaffinity(0, sizeof(cpu_set_t), &cpu_set) != 0) {
		fprintf(stderr, "Failed to set cpu affinity for thread %d: %d\n", s->id, errno);
	}

	if (s->sched_param != NULL) {
		if (sched_setscheduler(0, SCHED_RR, s->sched_param) != 0) {
			fprintf(stderr, "Failed to set scheduler priority for thread %d: %d\n", s->id, errno);
		}
	}

	// Block signals
	sigset_t set;
	int ret;

	sigemptyset(&set);
	sigaddset(&set, SIGALRM);
	sigaddset(&set, SIGINT);
	ret = pthread_sigmask(SIG_BLOCK, &set, NULL);
	if (ret != 0) {
		fprintf(stderr, "Failed to block signals in helper "
			"thread %d: retval %d\n", s->id, ret);
	}

	// Main
	while (! atomic_load(s->stop)) {
#ifdef WITH_BARRIER
		// WARNING: This is black magic... threads get stuck on barrier
		// if order is incorrect. See cleanup of powercom_transmit()
		pthread_barrier_wait(s->barrier);
		while (atomic_load(s->generate_load) == true);
#else
		pthread_mutex_lock(&(s->mutex));
		pthread_mutex_unlock(&(s->mutex));
#endif
	}

	return NULL;
}

void play_notes(const note_t *notes, size_t note_cnt)
{
	int i;

	pthread_t threads[MAX_THREADS];
	struct thread_state tstates[MAX_THREADS];

	timer_t timer;

	struct timespec timer_interval;

	struct itimerspec alarm_time;

	atomic_bool stop_threads = false;

	// Init state
	state.have_lock = true;

	// Setup signal handlers
	sighandler_t old_sigalrm_handler = signal(SIGALRM, &timer_cb);
	sighandler_t old_sigint_handler = signal(SIGINT, &send_terminate_cb);

	// Create the timer
	if (timer_create(CLOCK_REALTIME, NULL, &timer) == -1) {
		perror("timer_create");
		exit(EXIT_FAILURE);
	}

	// Configure scheduler
	struct sched_param sched_param;
	struct sched_param *thread_sched_param = &sched_param;
	static bool suppress_scheduler_warning = false;
	sched_param.sched_priority = 6;
	if (sched_setscheduler(0, SCHED_RR, &sched_param) != 0) {
		if (!suppress_scheduler_warning) {
			fprintf(stderr, "Failed to set scheduler priority: %s\n", strerror(errno));
			suppress_scheduler_warning = true;
		}
		thread_sched_param = NULL;
	} else {
		sched_param.sched_priority -= 1; // Make sure load threads have lower prio
	}

	// Start helper threads
	for (i=0; i < cfg_core_cnt; i++) {
		tstates[i].id = i;
		tstates[i].stop = &stop_threads;
		tstates[i].sched_param = thread_sched_param;

		pthread_mutex_init(&(tstates[i].mutex), NULL);

		pthread_create(&(threads[i]), NULL, helper_thread, &(tstates[i]));
	}

	while (note_cnt > 0 && ! abort_transmit) {
		// Select note to play
		timer_interval.tv_sec  = 0;
		timer_interval.tv_nsec = HZ_TO_NSEC(notes->pitch) / 2;

		state.cnt = notes[0].duration_ms * 1000 * 1000 / timer_interval.tv_nsec;
		state.done = false;

		printf("ival: %ju, cnt: %ju\n", (uintmax_t) timer_interval.tv_nsec, (uintmax_t) state.cnt);
		// select next note
		notes++;
		note_cnt--;

		// Start transmitting
		alarm_time.it_value = timer_interval; 
		alarm_time.it_interval = timer_interval;

		if (timer_settime(timer, 0, &alarm_time, NULL) == -1) {
			perror("timer_settime");
			goto cleanup;
		}

		while (!state.done && !abort_transmit) {
			if (state.have_lock) {
				for (i=0; i < cfg_core_cnt; i++) {
					pthread_mutex_lock(&(tstates[i].mutex));
				}

				while (state.have_lock && !state.done && !abort_transmit) {
					sleep(10);
				}

				for (i=0; i < cfg_core_cnt; i++) {
					pthread_mutex_unlock(&(tstates[i].mutex));
				}
			} else {
				sleep(10);
			}
		}
	}

	// Clean up
	if (timer_delete(timer) == -1) {
		perror("timer_delete");
	}

cleanup:
	atomic_store(&stop_threads, true);

	for (i=0; i < cfg_core_cnt; i++) {
		//pthread_cancel(threads[i]); // Doesn't do much...
		pthread_join(threads[i], NULL);
	}

	signal(SIGALRM, old_sigalrm_handler);
	signal(SIGINT, old_sigint_handler);

	if (abort_transmit) {
		if (old_sigint_handler) {
			old_sigint_handler(SIGINT);
		} else {
			exit(0);
		}
	}
}

/************************* Main program ***************************************/
void terminate_cb(int sig)
{
	terminate = true;
}

void check_cpu_governor()
{
	FILE *fp;
	char buf[1024];

	if ((fp = fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", "r")) == NULL) {
		return;
	}

	if (fgets(buf, sizeof(buf), fp) != NULL) {
		if (strncmp(buf, "performance", strlen("performance")) != 0) {
			fprintf(stderr, "WARNING: CPU frequency scaling governor is not set to performance\n");
		}
	}

	fclose(fp);
}

#define BPM 60

#define EIGHT_NOTE	ONE_NOTE / 8
#define QUARTER_NOTE    ONE_NOTE / 4
#define HALF_NOTE       ONE_NOTE / 4
#define ONE_NOTE	60000 / BPM
#define ONE_HALF_NOTE   ONE_NOTE * 1.5
#define TWO_NOTE    	ONE_NOTE * 2

int main(int argc, char *argv[])
{
	cfg_core_cnt = 4;

	check_cpu_governor();

	note_t notes[] = {
		// Bar 1
		{ A5, ONE_NOTE },
		{ E5, HALF_NOTE },
		{ F5, HALF_NOTE },
		{ G5, HALF_NOTE },
		{ A5, QUARTER_NOTE },
		{ G5, QUARTER_NOTE },
		{ F5, HALF_NOTE },
		{ E5, HALF_NOTE },
		{ D5, ONE_NOTE },
		{ D5, HALF_NOTE },
		{ F5, HALF_NOTE },
		{ A5, ONE_NOTE },
		{ G5, HALF_NOTE },
		{ F5, HALF_NOTE },
		{ E5, ONE_HALF_NOTE },
		{ F5, HALF_NOTE },
		{ G5, ONE_NOTE },
		{ A5, ONE_NOTE },
		{ F5, ONE_NOTE },
		{ D5, ONE_NOTE },
		{ D5, TWO_NOTE },

		/*
		// Bar 2
		{ MUTE, HALF_NOTE },
		{ G5, ONE_NOTE },
		{ B5_FLAT, HALF_NOTE },
		{ D6, ONE_NOTE },
		{ C6, HALF_NOTE },
		{ B5_FLAT, HALF_NOTE },
		{ A5, ONE_HALF_NOTE },
		{ F5, HALF_NOTE },
		{ A5, ONE_NOTE },
		{ G5, HALF_NOTE },
		{ F5, HALF_NOTE },
		{ E5, ONE_NOTE },
		{ E5, HALF_NOTE },
		{ F5, HALF_NOTE },
		{ G5, ONE_NOTE },
		{ A5, ONE_NOTE },
		{ F5, ONE_NOTE },
		{ D5, ONE_NOTE },
		{ D5, ONE_NOTE },
		{ MUTE, ONE_NOTE },
		*/
	};


	play_notes(notes, ARRAY_SIZE(notes));
	
	return EXIT_SUCCESS;
}
/*
int main(int argc, char *argv[])
{

	struct termios oldt;
	struct termios newt;

	frame_t test_frame = 0;
	size_t test_frame_len = 0;
	bool do_test_signal = false;
	bool modulate_backlight = false;
	char *input_file = NULL;

	// Parse command line arguments
	char *endp;
	int opt;
	while ((opt = getopt(argc, argv, "Bc:C:f:M:p:P:t:h")) != -1) {
		switch (opt) {
		case 'B':
			modulate_backlight = true;
			break;
		case 'c':
			carrier_freq = strtol(optarg, &endp, 10);
			if (endp == optarg || *endp != '\0' || carrier_freq <= 0) {
				fprintf(stderr, "Invalid argument to '-c' option\n");
				exit(EXIT_FAILURE);
			}
			break;
		case 'C':
			//TODO: autodetect core count and use that as default and max. value
			if (strcasecmp(optarg, "all") == 0) {
				cfg_core_cnt = -1;
			} else {
				cfg_core_cnt = strtol(optarg, &endp, 10);
				if (endp == optarg || *endp != '\0' || cfg_core_cnt < 0) {
					fprintf(stderr, "Invalid argument to '-C' option\n");
					exit(EXIT_FAILURE);
				}
			}
			break;
		case 'f':
			input_file = optarg;
			break;
		case 'M':
			if (strcasecmp(optarg, "ask") == 0) {
				cfg_modulation = MOD_ASK;
			} else if (strcasecmp(optarg, "psk") == 0) {
				cfg_modulation = MOD_PSK;
				cfg_bits_per_symbol = 1;
			} else if (strcasecmp(optarg, "qpsk") == 0) {
				cfg_modulation = MOD_PSK;
				cfg_bits_per_symbol = 2;
			} else if (strcasecmp(optarg, "8psk") == 0) {
				cfg_modulation = MOD_PSK;
				cfg_bits_per_symbol = 3;
			} else if (strcasecmp(optarg, "16psk") == 0) {
				cfg_modulation = MOD_PSK;
				cfg_bits_per_symbol = 4;
			} else {
				int retval = EXIT_SUCCESS;
				if (strcasecmp(optarg, "help") != 0) {
					fprintf(stderr, "Invalid modulation type\n");
					retval = EXIT_FAILURE;
				}
				printf("Available Modulation types: ask, psk, qpsk, 8psk, 16psk\n");
				exit(retval);
			}
			break;
		case 'p':
			bit_periods = strtol(optarg, &endp, 10);
			if (endp == optarg || *endp != '\0' || bit_periods <= 0) {
				fprintf(stderr, "Invalid argument to '-p' option\n");
				exit(EXIT_FAILURE);
			}
			break;
		case 'P':
			modulate_pid = strtol(optarg, &endp, 10);
			if (endp == optarg || *endp != '\0' || modulate_pid <= 0) {
				fprintf(stderr, "Invalid argument to '-P' option\n");
				exit(EXIT_FAILURE);
			}
			break;
		case 't': {
			unsigned long int val = strtoul(optarg, &endp, 16);
			if (endp == optarg || *endp != '\0') {
				fprintf(stderr, "Invalid argument to '-t' option\n");
				exit(EXIT_FAILURE);
			}
			if (val > ((frame_t) -1)) {
				fprintf(stderr, "Argument to '-t' option out of range\n");
				exit(EXIT_FAILURE);
			}
			test_frame = val;
			test_frame_len = 8;
			val >>= 8;
			while (val != 0) {
				val >>= 8;
				test_frame_len += 8;
			}
			do_test_signal = true;
			break;
		}
		case 'h':
			usage(argv[0]);
			exit(EXIT_SUCCESS);
			break;
		default: // '?'
			usage(argv[0]);
			exit(EXIT_FAILURE);
		}
	}

	if (argc - optind != 0) {
		fprintf(stderr, "Incorrect amount of arguments\n");
		usage(argv[0]);
		exit(EXIT_FAILURE);
	}

	check_cpu_governor();

	// Determine core count
	if (cfg_core_cnt < 0) {
		cfg_core_cnt = sysconf(_SC_NPROCESSORS_ONLN);
		if (cfg_core_cnt < 1) {
			fprintf(stderr, "Unable to determine amount of processor cores\n");
			exit(EXIT_FAILURE);
		}
	}
	if (cfg_core_cnt > MAX_THREADS) {
		fprintf(stderr, "WARNING: Only %d cores supported\n", MAX_THREADS);
		cfg_core_cnt = MAX_THREADS;
	}

	if (modulate_backlight) {
		backlight_fd = open(BACKLIGHT_CONTROL_FILE, O_WRONLY);
		if (backlight_fd == -1) {
			fprintf(stderr, "Failed to open file %s: %s\n",
					BACKLIGHT_CONTROL_FILE, strerror(errno));
			exit(EXIT_FAILURE);
		}
	}

	// Setup stuff
	signal(SIGINT, &terminate_cb);


	if (do_test_signal) {
		printf("Sending test signal at %d Hz\n", carrier_freq);
		while (! terminate) {
			send_test_signal(test_frame, test_frame_len);
		}
	} else if (input_file != NULL) {
		FILE *ifp = NULL;
		uint8_t buf[1024];
		size_t len;

		if ((ifp = fopen(input_file, "rb")) == NULL) {
			perror("Failed to open input file");
			exit(EXIT_FAILURE);
		}

		while ((len = fread(buf, 1, sizeof(buf), ifp)) != 0) {
			send_buf(buf, len);
		}

		fclose(ifp);
	} else {
		tcgetattr(STDIN_FILENO, &oldt);
		newt = oldt;
		newt.c_lflag &= ~( ICANON | ECHO );
		tcsetattr(STDIN_FILENO, TCSANOW, &newt);

		char c;
		c = getchar();
		while (! terminate && c != EOF) {
			send_char(c);
			putchar(c);
			fflush(stdout);
			c = getchar();
		}

		tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
	}

	if (backlight_fd != -1) close(backlight_fd);

	return EXIT_SUCCESS;
}
*/
