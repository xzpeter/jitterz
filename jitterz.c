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
#include <math.h>

static int cpu;
static int clocksel;
static int policy = SCHED_FIFO;
static int priority = 5;

#define CHECK_LOST_TIME()					\
	do {							\
		if (d >= dt_min) {				\
			lt += d;				\
			for (j = 16; j > 0; j--) {		\
				if (d >= b[j - 1].s) {		\
					b[j - 1].c =		\
						b[j - 1].c + 1;	\
					break;			\
				}				\
			}					\
		}						\
	} while (0)						\

static inline uint64_t tsc(void)
{
	uint64_t ret = 0;
	uint32_t l, h;

	__asm__ __volatile__("lfence");
	__asm__ __volatile__("rdtsc" : "=a"(l), "=d"(h));
	ret = ((uint64_t)h << 32) | l;
	return ret;
}

static int move_to_core(int core_i)
{
	cpu_set_t cpus;

	CPU_ZERO(&cpus);
	CPU_SET(core_i, &cpus);
	return sched_setaffinity(0, sizeof(cpus), &cpus);
}

static int set_sched(void)
{
	struct sched_param p = { 0 };

	p.sched_priority = priority;
	return sched_setscheduler(0, policy, &p);
}

static long read_cpuinfo_cur_freq(int core_i)
{
	uint64_t fs = -1;
	char path[80];
	struct stat sb;
	int i;
	char *freq[2] = {
		"cpuinfo_cur_freq",
		/* assumes a busy wait will be run at the max freq */
		"cpuinfo_max_freq",
	};
	for (i = 0; i < 2; i++) {
		snprintf(path, 80,
			 "/sys/devices/system/cpu/cpu%d/cpufreq/%s",
			 core_i, freq[1]);
		if (!stat(path, &sb)) {
			FILE *f = 0;

			f = fopen(path, "rt");
			if (f) {
				fscanf(f, "%lu", &fs);
				fclose(f);
			} else {
				perror(path);
			}
		} else {
			perror(path);
		}
	}

	if (fs == (uint64_t) -1) {
		printf("Error reading CPU frequency for core %d\n", core_i);
		exit(1);
	}
	return fs;
}

/* Print usage information */
static void display_help(int error)
{
	printf("jitterz V %1.2f\n", VERSION);
	printf("Usage:\n"
	       "jitterz <options>\n\n"
	       "-c NUM   --cpu=NUM         which cpu to run on"
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

static char *policyname(int policy)
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

static void handlepolicy(char *polname)
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
static void process_options(int argc, char *argv[], int max_cpus)
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
	uint64_t fs, fe, fr;
	unsigned int i, j, rt = 60;
	uint64_t dt = 1500;
	struct bucket {
		uint64_t s;
		uint64_t c;
	} b[16];
	uint64_t frs, fre, lt;

	process_options(argc, argv, max_cpus);

	/* return of this function must be tested for success */
	if (move_to_core(cpu) != 0) {
		printf("Error while setting thread affinity to cpu %d!", cpu);
		exit(1);
	}
	if (set_sched() != 0) {
		printf("Error while setting %s policy, priority %d!",
		       policyname(policy), priority);
		exit(1);
	}

	fr = fs = 0;
	fe = 1;
	while (fs != fe) {
retry:
		if (!fr) {
			fs = read_cpuinfo_cur_freq(cpu);
			fe = 0;
		} else {
			fs = fr;
		}
		uint64_t dt_min = (dt * fs) / 1000000;

		lt = 0;
		for (j = 0; j < 16; j++) {
			b[j].c = 0;
			if (j == 0)
				b[j].s = dt_min;
			else
				b[j].s = b[j - 1].s * 2;
		}
		fs *= 1000;

		frs = tsc();
		clock_gettime(CLOCK_MONOTONIC_RAW, &tvs);

		for (i = 0; i < rt; i++) {
			uint64_t s, e, so;

			s = tsc();
			e = s;
			e += fs;
			if (e < s)
				goto retry;
			so = s;

			while (1) {
				uint64_t d;

				s = tsc();
				if (s == so)
					continue;

				d = s - so;
				CHECK_LOST_TIME();
				if (s >= e)
					break;
				so = s;
			}
		}
		fre = tsc();
		clock_gettime(CLOCK_MONOTONIC_RAW, &tve);
		sec = tve.tv_sec - tvs.tv_sec +
		      (tve.tv_nsec - tvs.tv_nsec) / 1e9;
		if ((fabs(sec - rt) / (double)rt) > 0.01) {
			if (fre > frs) {
				fr = (fre - frs) / (1000 * sec);
				fe = fr * 1000;
			}
			goto retry;
		}
		if (!fr) {
			fe = read_cpuinfo_cur_freq(cpu);
			fe *= 1000;
		}
	}
	for (j = 0; j < 16; j++)
		printf("%lu\n", b[j].c);

	if (lt != fs) {
		printf("Lost time %f\n", (double)lt / (double)fs);
		return 1;
	}

	return 0;
}
