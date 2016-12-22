/*
 * Allocate and dirty memory.
 *
 * Usage: usemem size[k|m|g|t]
 *
 * gcc -lpthread -O -g -Wall  usemem.c -o usemem
 *
 * Copyright (C) Andrew Morton <akpm@linux-foundation.org>
 * Copyright (C) 2010-2015 Intel Corporation.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/sem.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/shm.h>
#include <sys/syscall.h>
#include <poll.h>
#include "usemem_mincore.h"
#include "usemem_hugepages.h"

#define ALIGN(x,a) (((x)+(a)-1)&~((a)-1))

#define HUGE_PAGE_SIZE (2UL * 1024 * 1024)

#define min(x, y) ({				\
	typeof(x) _min1 = (x);			\
	typeof(y) _min2 = (y);			\
	(void) (&_min1 == &_min2);		\
	_min1 < _min2 ? _min1 : _min2; })

#define max(x, y) ({				\
	typeof(x) _max1 = (x);			\
	typeof(y) _max2 = (y);			\
	(void) (&_max1 == &_max2);		\
	_max1 > _max2 ? _max1 : _max2; })

/* used for remapping the allocated size in remap() */
#define SCALE_FACTOR 10

char *ourname;
int pagesize;
unsigned long done_bytes = 0;
unsigned long opt_bytes = 0;
unsigned long unit = 0;
unsigned long step = 0;
unsigned long *prealloc;
unsigned long *buffer;
int sleep_secs = 0;
time_t runtime_secs = 0;
struct timeval start_time;
int reps = 1;
int do_mlock = 0;
int do_getchar = 0;
int opt_randomise = 0;
int opt_readonly;
int opt_openrw;
int opt_malloc;
int opt_detach;
int opt_advise = 0;
int opt_shm = 0;
int opt_remap = 0;
int opt_mincore = 0;
int opt_mincore_hugepages = 0;
int opt_write_signal_read = 0;
int opt_sync_rw = 0;
int sem_id = -1;
int nr_task;
int nr_thread;
int quiet = 0;
int msync_mode;
char *filename = "/dev/zero";
char *pid_filename;
int map_shared = MAP_PRIVATE;
int map_populate;
int fd;
int ready_fds[2];
int wake_fds[2];


void usage(int ok)
{
	fprintf(stderr,
	"Usage: %s [options] size[k|m|g|t]\n"
	"    -n|--nproc N        do job in N processes\n"
	"    -t|--thread M       do job in M threads\n"
	"    -a|--malloc         obtain memory from malloc()\n"
	"    -f|--file FILE      mmap FILE, default /dev/zero\n"
	"    -F|--prefault       prefault mmap with MAP_POPULATE\n"
	"    -P|--prealloc       allocate memory before fork\n"
	"    -u|--unit SIZE      allocate memory in SIZE chunks\n"
	"    -j|--step SIZE      step size in the read/write loop\n"
	"    -r|--repeat N       repeat read/write N times\n"
	"    -o|--readonly       readonly access\n"
	"    -w|--open-rw        open() and mmap() file in RW mode\n"
	"    -R|--random         random access pattern\n"
	"    -M|--mlock          mlock() the memory\n"
	"    -S|--msync          msync(MS_SYNC) the memory\n"
	"    -A|--msync-async    msync(MS_ASYNC) the memory\n"
	"    -d|--detach         detach before go sleeping\n"
	"    -s|--sleep SEC      sleep SEC seconds when done\n"
	"    -T|--runtime SEC    terminate after SEC seconds\n"
	"    -p|--pid-file FILE  store detached pid to FILE\n"
	"    -g|--getchar        wait for <enter> before quitting\n"
	"    -q|--quiet          operate quietly\n"
	"    -L|--lock           Lock a shared memory using sys V IPC\n"
	"    -D|--advise         advise file access pattern to kernel\n"
	"    -E|--remap          remap file virtual memory\n"
	"    -N|--mincore        get information about pages in memory\n"
	"    -H|--mincore-hgpg   get information abt hugepages in memory\n"
	"    -W|--write-signal-read do write first, then wait for signal to resume and do read\n"
	"    -y|--sync-rw        sync between tasks after allocate memory\n"
	"    -h|--help           show this message\n"
	,		ourname);

	exit(ok);
}

static const struct option opts[] = {
	{ "malloc"	, 0, NULL, 'a' },
	{ "unit"	, 1, NULL, 'u' },
	{ "step"	, 1, NULL, 'j' },
	{ "mlock"	, 0, NULL, 'M' },
	{ "readonly"	, 0, NULL, 'o' },
	{ "open-rw"	, 0, NULL, 'w' },
	{ "quiet"	, 0, NULL, 'q' },
	{ "random"	, 0, NULL, 'R' },
	{ "msync"	, 0, NULL, 'S' },
	{ "msync-async"	, 0, NULL, 'A' },
	{ "nproc"	, 1, NULL, 'n' },
	{ "thread"	, 1, NULL, 't' },
	{ "prealloc"	, 0, NULL, 'P' },
	{ "prefault"	, 0, NULL, 'F' },
	{ "repeat"	, 1, NULL, 'r' },
	{ "file"	, 1, NULL, 'f' },
	{ "pid-file"	, 1, NULL, 'p' },
	{ "detach"	, 0, NULL, 'd' },
	{ "sleep"	, 1, NULL, 's' },
	{ "runtime"	, 1, NULL, 'T' },
	{ "getchar"	, 0, NULL, 'g' },
	{ "Lock"	, 0, NULL, 'L' },
	{ "advice"	, 0, NULL, 'D' },
	{ "remap"	, 0, NULL, 'E' },
	{ "mncr_hgpgs"	, 0, NULL, 'H' },
	{ "sync-rw"	, 0, NULL, 'y' },
	{ "help"	, 0, NULL, 'h' },
	{ NULL		, 0, NULL, 0 }
};

/* print memory lock/unlock menu */
void print_menu() {
	printf("**********************************************************\n");
	printf("*							 *\n");
	printf("*		    Enter an option			 *\n");
	printf("**********************************************************\n");
	printf("*							 *\n");
	printf("*		    a ----> automate			 *\n");
	printf("*		    l ----> locking			 *\n");
	printf("*		    r ----> remapping			 *\n");
	printf("*		    u ----> unlocking			 *\n");
	printf("*		    q ----> quit			 *\n");
	printf("*							 *\n");
	printf("*							 *\n");
	printf("**********************************************************\n\n\n");
	printf("your option: ");
}

/**
 *	memparse - parse a string with mem suffixes into a number
 *	@ptr: Where parse begins
 *	@retptr: (output) Optional pointer to next char after parse completes
 *
 *	Parses a string into a number.	The number stored at @ptr is
 *	potentially suffixed with %K (for kilobytes, or 1024 bytes),
 *	%M (for megabytes, or 1048576 bytes), or %G (for gigabytes, or
 *	1073741824).  If the number is suffixed with K, M, or G, then
 *	the return value is the number multiplied by one kilobyte, one
 *	megabyte, or one gigabyte, respectively.
 */

unsigned long long memparse(const char *ptr, char **retptr)
{
	char *endptr;	/* local pointer to end of parsed string */

	unsigned long long ret = strtoull(ptr, &endptr, 0);

	switch (*endptr) {
	case 'T':
	case 't':
		ret <<= 10;
	case 'G':
	case 'g':
		ret <<= 10;
	case 'M':
	case 'm':
		ret <<= 10;
	case 'K':
	case 'k':
		ret <<= 10;
		endptr++;
	default:
		break;
	}

	if (retptr)
		*retptr = endptr;

	return ret;
}

static inline void os_random_seed(unsigned long seed, struct drand48_data *rs)
{
	srand48_r(seed, rs);
}

static inline long os_random_long(unsigned long max, struct drand48_data *rs)
{
	long val;

	lrand48_r(rs, &val);
	return (unsigned long)((double)max * val / (RAND_MAX + 1.0));
}

/*
 * semaphore get/put wrapper
 */
int down(int sem_id)
{
	struct sembuf buf = {
		.sem_num = 0,
		.sem_op  = -1,
		.sem_flg = SEM_UNDO,
	};
	return semop(sem_id, &buf, 1);
}

int up(int sem_id)
{
	struct sembuf buf = {
		.sem_num = 0,
		.sem_op  = 1,
		.sem_flg = SEM_UNDO,
	};
	return semop(sem_id, &buf, 1);
}

void update_pid_file(pid_t pid)
{
	FILE *file;

	if (!pid_filename)
		return;

	file = fopen(pid_filename, "a");
	if (!file) {
		perror(pid_filename);
		exit(1);
	}

	fprintf(file, "%d\n", pid);
	fclose(file);
}


void sighandler(int sig, siginfo_t *si, void *arg)
{
	up(sem_id);

	printf("usemem: exit on signal %d\n", sig);
	exit(0);
}

void detach(void)
{
	pid_t pid;

	sem_id = semget(IPC_PRIVATE, 1, 0666|IPC_CREAT|IPC_EXCL);
	if (sem_id == -1) {
		perror("semget");
		exit(1);
	}
	if (semctl(sem_id, 0, SETVAL, 0) == -1) {
		perror("semctl");
		if (semctl(sem_id, 0, IPC_RMID) == -1)
			perror("sem_id IPC_RMID");
		exit(1);
	}

	pid = fork();
	if (pid < 0) {
		perror("fork");
		exit(1);
	}

	if (pid) { /* parent */
		update_pid_file(pid);
		if (down(sem_id))
			perror("down");
		semctl(sem_id, 0, IPC_RMID);
		exit(0);
	}

	if (pid == 0) { /* child */
		struct sigaction sa = {
			.sa_sigaction = sighandler,
			.sa_flags = SA_SIGINFO,
		};

		/*
		 * XXX: SIGKILL cannot be caught.
		 * The parent will wait for ever if child is OOM killed.
		 */
		sigaction(SIGINT, &sa, NULL);
	}
}

unsigned long * allocate(unsigned long bytes)
{
	unsigned long *p;

	if (!bytes)
		return NULL;

	if (opt_malloc) {
		p = malloc(bytes);
		if (p == NULL) {
			perror("malloc");
			exit(1);
		}
	} else {
		p = mmap(NULL, bytes, (opt_readonly && !opt_openrw) ?
			 PROT_READ : PROT_READ|PROT_WRITE,
			 map_shared|map_populate, fd, 0);
		if (p == MAP_FAILED) {
			fprintf(stderr, "%s: mmap failed: %s\n",
				ourname, strerror(errno));
			exit(1);
		}
		p = (unsigned long *)ALIGN((unsigned long)p, pagesize - 1);
	}

	if (do_mlock && mlock(p, bytes) < 0) {
		perror("mlock");
		exit(1);
	}

	return p;
}

int runtime_exceeded(void)
{
	struct timeval now;

	if (!runtime_secs || start_time.tv_sec == 0)
		return 0;

	gettimeofday(&now, NULL);
	return (now.tv_sec - start_time.tv_sec > runtime_secs);
}

/* Function to allocate sys V IPC shared object of size "bytes" */

void shm_allocate_and_lock(unsigned long bytes)
{
	unsigned long *p = NULL;	    /* initialize to NULL */
	unsigned long itr;
	int page_size;

	if (!bytes) {
		perror("invalid size\n");
		exit(1);
	}

	/* create a shared sys V IPC shared object of size "bytes"*/

	/* Check if the shmget call was successful */
	if ((seg_id = shmget(IPC_PRIVATE, bytes, IPC_CREAT|IPC_EXCL)) == EINVAL) {
		fprintf(stderr, "SHM creation failed with error :%s\n", strerror(errno));
		exit(1);
	}

	/* Attach the shared memory to callers virtual address space */
	/* Check if the attachment was successful */
	if ((p = shmat(seg_id, NULL, SHM_RND)) == (void *) -1) {
		fprintf(stderr, "SHM attachment failed with error :%s\n", strerror(errno));
		shm_free(seg_id);
		exit(1);
	} else {
		/* Attach the shared segment to virtual space of process at p */
		page_size = getpagesize();

		/* Touch each page atleast once */
		for (itr = 0; itr < bytes; itr += page_size)
			*((char *)p + itr + 1) = 1;
	}

	if (shmctl(seg_id, SHM_LOCK, NULL) < 0) {
		perror("Error in SYS V SHM locking\n");
		exit(1);
	}
}

/* Unlock a locked SYS V IPC shared memory */
void shm_unlock(int seg_id)
{
	if (seg_id < 0) {
		perror("Invalid seg_id\n");
		exit(1);
	}

	/* Unlock shared memory segment */
	shmctl(seg_id, SHM_UNLOCK, NULL);
}

static unsigned long do_rw_once(unsigned long *p, unsigned long bytes,
				struct drand48_data *rand_data, int read,
				int *rep, int reps)
{
	unsigned long i;
	unsigned long m = bytes / sizeof(*p);
	unsigned long rw_bytes = 0;

	for (i = 0; i < m; i += step / sizeof(*p)) {
		volatile long d;
		unsigned long idx = i;

		if (opt_randomise)
			idx = os_random_long(m - 1, rand_data);

		/* verify last write */
		if (rep && *rep && !read && !opt_randomise && p[idx] != idx) {
			fprintf(stderr, "Data wrong at offset 0x%lx. "
					"Expected 0x%08lx, got 0x%08lx\n",
					idx * sizeof(*p), idx, p[idx]);
			exit(1);
		}

		/* read / write */
		if (read)
			d = p[idx];	/* read data into d */
		else
			p[idx] = idx;	/* write data into p[idx] */

		rw_bytes += sizeof(*p);

		if (!(i & 0xffff) && runtime_exceeded()) {
			if (rep)
				*rep = reps;
			break;
		}
	}

	return rw_bytes;
}

/* This is copied from hackbench, Thanks Ingo! */
static void ready(void)
{
	char dummy = '*';
	struct pollfd pollfd = { .fd = wake_fds[0], .events = POLLIN };

	/* Tell them we're ready. */
	if (write(ready_fds[1], &dummy, 1) != 1) {
		perror("write pipe");
		exit(1);
	}

	/* Wait for "GO" signal */
	if (poll(&pollfd, 1, -1) != 1) {
                perror("poll");
		exit(1);
	}
}

unsigned long do_unit(unsigned long bytes, struct drand48_data *rand_data)
{
	unsigned long rw_bytes;
	unsigned long *p;
	int rep;
	int c;

	if (prealloc) {
		p = prealloc;
		rw_bytes = 0;
	} else {
		p = allocate(bytes);
		rw_bytes = bytes / 8;
	}

	if (opt_sync_rw)
		ready();

	if (opt_write_signal_read)
		buffer = p;

	for (rep = 0; rep < reps; rep++) {
		if (rep > 0 && !quiet) {
			printf(".");
			fflush(stdout);
		}

		rw_bytes += do_rw_once(p, bytes, rand_data, opt_readonly, &rep, reps);

		if (msync_mode) {
			if ((msync(p, bytes, msync_mode)) == -1) {
				fprintf(stderr, "msync failed with error %s \n", strerror(errno));
				exit(1);
			}
		}
	}

	if (do_getchar) {

		print_menu();

		while (1) {

			if ((c = getchar()) == '\n') continue;

			switch (c) {
			case 'a':
				/*  select this case for an automatic lock, remap and unlock */
				mlock(p, bytes);
				printf("locking memory ..... done!\n\n");
				if ((p = mremap(p, bytes, SCALE_FACTOR * bytes, MREMAP_MAYMOVE)) == MAP_FAILED) {
					fprintf(stderr, "remap failed: %s\n\n", strerror(errno));
					exit(1);
				}

				bytes = SCALE_FACTOR * bytes;
				printf("remapping locked memory.....done!\n\n");
				munlock(p, bytes);
				printf("unlocking memory ..... done!\n\n");
				return rw_bytes;
			case 'u':
				munlock(p, bytes);
				printf("unlocking memory ..... done!\n");
				print_menu();
				break;
			case 'l':
				mlock(p, bytes);
				printf("locking memory ..... done!\n");
				print_menu();
				break;
			case 'r':
				if ((p = mremap(p, bytes, SCALE_FACTOR * bytes, MREMAP_MAYMOVE)) == MAP_FAILED) {
					fprintf(stderr, "remap failed: %s\n", strerror(errno));
					exit(1);
				}

				bytes = SCALE_FACTOR * bytes;
				printf("remapping locked memory.....done!\n");
				print_menu();
				break;
			case 'q':
				printf("Quitting now\n");
				return rw_bytes;
			}
		}
	}

	return rw_bytes;
}

static void output_statistics(unsigned long unit_bytes)
{
	struct timeval stop;
	char buf[1024];
	size_t len;
	unsigned long delta_us;
	unsigned long throughput;

	gettimeofday(&stop, NULL);
	delta_us = (stop.tv_sec - start_time.tv_sec) * 1000000 +
		(stop.tv_usec - start_time.tv_usec);
	throughput = ((unit_bytes * 1000000ULL) >> 10) / delta_us;
	len = snprintf(buf, sizeof(buf),
			"%lu bytes / %lu usecs = %lu KB/s\n",
			unit_bytes, delta_us, throughput);
	fflush(stdout);
	write(1, buf, len);
}

static void wait_for_sigusr1(int signal) {}

long do_units(void)
{
	struct drand48_data rand_data;
	unsigned long unit_bytes = done_bytes;
	unsigned long bytes = opt_bytes;

	if (opt_detach)
		detach();

	/* Base the random seed on the thread ID for multithreaded tests */
	if (opt_randomise)
		os_random_seed(time(0) ^ syscall(SYS_gettid), &rand_data);

	if (!unit)
		unit = bytes;

	if (!opt_sync_rw)
		ready();

	/*
	 * Allow a bytes=0 pass for pure fork bomb:
	 * usemem -n 10000 0 --detach --sleep 10
	 */
	do {
		unsigned long size = min(bytes, unit);

		unit_bytes += do_unit(size, &rand_data);
		bytes -= size;
		if (runtime_exceeded())
			break;
	} while (bytes);

	if (!opt_write_signal_read && unit_bytes)
		output_statistics(unit_bytes);

	if (opt_write_signal_read) {
		struct sigaction act;
		memset(&act, 0, sizeof(act));
		act.sa_handler = wait_for_sigusr1;
		if (sigaction(SIGUSR1, &act, NULL) == -1) {
			perror("sigaction");
			exit(-1);
		}
	}

	if (opt_detach && up(sem_id))
		perror("up");

	if (sleep_secs)
		sleep(sleep_secs);

	if (opt_write_signal_read) {
		sigset_t set;
		sigfillset(&set);
		sigdelset(&set, SIGUSR1);
		sigsuspend(&set);
		gettimeofday(&start_time, NULL);
		unit_bytes = do_rw_once(buffer, opt_bytes, &rand_data, 1, NULL, 0);
		output_statistics(unit_bytes);
	}

	return 0;
}

typedef void * (*start_routine)(void *);

int do_task(void)
{
	pthread_t threads[nr_thread];
	long thread_ret;
	int ret;
	int i;

	if (!nr_thread)
		return do_units();

	for (i = 0; i < nr_thread; i++) {
		ret = pthread_create(&threads[i], NULL, (start_routine)do_units, NULL);
		if (ret) {
			perror("pthread_create");
			exit(1);
		}
	}
	for (i = 0; i < nr_thread; i++) {
		ret = pthread_join(threads[i], (void *)&thread_ret);
		if (ret) {
			perror("pthread_join");
			exit(1);
		}
	}

	return 0;
}

int do_tasks(void)
{
	int i;
	int status;
	int child_pid;
	char dummy;

	for (i = 0; i < nr_task; i++) {
		if ((child_pid = fork()) == 0) {
			return do_task();
		}
	}

	for (i = 0; i < nr_task * nr_thread; i++)
		if (read(ready_fds[0], &dummy, 1) != 1) {
                        perror("read pipe");
			return 1;
		}

	/* Fire! */
	if (write(wake_fds[1], &dummy, 1) != 1) {
                perror("write pipe");
		return 1;
	}

	for (i = 0; i < nr_task; i++) {
		if (wait3(&status, 0, 0) < 0) {
			if (errno != EINTR) {
				printf("wait3 error on %dth child\n", i);
				perror("wait3");
				return 1;
			}
		}
	}

	return 0;
}

int main(int argc, char *argv[])
{
	int c;

#ifdef DBG
	/* print the command line parameters passed on to main */
	int itr = 0;
	printf("main() invoked with %d arguments and they are as follows \n", argc);
	for (itr = 0; itr < argc; itr++) {
		printf("arg[%d] is	----->	%s\n", itr, argv[itr]);
	}
#endif

	ourname = argv[0];
	pagesize = getpagesize();

	while ((c = getopt_long(argc, argv,
				"aAf:FPp:gqowRMm:n:t:ds:T:Sr:u:j:EHDNLWyh", opts, NULL)) != -1)
		{
		switch (c) {
		case 'a':
			opt_malloc++;
			break;
		case 'u':
			unit = memparse(optarg, NULL);
			break;
		case 'j':
			step = memparse(optarg, NULL);
			break;
		case 'A':
			msync_mode = MS_ASYNC;
			break;
		case 'f':
			filename = optarg;
			map_shared = MAP_SHARED;
			break;
		case 'p':
			pid_filename = optarg;
			break;
		case 'g':
			do_getchar = 1;
			break;
		case 'm': /* kept for compatibility */
			opt_bytes = strtol(optarg, NULL, 10);
			opt_bytes <<= 20;
			break;
		case 'n':
			nr_task = strtol(optarg, NULL, 10);
			break;
		case 't':
			nr_thread = strtol(optarg, NULL, 10);
			break;
		case 'P':
			prealloc++;
			break;
		case 'F':
			map_populate = MAP_POPULATE;
			break;
		case 'M':
			do_mlock = 1;
			break;
		case 'q':
			quiet = 1;
			break;
		case 's':
			sleep_secs = strtol(optarg, NULL, 10);
			break;
		case 'T':
			runtime_secs = strtol(optarg, NULL, 10);
			break;
		case 'd':
			opt_detach = 1;
			break;
		case 'r':
			reps = strtol(optarg, NULL, 10);
			break;
		case 'R':
			opt_randomise++;
			break;
		case 'S':
			msync_mode = MS_SYNC;
			break;
		case 'o':
			opt_readonly++;
			break;
		case 'w':
			opt_openrw++;
			break;
		case 'h':
			usage(0);
			break;
		case 'L':
			opt_shm = 1;
			break;
		case 'D':
			filename = argv[optind];
			opt_advise = 1;
			break;

		case 'E':
			opt_remap = 1;
			break;

		case 'N':
			opt_mincore = 1;
			break;

		case 'H':
			opt_mincore_hugepages = 1;
			break;

		case 'W':
			opt_write_signal_read = 1;
			break;

		case 'y':
			opt_sync_rw = 1;
			break;

		default:
			usage(1);
		}
	}

	if (pipe(ready_fds) || pipe(wake_fds)) {
		fprintf(stderr, "%s: failed to create pipes: %s\n",
			ourname, strerror(errno));
		exit(1);
	}

	if (step < sizeof(long))
		step = sizeof(long);

	if (optind != argc - 1)
		usage(0);

	if (!opt_write_signal_read)
		gettimeofday(&start_time, NULL);

	opt_bytes = memparse(argv[optind], NULL);

	if (opt_sync_rw && unit && unit != opt_bytes) {
		fprintf(stderr, "%s: to sync tasks after allocation, unit must equal total size\n",
			ourname);
		exit(1);
	}

	if (!opt_malloc)
		fd = open(filename, ((opt_readonly && !opt_openrw) ?
			  O_RDONLY : O_RDWR) | O_CREAT, 0666);
	if (fd < 0) {
		fprintf(stderr, "%s: failed to open `%s': %s\n",
			ourname, filename, strerror(errno));
		exit(1);
	}

	if (prealloc) {
		prealloc = allocate(opt_bytes);
		done_bytes = opt_bytes / 8;
	}

	if (opt_remap) {
		prealloc = mremap(prealloc, opt_bytes, SCALE_FACTOR * opt_bytes, MREMAP_MAYMOVE);
		done_bytes += opt_bytes / 8;
	}

	/* Allocate a shared sysV IPC shared object of size "opt_bytes" */
	if (opt_shm) {
		shm_allocate_and_lock(opt_bytes);

		/* Unlocking memory now */
		shm_unlock(seg_id);

		/* mark for destruction */
		shm_free(seg_id);

		done_bytes += opt_bytes / 8;
	}

	/* Advise file access pattern */
	if (opt_advise) {
		/* advice : WILLNEED */
		if ((posix_fadvise(fd, 0, 0, POSIX_FADV_WILLNEED)) == -1) {
			fprintf(stderr, "posix_advise() error : %s\n", strerror(errno));
			exit(1);
		}

		/* close the file */
		close(fd);

		/* open again for POSIX_FADV_DONTNEED */
		if ((fd = open(filename, ((opt_readonly && !opt_openrw) ?
					 O_RDONLY : O_RDWR) | O_CREAT, 0666)) < 0) {
			fprintf(stderr, "%s: failed to open `%s': %s\n",
				ourname, filename, strerror(errno));
			exit(1);
		}

		/* advise : DONTNEED*/
		if ((posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED)) == -1) {
			fprintf(stderr, "posix_advise() error : %s\n", strerror(errno));
			exit(1);
		}

		done_bytes += opt_bytes;
		close(fd);
	}

	if (opt_mincore) {
		unsigned long length_of_vector = ((opt_bytes/getpagesize()) + 1);
		unsigned char *ptr = NULL; /* initialize to NULL */

		if ((ptr = (unsigned char *)malloc((sizeof(char)) * length_of_vector)) == NULL) {
			fprintf(stderr, "Unable to allocate requested memory");
			fprintf(stdout, "exiting now...");
			exit(1);
		}

		if ((mincore(prealloc, opt_bytes, ptr)) == -1) {
			fprintf(stderr, "mincore failed with error: %s", strerror(errno));
			free(ptr);
			exit(1);
		}

		done_bytes += opt_bytes / 8;

		free(ptr);
		return 0;
	}

	if (opt_mincore_hugepages) {
		int i;
		int number_of_pages;

		unsigned char *ptr;
		unsigned char *p = (unsigned char *)allocate_hugepage_segment(opt_bytes);
		unsigned long pagesize = HUGE_PAGE_SIZE;

		char modify_nr_hugepages[50];

		/* error checking done inside the mincore_hugepages function */
		ptr = mincore_hugepages(p, opt_bytes);

		/* change protection of the hugepage segment */
		if (mprotect(p, opt_bytes, PROT_WRITE) == -1) {
			fprintf(stderr, "mprotect error: %s", strerror(errno));
			exit(1);
		}

		number_of_pages = opt_bytes/pagesize;

		/* copy on write for allocating child address space */
		for (i = 0; i < number_of_pages; i++)
			p[i * pagesize] = (char) 1;

		sprintf(modify_nr_hugepages, "echo 5 > /proc/sys/vm/nr_hugepages\n");

		if ((system(modify_nr_hugepages)) == -1) {
			fprintf(stderr, "bash command failed\n");
			exit(1);
		}

		done_bytes += opt_bytes / 8;

		/* free the shm segment */
		shm_free(seg_id);
		return 0;		/* return with out doing anything */
	}

	if (!nr_task)
		nr_task = 1;

	return do_tasks();
}
