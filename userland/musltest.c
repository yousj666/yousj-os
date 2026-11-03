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
    printf("MUSLTEST PASS counter=%ld\n", counter);
    return 0;
}
