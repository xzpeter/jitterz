// SPDX-License-Identifier: GPL-2.0-only
/*
 * jitterz
 *
 * Copyright 2019-2020 Tom Rix <trix@redhat.com>
 *
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <stdint.h>
#include <getopt.h>
#include <unistd.h>
#include <inttypes.h>
#include <time.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <sys/time.h>
#include <sched.h>
#include <stdbool.h>
#include <sys/sysinfo.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <math.h>

#define CPU_DEFAULT 0
static int cpu;
static int clocksel;
static int policy = SCHED_FIFO;
static int priority = 5;

#define NUMBER_BUCKETS 16
static struct bucket {
	uint64_t tick_boundry;
	uint64_t count;
	uint64_t time_boundry;
} b[NUMBER_BUCKETS];

static uint64_t accumulated_lost_ticks;
static uint64_t delta_time = 500; /* nano sec */
static uint64_t delta_tick_min; /* first bucket's tick boundry */
#define RUN_TIME_DEFAULT 60
static int run_time = RUN_TIME_DEFAULT; /* seconds */
static int use_gettime = 1;
#define NSEC_PER_SEC		1000000000
/* how close do multiple run's calculated frequency have to be valid */
#define FREQUENCY_TOLERNCE 0.01
static inline void initialize_buckets(void)
{
	int i;

	for (i = 0; i < NUMBER_BUCKETS; i++) {
		b[i].count = 0;
		if (i == 0) {
			b[i].tick_boundry = delta_tick_min;
			b[i].time_boundry = delta_time;
		} else {
			b[i].tick_boundry = b[i - 1].tick_boundry * 2;
			b[i].time_boundry = b[i - 1].time_boundry * 2;
		}
	}
}

static inline void update_buckets(uint64_t ticks)
{
	if (ticks >= delta_tick_min) {
		int i;

		accumulated_lost_ticks += ticks;
		for (i = NUMBER_BUCKETS; i > 0; i--) {
			if (ticks >= b[i - 1].tick_boundry) {
				b[i - 1].count++;
				break;
			}
		}
	}
}

/* Returns clock ticks */
static inline uint64_t time_stamp_counter(void)
{
	uint64_t ret = -1;

	if (use_gettime) {
		struct timespec ts;

		if (clock_gettime(CLOCK_MONOTONIC, &ts) == -1) {
			fprintf(stderr, "clock_gettime() call failed: %s\n", strerror(errno));
			exit (errno);
		}
		ret = (uint64_t) ((ts.tv_sec * NSEC_PER_SEC) + ts.tv_nsec);
	}
	else {
#if defined(__i386__) || defined(__x86_64__)
		uint32_t l, h;

		__asm__ __volatile__("lfence");
		__asm__ __volatile__("rdtsc" : "=a"(l), "=d"(h));
		ret = ((uint64_t)h << 32) | l;
#else
		fprintf(stderr,
			"Add a time_stamp_counter function for your arch here %s:%d\n",
			__FILE__, __LINE__);
		exit(1);
#endif
	}
	return ret;
}

static inline int move_to_core()
{
	cpu_set_t cpus;

	CPU_ZERO(&cpus);
	CPU_SET(cpu, &cpus);
	return sched_setaffinity(0, sizeof(cpus), &cpus);
}

static inline int set_sched(void)
{
	struct sched_param p = { 0 };

	p.sched_priority = priority;
	return sched_setscheduler(0, policy, &p);
}

static inline uint64_t read_cpu_current_frequency()
{
	uint64_t ret = -1;
	char path[256];
	struct stat sb;
	int i;
	char *freq[3] = {
		/* scaling_cur_freq is current kernel /sys file */
		"scaling_cur_freq"
		/* old /sys file is cpuinfo_cur_freq */
		"cpuinfo_cur_freq",
		/* assumes a busy wait will be run at the max freq */
		"cpuinfo_max_freq",
	};
	for (i = 0; i < 3 && ret == -1; i++) {
		snprintf(path, 256, "/sys/devices/system/cpu/cpu%d/cpufreq/%s",
			 cpu, freq[i]);
		if (!stat(path, &sb)) {
			FILE *f = 0;

			f = fopen(path, "rt");
			if (f) {
				fscanf(f, "%" PRIu64, &ret);
				/*
				 * sysfs interface is in units of KHz
				 * convert to Hz
				 */
				ret *= 1000;
				fclose(f);
			}
		}
	}

	if (ret == -1) {
		printf("Error reading CPU frequency for core %d\n", cpu);
		exit(1);
	}
	return ret;
}

/* Print usage information */
static inline void display_help(int error)
{
	printf("jitterz\n");
	printf("Usage:\n"
	       "jitterz <options>\n\n"
	       "-c NUM   --cpu=NUM         which cpu to run on\n"
	       "         --clock=CLOCK     select clock\n"
	       "                           0 = CLOCK_MONOTONIC (default)\n"
	       "                           1 = CLOCK_REALTIME\n"
	       "-d SEC   --duration=SEC    duration of the test in seconds\n"
	       "-p PRIO  --priority=PRIO   priority of highest prio thread\n"
	       "         --policy=NAME     policy of measurement thread, where NAME may be one\n"
	       "                           of: other, normal, batch, idle, fifo or rr.\n"
	       "         --rdtsc           use inline RDTSC instruction rather than clock_gettime()\n"
		);
	if (error)
		exit(EXIT_FAILURE);
	exit(EXIT_SUCCESS);
}

static inline char *policyname()
{
	char *policystr = "";

	switch (policy) {
	case SCHED_OTHER:
		policystr = "other";
		break;
	case SCHED_FIFO:
		policystr = "fifo";
		break;
	case SCHED_RR:
		policystr = "rr";
		break;
	case SCHED_BATCH:
		policystr = "batch";
		break;
	case SCHED_IDLE:
		policystr = "idle";
		break;
	}
	return policystr;
}

static inline void handlepolicy(char *polname)
{
	if (strncasecmp(polname, "other", 5) == 0)
		policy = SCHED_OTHER;
	else if (strncasecmp(polname, "batch", 5) == 0)
		policy = SCHED_BATCH;
	else if (strncasecmp(polname, "idle", 4) == 0)
		policy = SCHED_IDLE;
	else if (strncasecmp(polname, "fifo", 4) == 0)
		policy = SCHED_FIFO;
	else if (strncasecmp(polname, "rr", 2) == 0)
		policy = SCHED_RR;
	else /* default policy if we don't recognize the request */
		policy = SCHED_OTHER;
}

enum option_values {
	OPT_CPU = 1,
	OPT_CLOCK,
	OPT_DURATION,
	OPT_PRIORITY,
	OPT_POLICY,
	OPT_RDTSC,
	OPT_HELP,
};

/* Process commandline options */
static inline void process_options(int argc, char *argv[], long max_cpus)
{
	for (;;) {
		int option_index = 0;
		/*
		 * Options for getopt
		 * Ordered alphabetically by single letter name
		 */
		static const struct option long_options[] = {
			{ "clock", required_argument, NULL, OPT_CLOCK },
			{ "cpu", required_argument, NULL, OPT_CPU },
			{ "duration", required_argument, NULL, OPT_DURATION },
			{ "priority", required_argument, NULL, OPT_PRIORITY },
			{ "policy", required_argument, NULL, OPT_POLICY },
			{ "rdtsc", optional_argument, NULL, OPT_RDTSC },
			{ "help", no_argument, NULL, OPT_HELP },
			{ NULL, 0, NULL, 0 },
		};
		int c = getopt_long(argc, argv, "c:d:hp:", long_options,
				    &option_index);
		if (c == -1)
			break;
		switch (c) {
		case 'c':
		case OPT_CPU:
			cpu = atoi(optarg);
			if (cpu >= max_cpus)
				cpu = CPU_DEFAULT;
			break;
		case OPT_CLOCK:
			clocksel = atoi(optarg);
			break;
		case 'd':
		case OPT_DURATION:
			run_time = atoi(optarg);
			if (run_time <= 0)
				run_time = RUN_TIME_DEFAULT;
			break;
		case 'p':
		case OPT_PRIORITY:
			priority = atoi(optarg);
			if (policy != SCHED_FIFO && policy != SCHED_RR)
				policy = SCHED_FIFO;
			break;
		case '?':
		case OPT_HELP:
			display_help(0);
			break;
		case OPT_POLICY:
			handlepolicy(optarg);
			break;
		case OPT_RDTSC:
			use_gettime = 0;
			break;
		}
	}
}

int main(int argc, char **argv)
{
	long max_cpus = sysconf(_SC_NPROCESSORS_ONLN);
	struct timespec tvs, tve;
	double real_duration; /* sec */
	int i;
	uint64_t test_tick_start, test_tick_end;
	static uint64_t frequency_start, frequency_run;
	double frequency_diff = 0.0; /* unitless */

	process_options(argc, argv, max_cpus);

	/* return of this function must be tested for success */
	if (move_to_core() != 0) {
		fprintf(stderr,
			"Error while setting thread affinity to cpu %d\n", cpu);
		exit(1);
	}
	if (set_sched() != 0) {
		fprintf(stderr, "Error while setting %s policy, priority %d\n",
			policyname(), priority);
		exit(1);
	}

	if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
		fprintf(stderr, "Error while locking process memory\n");
		exit(1);
	}

	frequency_run = 0;
	frequency_start = read_cpu_current_frequency();
	/*
	 * Start off using the cpu frequency from sysfs
	 * After each loop
	 *   calculate the real frequency the whole test
	 *   run until the real frequency is close enough to the last run
	 */
	do {
	retry:
		if (frequency_run)
			frequency_start = frequency_run;
		delta_tick_min = (delta_time * frequency_start) /
				 1000000000; /* ticks/nsec */

		accumulated_lost_ticks = 0;
		initialize_buckets();

		/* record the starting tick and clock time for the test */
		test_tick_start = time_stamp_counter();
		clock_gettime(CLOCK_MONOTONIC_RAW, &tvs);

		/* loop over seconds run time */
		for (i = 0; i < run_time; i++) {
			uint64_t tick, end_tick, old_tick, tick_overflow;

			end_tick = old_tick = tick = time_stamp_counter();
			end_tick += frequency_start;
			/*
			 * Overflow check
			 * If end_tick < tick, there will be an overlow
			 * Add additional second's worth of ticks so
			 * we do not have to check inside the while loop
			 * to cover the case that end_tick is close to
			 * overflowing.
			 */
			tick_overflow = end_tick + frequency_start;
			if (tick_overflow < tick)
				goto retry;

			/*
			 * Loop until tick >= end_tick
			 *
			 * If the difference in old and current tick
			 * exceed the minimum tick treshold
			 *   increment the greatest bucket
			 *   accumulate total lost ticks
			 *
			 * set old_tick to current tick
			 */
			while (tick < end_tick) {
				tick = time_stamp_counter();
				if (tick == old_tick)
					continue;
				update_buckets(tick - old_tick);
				old_tick = tick;
			}
		}
		/* Record the test ending tick and clock time */
		test_tick_end = time_stamp_counter();
		/* overflow */
		if (test_tick_end < test_tick_start)
			goto retry;
		clock_gettime(CLOCK_MONOTONIC_RAW, &tve);
		/* sec */
		real_duration = tve.tv_sec - tvs.tv_sec +
				(tve.tv_nsec - tvs.tv_nsec) / 1e9;
		/* tick / sec */
		frequency_run =
			(test_tick_end - test_tick_start) / (real_duration);
		frequency_diff = fabs((frequency_run * 1.) - frequency_start) /
				 frequency_start;
	} while (frequency_diff > FREQUENCY_TOLERNCE);

	fprintf(stdout, "cutoff time (usec) : stall count \n");
	for (i = 0; i < NUMBER_BUCKETS; i++) {
		double t = b[i].time_boundry / 1000000000.; /* sec */
		if (t < real_duration) {
			double tb = b[i].time_boundry; /* nsec */
			fprintf(stdout, "%.1f : %" PRIu64 "\n", tb / 1000.,
				b[i].count);
		}
	}

	printf("Lost time %f out of %d seconds\n",
	       (double)accumulated_lost_ticks / (double)frequency_start,
	       run_time);

	return 0;
}
