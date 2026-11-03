/* musltest.c -- M56: a REAL libc program (musl 1.2.5,
 * statically linked) exercising the pthread surface the
 * M50-M55 milestones built: pthread_create/join over the
 * full clone flag set, a contended mutex over futex,
 * and __thread TLS over arch_prctl SET_FS.
 *
 * Built by a dedicated Makefile rule against the musl
 * tree in build/musl (Alpine musl-dev 1.2.5), NOT by the
 * freestanding userland rules. Prints MUSLTEST PASS and
 * the final counter; the harness checks both. */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static long counter;
static __thread long tls_val;

static void *worker(void *arg)
{
    long id = (long)arg;
    int i;
    tls_val = (id + 1) * 1000;
    for (i = 0; i < 5000; i++) {
        pthread_mutex_lock(&mu);
        counter++;
        pthread_mutex_unlock(&mu);
    }
    if (tls_val != (id + 1) * 1000)
        return (void *)0xDEAD;
    return (void *)(id + 7);
}

/* M61: robust mutex owner death. The doomed thread locks
 * the robust mutex and exits holding it; the kernel's exit
 * walk must mark it owner-died, so the main thread's lock
 * returns EOWNERDEAD (not a hang, not success), and after
 * pthread_mutex_consistent the mutex works again. */
static pthread_mutex_t rmu;

static void *doomed(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&rmu);
    return (void *)0; /* exit still holding the lock */
}

static int robust_scenario(void)
{
    pthread_mutexattr_t at;
    pthread_t th;
    void *ret;
    int rc;

    pthread_mutexattr_init(&at);
    pthread_mutexattr_setrobust(&at, PTHREAD_MUTEX_ROBUST);
    if (pthread_mutex_init(&rmu, &at) != 0)
        return 1;
    if (pthread_create(&th, 0, doomed, 0) != 0)
        return 2;
    pthread_join(th, &ret);
    rc = pthread_mutex_lock(&rmu);
    if (rc != EOWNERDEAD) {
        printf("MUSLTEST robust FAIL rc=%d\n", rc);
        return 3;
    }
    if (pthread_mutex_consistent(&rmu) != 0)
        return 4;
    if (pthread_mutex_unlock(&rmu) != 0)
        return 5;
    if (pthread_mutex_lock(&rmu) != 0)
        return 6;
    pthread_mutex_unlock(&rmu);
    return 0;
}

int main(void)
{
    pthread_t th[3];
    long i;
    void *ret;

    printf("MUSLTEST start\n");
    for (i = 0; i < 3; i++) {
        if (pthread_create(&th[i], 0, worker, (void *)i) != 0) {
            printf("MUSLTEST create FAIL\n");
            return 1;
        }
    }
    for (i = 0; i < 3; i++) {
        if (pthread_join(th[i], &ret) != 0 ||
            ret != (void *)(i + 7)) {
            printf("MUSLTEST join FAIL\n");
            return 1;
        }
    }
    if (counter != 15000) {
        printf("MUSLTEST counter FAIL %ld\n", counter);
        return 1;
    }
    i = robust_scenario();
    if (i != 0) {
        printf("MUSLTEST robust FAIL step %ld\n", i);
        return 1;
    }
    printf("MUSLTEST PASS counter=%ld\n", counter);
    return 0;
}
