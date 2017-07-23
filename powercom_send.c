/**
 * powercom_send.c - Sends data through power line by modulating CPU load
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
#include <assert.h>
#include <termios.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <sys/stat.h>

// includes needed for the timers
#include <signal.h>
#include <time.h>
#include <sys/types.h>

#include <pthread.h>

//#define DEBUG_BITS 1

#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define USEC_AS_NSEC 1000
#define MSEC_AS_NSEC (1000 * 1000)
#define SEC_AS_NSEC (1000 * 1000 * 1000)
#define HZ_TO_NSEC(F) (SEC_AS_NSEC / F)
#define EVENTS_PER_PERIOD (1 << cfg_bits_per_symbol)

// Modulation parameters
#define DEFAULT_BIT_PERIODS 10 // amount of wave periods(=2 timer events) per bit
#define DEFAULT_CARRIER_FREQ 30

#define MAX_PKT_LEN ((uint8_t) 16)

#define MAX_THREADS 32

static bool abort_transmit = false;
static bool terminate = false;

static int carrier_freq = DEFAULT_CARRIER_FREQ;
static int bit_periods = DEFAULT_BIT_PERIODS;

static int modulate_pid = 0;

static int cfg_core_cnt = -1;
static enum { MOD_ASK, MOD_PSK, MOD_DPSK } cfg_modulation = MOD_ASK;
static int cfg_bits_per_symbol = 1;
enum { ENC_NONE, ENC_RS232, ENC_PACKET } cfg_encoding = ENC_PACKET;

typedef uint32_t frame_t;

struct {
	// Variable used to communicate back to main program
	sig_atomic_t have_lock;	// If main thread currently has a lock
	sig_atomic_t done;	// all frames transmitted

	// Frame data
	const frame_t *frames;	// Frames to send
	size_t frame_cnt;	// Amount of frames to send
	size_t frame_len;	// Length of frames in bits
	frame_t bit_mask;	// bit mask of current byte

	// Modulation state
	uint8_t symbol;			// Symbol currently being transmitted
	unsigned int event_cnt;		// Number of timer events since start(psk)/last edge(ask)
} state = { false, false, NULL, 0, 0, 0, 0};

/********************** Powercom transmission functions ***********************/
void send_terminate_cb(int sig)
{
	abort_transmit = true;
}

void ask_timer_cb(int sig)
{
	// If no data then generate test signal
	//TODO: move to own callback
	if (state.frames == NULL) {
		if (state.have_lock) {
			state.have_lock = false;
		} else {
			state.have_lock = true;
		}
		return;
	}

	// All data transmitted then exit
	//TODO: doesn't this cut the last bit short???
	if (state.frame_cnt == 0 || state.frames == NULL) {
		state.have_lock = false;
		state.done = true;
		return;
	}

	// Set locking based on data
	if (state.frames[0] & state.bit_mask) {
		if (state.have_lock) {
#ifdef DEBUG_BITS
			fprintf(stderr, "1");
#endif
			state.have_lock = false;
		} else {
#ifdef DEBUG_BITS
			fprintf(stderr, "0");
#endif
			state.have_lock = true;
		}
	} else {
#ifdef DEBUG_BITS
		fprintf(stderr, "0");
#endif
		if (! state.have_lock) {
			state.have_lock = true;
		}
	}

	// select next bit
	state.event_cnt++;
	if (state.event_cnt >= bit_periods * 2) {
#ifdef DEBUG_BITS
		fprintf(stderr, "\n");
#endif
		state.event_cnt = 0;

		state.bit_mask = (state.bit_mask >> 1);
		if (state.bit_mask == 0) {
			state.bit_mask = (1 << (state.frame_len - 1));
			state.frames++;
			state.frame_cnt--;
		}
	}
}

void psk_timer_cb(int sig)
{
	if (state.done) return;

	// Determine new phase based on data
	if ((state.event_cnt % (EVENTS_PER_PERIOD * bit_periods)) == 0) {
		// All data transmitted then exit
		if (state.frame_cnt == 0 || state.frames == NULL) {
			state.have_lock = false;
			state.done = true;
#ifdef DEBUG_BITS
			fprintf(stderr, "\n---\n");
#endif
			return;
		}

		// Select new bit(s) to transmit
		uint8_t new_bits = 0;
		for (int i = 0; i < cfg_bits_per_symbol; i++) {
			new_bits <<= 1;
			if (state.frame_cnt != 0) {
				if (state.frames[0] & state.bit_mask) {
					new_bits |= 1;
				}

				state.bit_mask = (state.bit_mask >> 1);
				if (state.bit_mask == 0) {
					state.bit_mask = (1 << (state.frame_len - 1));
					state.frames++;
					state.frame_cnt--;
				}
			}
		}

		//TODO: symbol to phase mapping eg. '11' = 180 Deg. instead of -90 Deg.
		if (cfg_modulation == MOD_DPSK) {
			state.symbol += new_bits;
			state.symbol %= (1 << cfg_bits_per_symbol);
		} else {
			state.symbol = new_bits;
		}

#ifdef DEBUG_BITS
		fprintf(stderr, "\nbits: 0x%02x - ", new_bits);
#endif
	}

	// Generate carrier
	// x[n] = { 1 : (n + phase) % EVENTS_PER_PERIOD) >= 0.5 * EVENTS_PER_PERIOD
	//          0 : (n + phase) % EVENTS_PER_PERIOD) <  0.5 * EVENTS_PER_PERIOD
	unsigned int phase = state.symbol * EVENTS_PER_PERIOD / (1 << cfg_bits_per_symbol);
	if (((state.event_cnt + phase) % EVENTS_PER_PERIOD) < (EVENTS_PER_PERIOD / 2)) {
		// level = 0;
		state.have_lock = true;
	} else {
		// level = 1;
		state.have_lock = false;
	}

#ifdef DEBUG_BITS
	fprintf(stderr, "%d", state.have_lock ? 0 : 1);
#endif

	state.event_cnt++;
}

struct thread_state {
	pthread_mutex_t mutex;		// Mutex, if locked by master thread, child thread will be idle
	unsigned int id;		// number of thread
	sig_atomic_t *stop;		// If true the child thread should terminate 
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
	while (! *(s->stop)) {
		pthread_mutex_lock(&(s->mutex));
		pthread_mutex_unlock(&(s->mutex));
	}

	return NULL;
}

void powercom_transmit(const frame_t *frames, size_t frame_cnt, size_t frame_len)
{
	int i;

	pthread_t threads[MAX_THREADS];
	struct thread_state tstates[MAX_THREADS];

	timer_t timer;

	struct timespec first_alarm;
	struct timespec timer_interval;

	struct itimerspec alarm_time;

	sig_atomic_t stop_threads = false;

	// Init state
	state.have_lock = true;
	state.done = false;

	state.frames = frames;
	state.frame_cnt = frame_cnt;
	state.frame_len = frame_len;
	state.bit_mask = (1 << (state.frame_len - 1));

	state.symbol = 0;
	state.event_cnt = 0;

	// Per modulation configuration
	sighandler_t timer_cb = &ask_timer_cb;
	switch (cfg_modulation) {
	case MOD_ASK:
		timer_cb = &ask_timer_cb;

		timer_interval.tv_sec  = 0;
		timer_interval.tv_nsec = HZ_TO_NSEC(carrier_freq) / 2;
		break;
	case MOD_PSK:
	case MOD_DPSK:
		timer_cb = &psk_timer_cb;

		timer_interval.tv_sec  = 0;
		timer_interval.tv_nsec = HZ_TO_NSEC(carrier_freq) / EVENTS_PER_PERIOD;
		break;
	default:
		fprintf(stderr, "Unknown modulation type\n");
		exit(EXIT_FAILURE);
	}

	// Setup signal handlers
	sighandler_t old_sigalrm_handler = signal(SIGALRM, timer_cb);
	sighandler_t old_sigint_handler = signal(SIGINT, &send_terminate_cb);

	// Create the timer
	if (timer_create(CLOCK_REALTIME, NULL, &timer) == -1) {
		perror("timer_create");
		exit(EXIT_FAILURE);
	}

	first_alarm.tv_sec  = 0;
	first_alarm.tv_nsec = 1;

	alarm_time.it_value = first_alarm; 
	alarm_time.it_interval = timer_interval;

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

#ifdef WITH_BARRIER
		tstates[i].generate_load = &generate_load;
		tstates[i].barrier = &barrier;
#else
		pthread_mutex_init(&(tstates[i].mutex), NULL);
#endif

		pthread_create(&(threads[i]), NULL, helper_thread, &(tstates[i]));
	}

	// Start transmitting
	if (timer_settime(timer, 0, &alarm_time, NULL) == -1) {
		perror("timer_settime");
		goto cleanup;
	}

	while (!state.done && !abort_transmit) {
		if (state.have_lock) {
			if (modulate_pid > 0) {
				kill(modulate_pid, SIGSTOP);
			}

			for (i=0; i < cfg_core_cnt; i++) {
				pthread_mutex_lock(&(tstates[i].mutex));
			}

			while (state.have_lock && !state.done &&
					!abort_transmit) {
				sleep(10);
			}

			if (modulate_pid > 0) {
				kill(modulate_pid, SIGCONT);
			}

			for (i=0; i < cfg_core_cnt; i++) {
				pthread_mutex_unlock(&(tstates[i].mutex));
			}
		} else {
			sleep(10);
		}
	}

	if (modulate_pid > 0) {
		kill(modulate_pid, SIGSTOP);
	}

	// Clean up
	if (timer_delete(timer) == -1) {
		perror("timer_delete");
	}

cleanup:
	stop_threads = true;
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

/*************************** Encoding functions ******************************/

#define PREAMBLE1 0xaa
#define PREAMBLE2 0xa1
void send_buf_packet(const uint8_t *buf, size_t len)
{
	frame_t *frames = NULL;
	size_t frame_cnt = 2 + 1 + len;
	int i;

	assert(len <= 0xff);

	// Encode data to frames
	frames = (frame_t *) calloc(frame_cnt, sizeof(frame_t));
	if (frames == NULL) {
		perror("calloc()");
		return;
	}

	frames[0] = PREAMBLE1;
	frames[1] = PREAMBLE2;
	frames[2] = len;
	for (i=0; i < len; i++) {
		frames[3+i] = buf[i];
	}

	powercom_transmit(frames, frame_cnt, 8);

	free(frames);
}

#define BITS_PER_FRAME 7
#define STOP_BITS 1
#define FRAME_LEN (1 + BITS_PER_FRAME + STOP_BITS)

void send_buf_rs232(const uint8_t *buf, size_t len)
{
	frame_t *frames = NULL;
	size_t frame_cnt = len;
	int i;

	// Encode data to frames
	frames = (frame_t *) calloc(frame_cnt, sizeof(frame_t));
	if (frames == NULL) {
		perror("calloc()");
		return;
	}

	for (i = 0; i < len; i++) {
		frame_t frame;

		frame = 0x1; // preamble
		frame = (frame << BITS_PER_FRAME) | (buf[i] & ((1 << BITS_PER_FRAME) - 1)); // data
		frame = (frame << STOP_BITS); // stop bits

		frames[i] = frame;
	}

	powercom_transmit(frames, frame_cnt, FRAME_LEN);

	free(frames);
}

void send_buf_raw(const uint8_t *buf, size_t len)
{
	frame_t *frames = NULL;
	size_t frame_cnt = len;
	int i;

	// Encode data to frames
	frames = (frame_t *) calloc(frame_cnt, sizeof(frame_t));
	if (frames == NULL) {
		perror("calloc()");
		return;
	}

	for (i = 0; i < len; i++) {
		frames[i] = buf[i];
	}

	powercom_transmit(frames, frame_cnt, 8);

	free(frames);
}

/****************************** Send Helpers *********************************/
void send_buf(const uint8_t *buf, size_t len)
{
	switch (cfg_encoding) {
	case ENC_PACKET:
		send_buf_packet(buf, len);
		break;
	case ENC_RS232:
		send_buf_rs232(buf, len);
		break;
	case ENC_NONE:
		send_buf_raw(buf, len);
		break;
	default:
		fprintf(stderr, "ERROR: cfg_encoding contains unknown encoding\n");
		break;
	}
}

void send_str(const char *str)
{
	send_buf((uint8_t *)str, strlen(str));
}

void send_char(char c)
{
	send_buf((uint8_t *) &c, 1);
}

void send_test_signal(frame_t test_frame, size_t frame_len)
{
	frame_t frames[1024];
	for (size_t i = 0; i < ARRAY_SIZE(frames); i++) {
		frames[i] = test_frame;
	}
	powercom_transmit(frames, ARRAY_SIZE(frames), frame_len);
}

/************************* Main program ***************************************/
void terminate_cb(int sig)
{
	terminate = true;
	fclose(stdin);
}

void usage(const char *name)
{
	printf("Usage: %s [-cCfpPMth]\n", name);
	printf("\n");
	printf("Where:\n");
	printf(" -c <FREQ>    Carrier frequency (default: %d)\n", DEFAULT_CARRIER_FREQ);
	printf(" -C <N>       Number of CPU cores to modulate (default: 'all')\n");
	printf(" -E <ENC>     Encoding type to use, or 'help' (default: 'packet')\n");
	printf(" -f <path>    Send data contained in file\n");
	printf(" -p <N>       Amount of carrier periods to encode one bit (default: %d)\n", DEFAULT_BIT_PERIODS);
	printf(" -P <PID>     Modulate running state of external process\n");
	printf(" -M <MOD>     Modulation type to use, or 'help' (default: ask)\n");
	printf(" -t <PTRN>    Continuously transmit byte(s) without extra encoding using the\n"
	       "              selected modulation. PTRN is given in hexadecimal notation\n");
	printf(" -h           Display this help message\n");
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

int main(int argc, char *argv[])
{
	frame_t test_frame = 0;
	size_t test_frame_len = 0;
	bool do_test_signal = false;
	char *input_file = NULL;

	// Parse command line arguments
	char *endp;
	int opt;
	while ((opt = getopt(argc, argv, "c:C:E:f:M:p:P:t:h")) != -1) {
		switch (opt) {
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
		case 'E':
			if (strcasecmp(optarg, "packet") == 0) {
				cfg_encoding = ENC_PACKET;
			} else if (strcasecmp(optarg, "rs232") == 0) {
				cfg_encoding = ENC_RS232;
			} else if (strcasecmp(optarg, "none") == 0) {
				cfg_encoding = ENC_NONE;
			} else {
				int retval = EXIT_SUCCESS;
				if (strcasecmp(optarg, "help") != 0) {
					fprintf(stderr, "Invalid encoding type\n");
					retval = EXIT_FAILURE;
				}
				printf("Available Encoding types: none, packet, rs232\n");
				exit(retval);
			}
			break;
		case 'f':
			input_file = optarg;
			break;
		case 'M':
			if (strcasecmp(optarg, "ask") == 0) {
				cfg_modulation = MOD_ASK;
			} else if (strcasecmp(optarg, "bpsk") == 0) {
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
			} else if (strcasecmp(optarg, "dbpsk") == 0) {
				cfg_modulation = MOD_DPSK;
				cfg_bits_per_symbol = 1;
			} else if (strcasecmp(optarg, "dqpsk") == 0) {
				cfg_modulation = MOD_DPSK;
				cfg_bits_per_symbol = 2;
			} else if (strcasecmp(optarg, "d8psk") == 0) {
				cfg_modulation = MOD_DPSK;
				cfg_bits_per_symbol = 3;
			} else if (strcasecmp(optarg, "d16psk") == 0) {
				cfg_modulation = MOD_DPSK;
				cfg_bits_per_symbol = 4;
			} else {
				int retval = EXIT_SUCCESS;
				if (strcasecmp(optarg, "help") != 0) {
					fprintf(stderr, "Invalid modulation type\n");
					retval = EXIT_FAILURE;
				}
				printf("Available Modulation types: ask, bpsk, qpsk, 8psk, 16psk, dbpsk, dqpsk, d8psk, d16psk\n");
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
		default: /* '?' */
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

	// Setup stuff
	signal(SIGINT, &terminate_cb);


	if (do_test_signal) {
		printf("Sending test signal at %d Hz @ %d bps\n", carrier_freq, carrier_freq / bit_periods * cfg_bits_per_symbol);
		while (! terminate) {
			send_test_signal(test_frame, test_frame_len);
		}
	} else if (input_file != NULL) {
		printf("Sending file at %d Hz @ %d bps\n", carrier_freq, carrier_freq / bit_periods * cfg_bits_per_symbol);
		FILE *ifp = NULL;
		uint8_t buf[MAX_PKT_LEN];
		size_t len;

		if ((ifp = fopen(input_file, "rb")) == NULL) {
			perror("Failed to open input file");
			exit(EXIT_FAILURE);
		}

		while (!terminate && (len = fread(buf, 1, sizeof(buf), ifp)) != 0) {
			send_buf(buf, len);
		}

		fclose(ifp);
#if 0
	} else if (do_interactive) {
		printf("Sending input at %d Hz @ %d bps\n", carrier_freq, carrier_freq / bit_periods * cfg_bits_per_symbol);

		struct termios oldt;
		struct termios newt;

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
#endif
	} else {
		printf("Sending input at %d Hz @ %d bps\n", carrier_freq, carrier_freq / bit_periods * cfg_bits_per_symbol);

		char buf[MAX_PKT_LEN];
		while (!terminate && fgets(buf, sizeof(buf), stdin) != NULL) {
			send_str(buf);
		}
	}

	return EXIT_SUCCESS;
}
