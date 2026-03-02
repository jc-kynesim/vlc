#include "pollqueue.h"

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/eventfd.h>

#define request_log(...) fprintf(stderr, __VA_ARGS__)

struct pollqueue;

enum polltask_state {
    POLLTASK_UNQUEUED = 0,
    POLLTASK_QUEUED,
    POLLTASK_RUNNING,
    POLLTASK_Q_KILL,
    POLLTASK_Q_DEAD,
    POLLTASK_RUN_KILL,
    POLLTASK_EXIT,
};

// Run this task once and auto delete it once run
#define POLLTASK_FLAG_ONCE      1
// This polltask does not keep a ref to its Q
// This means that it does not block queue deletion and will run on thread
// exit with revents = -1. Caller is reponsible for ensuring that the queue
// is valid for the duration of the tasks existence.
#define POLLTASK_FLAG_NO_REF    2

struct polltask {
    struct polltask *next;
    struct polltask *prev;
    struct pollqueue *q;
    enum polltask_state state;

    int fd;
    short events;
    unsigned short flags;

    void (*fn)(void *v, short revents);
    void * v;

    uint64_t timeout; /* CLOCK_MONOTONIC time, 0 => never */
};

struct pollqueue {
    atomic_int ref_count;
    pthread_mutex_t lock;
    pthread_cond_t cond;

    struct polltask *head;
    struct polltask *tail;

    struct prepost_ss {
        void (*pre)(void *v, struct pollfd *pfd);
        void (*post)(void *v, short revents);
        void *v;
    } prepost;

    void (* exit_fn)(void * v);
    void * exit_v;

    // On thread exit do not detach - counted value allows us to cope with
    // multiple simultainious finish calls. Only poked from poll_thread so
    // no need to lock.
    int join_req;

    bool kill;
    bool no_prod;

    bool sig_seq; // Signal cond when seq incremented
    uint32_t seq;

    int prod_fd;
    struct polltask *prod_pt;
    pthread_t worker;
};

static struct polltask *
polltask_new2(struct pollqueue *const pq,
              const int fd, const short events,
              void (*const fn)(void *v, short revents),
              void *const v,
              const unsigned short flags)
{
    struct polltask *pt;

    if (!events && fd != -1)
        return NULL;

    pt = malloc(sizeof(*pt));
    if (!pt)
        return NULL;

    *pt = (struct polltask){
        .next = NULL,
        .prev = NULL,
        .q = (flags & POLLTASK_FLAG_NO_REF) != 0 ? pq : pollqueue_ref(pq),
        .fd = fd,
        .events = events,
        .flags = flags,
        .fn = fn,
        .v = v
    };

    return pt;
}

struct polltask *
polltask_new(struct pollqueue *const pq,
             const int fd, const short events,
             void (*const fn)(void *v, short revents),
             void *const v)
{
    return polltask_new2(pq, fd, events, fn, v, 0);
}

struct polltask *polltask_new_timer(struct pollqueue *const pq,
                  void (*const fn)(void *v, short revents),
                  void *const v)
{
    return polltask_new(pq, -1, 0, fn, v);
}

int pollqueue_timer_once(struct pollqueue *const pq,
                         void (*const fn)(void *v, short revents),
                         void *const v,
                         const int timeout_ms)
{
    struct polltask * const pt = polltask_new2(pq, -1, 0, fn, v, POLLTASK_FLAG_ONCE);
    if (pt == NULL)
        return -EINVAL;
    pollqueue_add_task(pt, timeout_ms);
    return 0;
}

int
pollqueue_callback_once(struct pollqueue *const pq,
                        void (*const fn)(void *v, short revents),
                        void *const v)
{
    return pollqueue_timer_once(pq, fn, v, 0);
}

static void pollqueue_rem_task(struct pollqueue *const pq, struct polltask *const pt)
{
    if (pt->prev)
        pt->prev->next = pt->next;
    else
        pq->head = pt->next;
    if (pt->next)
        pt->next->prev = pt->prev;
    else
        pq->tail = pt->prev;
    pt->next = NULL;
    pt->prev = NULL;
}

static void polltask_free(struct polltask * const pt)
{
    free(pt);
}

static void polltask_kill(struct polltask * const pt)
{
    struct pollqueue * pq = (pt->flags & POLLTASK_FLAG_NO_REF) != 0 ? NULL : pt->q;
    polltask_free(pt);
    pollqueue_unref(&pq);
}

static void polltask_dead(struct polltask * const pt)
{
    pt->state = POLLTASK_Q_DEAD;
    pthread_cond_broadcast(&pt->q->cond);
}

static void pollqueue_prod(const struct pollqueue *const pq)
{
    static const uint64_t one = 1;
    int rv;
    while ((rv = write(pq->prod_fd, &one, sizeof(one))) != sizeof(one)) {
        if (!(rv == -1 && errno == EINTR))
            break;
    }
}

static bool am_in_thread(const struct pollqueue * const pq)
{
    return pthread_equal(pthread_self(), pq->worker);
}

void polltask_delete(struct polltask **const ppt)
{
    struct polltask *const pt = *ppt;
    struct pollqueue * pq;
    enum polltask_state state;
    bool prodme;
    bool inthread;

    if (!pt)
        return;

    pq = pt->q;
    inthread = am_in_thread(pq);

    pthread_mutex_lock(&pq->lock);
    state = pt->state;
    pt->state = inthread ? POLLTASK_RUN_KILL : POLLTASK_Q_KILL;
    prodme = !pq->no_prod;
    pthread_mutex_unlock(&pq->lock);

    switch (state) {
        case POLLTASK_UNQUEUED:
            *ppt = NULL;
            polltask_kill(pt);
            break;

        case POLLTASK_QUEUED:
        case POLLTASK_RUNNING:
        {
            int rv = 0;

            if (inthread) {
                // We are in worker thread - kill in main loop to avoid confusion or deadlock
                *ppt = NULL;
                break;
            }

            if (prodme)
                pollqueue_prod(pq);

            pthread_mutex_lock(&pq->lock);
            while (rv == 0 && pt->state != POLLTASK_Q_DEAD)
                rv = pthread_cond_wait(&pq->cond, &pq->lock);
            pthread_mutex_unlock(&pq->lock);

            // Leave zapping the ref until we have DQed the PT as might well be
            // legitimately used in it
            *ppt = NULL;
            polltask_kill(pt);
            break;
        }
        default:
            request_log("%s: Unexpected task state: %d\n", __func__, state);
            *ppt = NULL;
            break;
    }
}

static uint64_t pollqueue_now(int timeout)
{
    struct timespec now;
    uint64_t now_ms;

    if (clock_gettime(CLOCK_MONOTONIC, &now))
        return 0;
    now_ms = (now.tv_nsec / 1000000) + (uint64_t)now.tv_sec * 1000 + timeout;
    return now_ms ? now_ms : (uint64_t)1;
}

void pollqueue_add_task(struct polltask *const pt, const int timeout)
{
    bool prodme = false;
    struct pollqueue * const pq = pt->q;
    const uint64_t timeout_time = timeout < 0 ? 0 : pollqueue_now(timeout);

    pthread_mutex_lock(&pq->lock);
    if (pt->state == POLLTASK_UNQUEUED || pt->state == POLLTASK_RUNNING) {
        if (pq->tail)
            pq->tail->next = pt;
        else
            pq->head = pt;
        pt->prev = pq->tail;
        pt->next = NULL;
        pt->state = POLLTASK_QUEUED;
        pt->timeout = timeout_time;
        pq->tail = pt;
        prodme = !pq->no_prod;
    }
    pthread_mutex_unlock(&pq->lock);
    if (prodme)
        pollqueue_prod(pq);
}

static void *poll_thread(void *v)
{
    struct pollqueue *const pq = v;

    pthread_mutex_lock(&pq->lock);
    do {
        struct pollfd a[POLLQUEUE_MAX_QUEUE];
        unsigned int i, j;
        unsigned int nall = 0;
        unsigned int npoll = 0;
        struct polltask *pt;
        struct polltask *pt_next;
        struct prepost_ss prepost;
        uint64_t timeout0 = 0;
        int rv;

        for (pt = pq->head; pt; pt = pt_next) {
            pt_next = pt->next;

            if (pt->state == POLLTASK_Q_KILL) {
                pollqueue_rem_task(pq, pt);
                polltask_dead(pt);
                continue;
            }
            if (pt->state == POLLTASK_RUN_KILL) {
                pollqueue_rem_task(pq, pt);
                polltask_kill(pt);
                continue;
            }

            if (pt->fd != -1) {
                assert(npoll < POLLQUEUE_MAX_QUEUE - 1); // Allow for pre/post
                a[npoll++] = (struct pollfd){
                    .fd = pt->fd,
                    .events = pt->events
                };
            }

            // Get earliest timeout
            if (pt->timeout != 0 &&
                (timeout0 == 0 || (int64_t)(pt->timeout - timeout0) < 0))
                timeout0 = pt->timeout;

            ++nall;
        }
        prepost = pq->prepost;
        pthread_mutex_unlock(&pq->lock);

        a[npoll] = (struct pollfd){.fd=-1, .events=0, .revents=0};
        if (prepost.pre)
            prepost.pre(prepost.v, a + npoll);

        do {
            const int64_t diff = (int64_t)(timeout0 - pollqueue_now(0));
            const int timeout = timeout0 == 0 ? -1 :
                                diff <= 0 ? 0 :
                                diff >= INT_MAX ? INT_MAX : (int)diff;

            rv = poll(a, npoll + (a[npoll].fd != -1), timeout);
        } while (rv == -1 && errno == EINTR);

        // Only do timeouts if nothing polled
        if (rv > 0)
            timeout0 = 0;

        if (prepost.post)
            prepost.post(prepost.v, a[npoll].revents);

        if (rv == -1) {
            request_log("Poll error: %s\n", strerror(errno));
            goto fail_unlocked;
        }

        pthread_mutex_lock(&pq->lock);
        /* Prodding in this loop is pointless and might lead to
         * infinite looping
        */
        pq->no_prod = true;

        // Sync for prepost changes
        ++pq->seq;
        if (pq->sig_seq) {
            pq->sig_seq = false;
            pthread_cond_broadcast(&pq->cond);
        }

        for (i = 0, j = 0, pt = pq->head; i < nall; ++i, pt = pt_next) {
            const short r = pt->fd == -1 ? 0 : a[j++].revents;
            pt_next = pt->next;

            if (pt->state != POLLTASK_QUEUED)
                continue;

            /* Pending?
             * Take time as intended time rather than actual time.
             * probably makes no actual difference and saves us a call
             */
            if (r || (pt->timeout != 0 && timeout0 != 0 &&
                      (int64_t)(timeout0 - pt->timeout) >= 0)) {
                pollqueue_rem_task(pq, pt);
                pt->state = POLLTASK_RUNNING;
                pthread_mutex_unlock(&pq->lock);

                /* This can add new entries to the Q but as
                 * those are added to the tail our existing
                 * chain remains intact
                */
                pt->fn(pt->v, r);

                pthread_mutex_lock(&pq->lock);
                if (pt->state == POLLTASK_Q_KILL)
                    polltask_dead(pt);
                else if (pt->state == POLLTASK_RUN_KILL ||
                    (pt->flags & POLLTASK_FLAG_ONCE) != 0)
                    polltask_kill(pt);
                else if (pt->state == POLLTASK_RUNNING)
                    pt->state = POLLTASK_UNQUEUED;
            }
        }
        pq->no_prod = false;

    } while (!pq->kill);

    {
        struct polltask * pt;
        for (pt = pq->head; pt != NULL; pt = pt->next)
            pt->state = POLLTASK_EXIT;
    }
    pthread_mutex_unlock(&pq->lock);
fail_unlocked:

    {
        struct polltask *pt = pq->head;

        pthread_cond_destroy(&pq->cond);
        pthread_mutex_destroy(&pq->lock);
        close(pq->prod_fd);
        if (!pq->join_req != 0)
            pthread_detach(pthread_self());
        free(pq);

        // **** Think harder about freeing non-single use PTs
        // prod may be special?
        while (pt != NULL) {
            struct polltask * const next = pt->next;
            pt->fn(pt->v, -1);
            polltask_free(pt);
            pt = next;
        }
    }

    return NULL;
}

static void prod_fn(void *v, short revents)
{
    struct pollqueue *const pq = v;
    char buf[8];
    if (revents == -1)
        return;
    if (revents != 0) {
        int rv;
        while ((rv = read(pq->prod_fd, buf, 8)) != 8) {
            if (!(rv == -1 && errno == EINTR))
                break;
        }
    }
    pollqueue_add_task(pq->prod_pt, -1);
}

struct pollqueue * pollqueue_new(void)
{
    struct pollqueue *pq = malloc(sizeof(*pq));
    if (!pq)
        return NULL;
    *pq = (struct pollqueue){
        .lock = PTHREAD_MUTEX_INITIALIZER,
        .cond = PTHREAD_COND_INITIALIZER,
        .head = NULL,
        .tail = NULL,
        .kill = false,
        .prod_fd = -1
    };

    atomic_init(&pq->ref_count, 0);
    pq->prod_fd = eventfd(0, EFD_NONBLOCK);
    if (pq->prod_fd == -1)
        goto fail1;
    pq->prod_pt = polltask_new2(pq, pq->prod_fd, POLLIN, prod_fn, pq, POLLTASK_FLAG_NO_REF);
    if (!pq->prod_pt)
        goto fail2;
    pollqueue_add_task(pq->prod_pt, -1);
    if (pthread_create(&pq->worker, NULL, poll_thread, pq))
        goto fail3;
    return pq;

fail3:
    polltask_free(pq->prod_pt);
fail2:
    close(pq->prod_fd);
fail1:
    free(pq);
    return NULL;
}

static void pollqueue_free(struct pollqueue *const pq)
{
    if (am_in_thread(pq)) {
        pq->kill = true;
        if (!pq->no_prod)
            pollqueue_prod(pq);
    }
    else
    {
        pthread_mutex_lock(&pq->lock);
        pq->kill = true;
        // Must prod inside lock here as otherwise there is a potential race
        // where the worker terminates and pq is freed before the prod
        if (!pq->no_prod)
            pollqueue_prod(pq);
        pthread_mutex_unlock(&pq->lock);
    }
}

struct pollqueue * pollqueue_ref(struct pollqueue *const pq)
{
    atomic_fetch_add(&pq->ref_count, 1);
    return pq;
}

void pollqueue_unref(struct pollqueue **const ppq)
{
    struct pollqueue * const pq = *ppq;

    if (!pq)
        return;
    *ppq = NULL;

    if (atomic_fetch_sub(&pq->ref_count, 1) != 0)
        return;

    pollqueue_free(pq);
}

//----------------------------------------------------------------------------
//
// Finish code.
// Most of the complexity is in ensuring that timeouts are raceless
// Try to keep everything that might error in the calling thread so
// the error can be signalled easily

struct finish_timeout_ss {
    bool timed_out;
    int timeout_ms;
    struct pollqueue * pq;
    struct polltask * pt;
    sem_t sem;
};

// Inc ref_count iff it hasn't gone -ve i.e. _free has not been called
// If we inc from -ve then there is always a race that might end up with
// freeing twice
static bool
unfinish(struct pollqueue * const pq)
{
    int n = atomic_load(&pq->ref_count);
    while (n >= 0 && !atomic_compare_exchange_weak(&pq->ref_count, &n, n + 1))
        /* loop */;
    return n >= 0;
}

static void
finish_timeout_cb2(void *v, short revents)
{
    struct finish_timeout_ss * const ft = v;
    struct pollqueue * const pq = ft->pq;

    if (revents != -1 && unfinish(pq))
    {
        --pq->join_req;
        ft->timed_out = true;
    }

    sem_post(&ft->sem);
}

static void
finish_timeout_cb1(void *v, short revents)
{
    struct finish_timeout_ss * const ft = v;
    struct pollqueue * pq = ft->pq;
    (void)revents;

    ++pq->join_req;
    pollqueue_add_task(ft->pt, ft->timeout_ms);
    pollqueue_unref(&pq);
}

int
pollqueue_finish_timeout(struct pollqueue **const ppq, int timeout_ms)
{
    struct pollqueue * pq = *ppq;
    pthread_t worker;
    struct finish_timeout_ss ft;
    int rv;

    if (!pq)
        return 0;

    ft.timed_out = false;
    ft.timeout_ms = timeout_ms;
    ft.pq = pq;
    ft.pt = polltask_new2(pq, -1, 0, finish_timeout_cb2, &ft,
                          POLLTASK_FLAG_ONCE | POLLTASK_FLAG_NO_REF);
    if (ft.pt == NULL)
        return -ENOMEM;

    sem_init(&ft.sem, 0, 0);

    worker = pq->worker;

    // Kick execution into poll thread as otherwise there are races
    // between the execution of _cb2 and the unref
    if ((rv = pollqueue_callback_once(pq, finish_timeout_cb1, &ft)) != 0)
    {
        polltask_delete(&ft.pt);
        sem_destroy(&ft.sem);
        return rv;
    }

    while (sem_wait(&ft.sem) == -1 && errno == EINTR)
        /* loop */;
    sem_destroy(&ft.sem);

    if (ft.timed_out)
        return 1;

    pthread_join(worker, NULL);

    // Delay zapping the ref until after the join as it is legit for the
    // remaining active polltasks to use it.
    *ppq = NULL;
    return 0;
}

void
pollqueue_finish(struct pollqueue **const ppq)
{
    // Whilst it is possible to write a simpler non-timeout version
    // of the finish code it is a better idea to keep the code common
    // given that performance is not important.
    pollqueue_finish_timeout(ppq, -1);
}

//----------------------------------------------------------------------------

void pollqueue_set_pre_post(struct pollqueue *const pq,
                            void (*fn_pre)(void *v, struct pollfd *pfd),
                            void (*fn_post)(void *v, short revents),
                            void *v)
{
    const bool in_thread = am_in_thread(pq);

    pthread_mutex_lock(&pq->lock);
    pq->prepost.pre = fn_pre;
    pq->prepost.post = fn_post;
    pq->prepost.v = v;

    if (!pq->no_prod && !in_thread) {
        const uint32_t seq = pq->seq;
        int rv = 0;

        pollqueue_prod(pq);

        pq->sig_seq = true;
        while (rv == 0 && pq->seq == seq)
            rv = pthread_cond_wait(&pq->cond, &pq->lock);
    }
    pthread_mutex_unlock(&pq->lock);
}

//----------------------------------------------------------------------------
//
// On exit fn
// Would have been simpler if it took a standard callback

struct exit_env_ss {
    void (* fn)(void * v);
    void * v;
};

static void exit_cb(void * v, short revents)
{
    struct exit_env_ss * ee = v;
    assert(revents == -1);

    ee->fn(ee->v);
    free(ee);
}

void pollqueue_set_exit(struct pollqueue *const pq,
                        void (* const exit_fn)(void * v), void * v)
{
    struct exit_env_ss * ee = malloc(sizeof(*ee));
    struct polltask * pt;

    if (ee == NULL)
        return;

    ee->fn = exit_fn;
    ee->v = v;

    pt = polltask_new2(pq, -1, 0, exit_cb, ee, POLLTASK_FLAG_ONCE | POLLTASK_FLAG_NO_REF);
    if (pt == NULL)
        goto fail;

    pollqueue_add_task(pt, -1);
    return;

fail:
    free(ee);
}
