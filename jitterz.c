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

static int cpu;
static int clocksel;
static int policy = SCHED_FIFO;
static int priority = 5;

#define NUMBER_BUCKETS 16
static struct bucket {
	uint64_t tick_boundry;
	uint64_t count;
} b[NUMBER_BUCKETS];

static uint64_t accumulated_lost_ticks;
static uint64_t delta_time = 1500; /* milli sec */
static uint64_t delta_tick_min; /* first bucket's tick boundry */
static uint64_t frequency_start;
static uint64_t frequency_end;
static uint64_t frequency_run;

static inline void initialize_buckets(void)
{
	int i;

	for (i = 0; i < NUMBER_BUCKETS; i++) {
		b[i].count = 0;
		if (i == 0)
			b[i].tick_boundry = delta_tick_min;
		else
			b[i].tick_boundry = b[i - 1].tick_boundry * 2;
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

static inline uint64_t time_stamp_counter(void)
{
	uint64_t ret = -1;
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
	return ret;
}

static inline int move_to_core(int core_i)
{
	cpu_set_t cpus;

	CPU_ZERO(&cpus);
	CPU_SET(core_i, &cpus);
	return sched_setaffinity(0, sizeof(cpus), &cpus);
}

static inline int set_sched(void)
{
	struct sched_param p = { 0 };

	p.sched_priority = priority;
	return sched_setscheduler(0, policy, &p);
}

static inline long read_cpu_current_frequency(int cpu)
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
				fclose(f);
			}
		}
	}

	if (ret == (uint64_t)-1) {
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
	       "-p PRIO  --priority=PRIO   priority of highest prio thread\n"
	       "         --policy=NAME     policy of measurement thread, where NAME may be one\n"
	       "                           of: other, normal, batch, idle, fifo or rr.\n");
	if (error)
		exit(EXIT_FAILURE);
	exit(EXIT_SUCCESS);
}

static inline char *policyname(int policy)
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
	OPT_PRIORITY,
	OPT_POLICY,
	OPT_HELP,
};

/* Process commandline options */
static inline void process_options(int argc, char *argv[], int max_cpus)
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
			{ "priority", required_argument, NULL, OPT_PRIORITY },
			{ "policy", required_argument, NULL, OPT_POLICY },
			{ "help", no_argument, NULL, OPT_HELP },
			{ NULL, 0, NULL, 0 },
		};
		int c = getopt_long(argc, argv, "c:hp:", long_options,
				    &option_index);
		if (c == -1)
			break;
		switch (c) {
		case 'c':
		case OPT_CPU:
			cpu = atoi(optarg);
			break;
		case OPT_CLOCK:
			clocksel = atoi(optarg);
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
		}
	}
}

int main(int argc, char **argv)
{
	int max_cpus = sysconf(_SC_NPROCESSORS_ONLN);
	struct timespec tvs, tve;
	double sec;
	unsigned int i, j, rt = 60;
	uint64_t frs, fre;

	process_options(argc, argv, max_cpus);

	/* return of this function must be tested for success */
	if (move_to_core(cpu) != 0) {
		fprintf(stderr,
			"Error while setting thread affinity to cpu %d\n", cpu);
		exit(1);
	}
	if (set_sched() != 0) {
		fprintf(stderr, "Error while setting %s policy, priority %d\n",
			policyname(policy), priority);
		exit(1);
	}

	if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
		fprintf(stderr, "Error while locking process memory\n");
		exit(1);
	}

	frequency_run = frequency_start = 0;
	frequency_end = 1;
	/* keep running until the start/end cpu frequency is the same */
	while (frequency_start != frequency_end) {
	retry:
		if (!frequency_run) {
			frequency_start = read_cpu_current_frequency(cpu);
			frequency_end = 0;
		} else {
			frequency_start = frequency_run;
		}
		delta_tick_min = (delta_time * frequency_start) / 1000000;

		accumulated_lost_ticks = 0;
		initialize_buckets();

		frequency_start *= 1000;

		frs = time_stamp_counter();
		clock_gettime(CLOCK_MONOTONIC_RAW, &tvs);

		for (i = 0; i < rt; i++) {
			uint64_t s, e, so;

			s = time_stamp_counter();
			e = s;
			e += frequency_start;
			if (e < s)
				goto retry;
			so = s;

			while (1) {
				uint64_t d;

				s = time_stamp_counter();
				if (s == so)
					continue;

				d = s - so;
				update_buckets(d);

				if (s >= e)
					break;
				so = s;
			}
		}
		fre = time_stamp_counter();
		clock_gettime(CLOCK_MONOTONIC_RAW, &tve);
		sec = tve.tv_sec - tvs.tv_sec +
		      (tve.tv_nsec - tvs.tv_nsec) / 1e9;
		if ((fabs(sec - rt) / (double)rt) > 0.01) {
			if (fre > frs) {
				frequency_run = (fre - frs) / (1000 * sec);
				frequency_end = frequency_run * 1000;
			}
			goto retry;
		}
		if (!frequency_run) {
			frequency_end = read_cpu_current_frequency(cpu);
			frequency_end *= 1000;
		}
	}
	for (j = 0; j < 16; j++)
		printf("%" PRIu64 "\n", b[j].count);

	printf("Lost time %f\n",
	       (double)accumulated_lost_ticks / (double)frequency_start);

	return 0;
}
