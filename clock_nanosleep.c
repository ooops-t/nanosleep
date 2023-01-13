#define _GUN_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>

#define USEC_PER_SEC 1000000
#define NSEC_PER_SEC 1000000000

#define DEFAULT_CLOCK CLOCK_MONOTONIC

static inline void tsnorm(struct timespec *ts) {
  while (ts->tv_nsec >= NSEC_PER_SEC) {
    ts->tv_nsec -= NSEC_PER_SEC;
    ts->tv_sec++;
  }
}

static inline int tsgreater(struct timespec *a, struct timespec *b) {
  return ((a->tv_sec > b->tv_sec) ||
          (a->tv_sec == b->tv_sec && a->tv_nsec > b->tv_nsec));
}

static inline int64_t calcdiff_ns(struct timespec t1, struct timespec t2) {
  int64_t diff;
  diff = NSEC_PER_SEC * (int64_t)((int)t1.tv_sec - (int)t2.tv_sec);
  diff += ((int)t1.tv_nsec - (int)t2.tv_nsec);
  return diff;
}

static inline int64_t calcdiff(struct timespec t1, struct timespec t2) {
  int64_t diff;
  diff = USEC_PER_SEC * (long long)((int)t1.tv_sec - (int)t2.tv_sec);
  diff += ((int)t1.tv_nsec - (int)t2.tv_nsec) / 1000;
  return diff;
}

static int config_process() {
  /* Set priority */
  struct sched_param param = {};
  param.sched_priority = 98;

  printf("Using priority %i.\n", param.sched_priority);
  if (sched_setscheduler(0, SCHED_FIFO, &param) == -1) {
    perror("sched_setscheduler failed");
    return -1;
  }

  return 0;
}

static int latency_target_fd = -1;
static int32_t latency_target_value = 0;
static void set_latency_target(void) {
  struct stat s;
  int err;

  errno = 0;
  err = stat("/dev/cpu_dma_latency", &s);
  if (err == -1) {
    fprintf(stderr, "WARN: stat /dev/cpu_dma_latency failed");
    return;
  }

  errno = 0;
  latency_target_fd = open("/dev/cpu_dma_latency", O_RDWR);
  if (latency_target_fd == -1) {
    fprintf(stderr, "WARN: open /dev/cpu_dma_latency");
    return;
  }

  errno = 0;
  err = write(latency_target_fd, &latency_target_value, 4);
  if (err < 1) {
    fprintf(stderr, "# error setting cpu_dma_latency to %d!\n",
            latency_target_value);
    close(latency_target_fd);
    return;
  }
  printf("# /dev/cpu_dma_latency set to %dus\n", latency_target_value);
}

static int check_timer(void) {
  struct timespec ts;

  if (clock_getres(CLOCK_MONOTONIC, &ts))
    return 1;

  return (ts.tv_sec != 0 || ts.tv_nsec != 1);
}

/*
 * Raise the soft priority limit up to prio, if that is less than or equal
 * to the hard limit
 * if a call fails, return the error
 * if successful return 0
 * if fails, return -1
 */
static int raise_soft_prio(int policy, const struct sched_param *param) {
  int err;
  int policy_max; /* max for scheduling policy such as SCHED_FIFO */
  int soft_max;
  int hard_max;
  int prio;
  struct rlimit rlim;

  prio = param->sched_priority;

  policy_max = sched_get_priority_max(policy);
  if (policy_max == -1) {
    err = errno;
    fprintf(stderr, "WARN: no such policy\n");
    return err;
  }

  err = getrlimit(RLIMIT_RTPRIO, &rlim);
  if (err) {
    err = errno;
    fprintf(stderr, "WARN: getrlimit failed");
    return err;
  }

  soft_max = (rlim.rlim_cur == RLIM_INFINITY) ? policy_max : rlim.rlim_cur;
  hard_max = (rlim.rlim_max == RLIM_INFINITY) ? policy_max : rlim.rlim_max;

  if (prio > soft_max && prio <= hard_max) {
    rlim.rlim_cur = prio;
    err = setrlimit(RLIMIT_RTPRIO, &rlim);
    if (err) {
      err = errno;
      fprintf(stderr, "WARN: setrlimit failed");
      /* return err; */
    }
  } else {
    err = -1;
  }

  return err;
}

/*
 * Check the error status of sched_setscheduler
 * If an error can be corrected by raising the soft limit priority to
 * a priority less than or equal to the hard limit, then do so.
 */
static int setscheduler(pid_t pid, int policy,
                        const struct sched_param *param) {
  int err = 0;

try_again:
  err = sched_setscheduler(pid, policy, param);
  if (err) {
    err = errno;
    if (err == EPERM) {
      int err1;
      err1 = raise_soft_prio(policy, param);
      if (!err1)
        goto try_again;
    }
  }

  return err;
}

static uint64_t diff, diff_us;
static uint64_t max = 0, min = 1000000, cycles;
static double avg;
static pthread_t thread;

void *threadcalc(void *param) {
  (void)param;

  int ret;
  struct timespec now, next, interval;
  cpu_set_t mask;
  struct sched_param schedp;

  CPU_ZERO(&mask);
  CPU_SET(1, &mask);
  thread = pthread_self();
  if (pthread_setaffinity_np(thread, sizeof(mask), &mask) == -1)
    fprintf(stderr, "Could not set CPU affinity to CPU #%d\n", 1);

  pthread_setname_np(thread, "nanosleep");

  memset(&schedp, 0, sizeof(schedp));
  schedp.sched_priority = 98;
  if (setscheduler(0, SCHED_FIFO, &schedp))
    fprintf(stderr, "timerthread%d: failed to set priority to %d\n", 1, 98);

  interval.tv_sec = 1000 / USEC_PER_SEC;
  interval.tv_nsec = (1000 % USEC_PER_SEC) * 1000;

  clock_gettime(DEFAULT_CLOCK, &now);

  next = now;
  next.tv_sec += interval.tv_sec;
  next.tv_nsec += interval.tv_nsec;
  tsnorm(&next);

  while (1) {
    ret = clock_nanosleep(DEFAULT_CLOCK, TIMER_ABSTIME, &next, NULL);
    if (ret != 0) { // Oversleeping
      if (ret == EINTR)
        syslog(LOG_INFO, "Interrupted by signal handler\n");
      else
        syslog(LOG_INFO, "clock_nanosleep:errno=%d\n", ret);
    }

    if ((ret = clock_gettime(DEFAULT_CLOCK, &now))) {
      if (ret != EINTR)
        fprintf(stderr, "clock_getttime() failed. errno: %d\n", errno);
    }

    // diff = calcdiff_ns(now, next);
    diff_us = calcdiff(now, next);

    if (diff_us > max)
      max = diff_us;
    if (diff_us < min)
      min = diff_us;

    avg += (double)diff_us;

    cycles++;
    next.tv_sec += interval.tv_sec;
    next.tv_nsec += interval.tv_nsec;
    tsnorm(&next);

    while (tsgreater(&now, &next)) {
      next.tv_sec += interval.tv_sec;
      next.tv_nsec += interval.tv_nsec;
      tsnorm(&next);
    }
  }
  return NULL;
}

int main(int argc, char *argv[]) {
  (void)argc;
  (void)argv;

  pthread_t thread;

  if (mlockall(MCL_CURRENT | MCL_FUTURE) == -1) {
    perror("mlockall\n");
    return -1;
  }

  set_latency_target();

  if (check_timer())
    fprintf(stdout, "High resolution timers not available\n");

  pthread_create(&thread, NULL, threadcalc, NULL);

  while (1) {
    printf("\033[%dA", 3);

    fprintf(stdout, "Min:%7lld Act:%8lld Avg:%8ld Max:%8lld\n", min, diff_us,
            cycles ? (long)(avg / cycles) : 0, max);
    usleep(10000);
  }

  /* close the latency_target_fd if it's open */
  if (latency_target_fd >= 0)
    close(latency_target_fd);

  return 0;
}
