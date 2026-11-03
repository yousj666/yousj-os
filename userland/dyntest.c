/* M58: dynamically linked musl program. The kernel loads
 * /lib/ld-musl-x86_64.so.1 (PT_INTERP), the loader maps
 * libc.so, relocates everything, and only then does this
 * code run. Exercises: stdio via the shared libc, malloc
 * across the library boundary, and a pthread whose code
 * lives in the main image but whose machinery is libc's. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static long g_counter;
static __thread int g_tls;

static void *worker(void *arg)
{
    long id = (long)arg;
    int i;
    g_tls = (int)(id * 100);
    for (i = 0; i < 2000; i++) {
        pthread_mutex_lock(&g_lock);
        g_counter++;
        pthread_mutex_unlock(&g_lock);
    }
    if (g_tls != (int)(id * 100))
        return (void *)99;
    return (void *)(id + 3);
}

int main(void)
{
    pthread_t th[2];
    char *buf;
    long i;
    void *ret = NULL;

    printf("DYNTEST start\n");
    buf = malloc(4096);
    if (buf == NULL) {
        printf("DYNTEST FAIL malloc\n");
        return 1;
    }
    memset(buf, 0x5a, 4096);
    for (i = 0; i < 2; i++) {
        if (pthread_create(&th[i], NULL, worker,
                           (void *)(i + 1)) != 0) {
            printf("DYNTEST FAIL create\n");
            return 1;
        }
    }
    for (i = 0; i < 2; i++) {
        if (pthread_join(th[i], &ret) != 0 ||
            ret != (void *)(i + 1 + 3)) {
            printf("DYNTEST FAIL join\n");
            return 1;
        }
    }
    free(buf);
    if (g_counter != 4000) {
        printf("DYNTEST FAIL counter\n");
        return 1;
    }
    printf("DYNTEST PASS counter=4000\n");
    return 0;
}
