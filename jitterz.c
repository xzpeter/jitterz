/*
 * jitterz
 *
 * Copyright 2019 Tom Rix <trix@redhat.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of version 3 of the GNU General Public License
 * as published by the Free Software Foundation.
 *
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <stdint.h>
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
#include <math.h>

static inline uint64_t tsc()
{
	uint64_t ret = 0;
	uint32_t l, h;
	__asm__ __volatile__("lfence");
	__asm__ __volatile__("rdtsc" : "=a" (l) , "=d" (h));
	ret = ((uint64_t) h << 32) | l;
	return ret;
}

struct bucket {
	uint64_t s;
	uint64_t c;
};

static int move_to_core(int core_i)
{
	cpu_set_t cpus;

	CPU_ZERO(&cpus);
	CPU_SET(core_i, &cpus);
	return sched_setaffinity(0, sizeof(cpus), &cpus);
}

static int make_it_rt(int rtprio)
{
	struct sched_param p;

	p.sched_priority = rtprio;
	return sched_setscheduler(0, SCHED_FIFO, &p);
}

static long read_cpuinfo_cur_freq(int core_i)
{
	uint64_t fs;
	char path[80];
	FILE *f = 0;

	snprintf(path, 80, "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_cur_freq", core_i);
	f = fopen(path, "rt");
	if (f) {
		fscanf(f, "%lu", &fs);
		fclose(f);
	} else {
		exit(1);
	}

	return fs;
}

int main(int argc, char* argv[])
{
	int core = 0;    /* CMD line parameter */
	int rtprio = 5;  /* CMD line parameter */
	struct timespec tvs, tve;
	double sec;
	uint64_t fs, fe, fr;
	unsigned i, j, rt = 60;
	uint64_t dt = 1500;
	struct bucket b[16];
	uint64_t frs, fre, lt;

	/* return of this function must be tested for success */
        if (move_to_core(core) != 0) {
		printf("Error while setting thread affinity to core %d!", core);
		exit(1);
	}
        if (make_it_rt(rtprio) != 0) {
		printf("Error while setting SCHED_FIFO policy/priority!");
		exit(1);
	}

	fr = fs = 0; fe = 1;
	while (fs != fe) {
	retry:
		if (!fr) {
			fs = read_cpuinfo_cur_freq(0);
			fe = 0;
		} else {
			fs = fr;
		}
		// printf("%lu\n", fs);
		uint64_t dt_min = (dt * fs) / 1000000;
		lt = 0;
		// printf("%lu\n", dt_min);
		
		for (j = 0; j < 16; j++) {
			b[j].c = 0;
			if (j == 0)
				b[j].s = dt_min;
			else
				b[j].s = b[j-1].s * 2;
		}
		fs *= 1000;
		
		frs = tsc();
		clock_gettime(CLOCK_MONOTONIC_RAW, &tvs);
		
		for (i = 0; i < rt; i++) {
			uint64_t s, e, so;

			s = tsc();
			e = s;
			e += fs;
			if (e < s) {
				goto retry;
			}
			so = s;
			
			while(1) {
				s = tsc();
				if (s > so) {
					uint64_t d = s - so;
					if (d >= dt_min) {
						lt += d;
						for (j = 16; j > 0; j--) {
							if (d >= b[j-1].s) {
								b[j-1].c = b[j-1].c + 1;
								break;
							}
						}
					}
				}
				if (s >= e)
					break;
				so = s;
			}
		}
		fre = tsc();
		clock_gettime(CLOCK_MONOTONIC_RAW, &tve);
		sec = tve.tv_sec - tvs.tv_sec + (tve.tv_nsec - tvs.tv_nsec) / 1e9;
		if ((fabs(sec - rt) / (double)rt) > 0.01) {
			//printf("%f %u\n", sec, rt);
			if (fre > frs) {
				fr = (fre - frs) / (1000 * sec);
				fe = fr * 1000;
				//printf("%lu\n", fr);
			}
			goto retry;
		}
		
		// printf("%f %f\n", sec, 1/sec);
		if (!fr) {
			fe = read_cpuinfo_cur_freq(0);
			fe *= 1000;
		}
	}

	// printf("%lu\n", fs / 1000);
	for (j = 0; j < 16; j++) {
		// printf("%lu %lu\n", b[j].s, b[j].c);
		printf("%lu\n", b[j].c);
	}

	if (lt != fs) {
		printf("Lost time %f\n", (double)lt/(double)fs);
		return 1;
	}

	return 0;
}
