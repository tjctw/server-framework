#include "async.h"

#include <stdlib.h>
#include <stdio.h>
//#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#include <fcntl.h>

#define WK_QUESIZE 8

/* suppress compilation warnings */
static inline ssize_t write_wrapper(int fd, const void *buf, size_t count)
{
    ssize_t s;
    if ((s = write(fd, buf, count)) < count) perror("write");
    return s;
}
#undef write
#define write write_wrapper

/* the actual working thread */
static void *worker_thread_cycle(void *async);

/* signaling to finish */
static void async_signal(async_p async);

/* the destructor */
static void async_destroy(async_p queue);

/** A task node */
struct AsyncTask {
    struct AsyncTask *next;
    void (*task)(void *);
    void *arg;
};

typedef struct tt {
	pthread_t    id;
    pthread_mutex_t wake_mutex;
    pthread_cond_t wake_cv;
	unsigned char on_flag;
	unsigned char in;	/* offset from start of work_queue where to put work next */
	unsigned char out;	/* offset from start of work_queue where to get work next */
	struct AsyncTask work_queue[WK_QUESIZE];
} thread_t;

static void *join_thread(thread_t *thr)
{
    void *ret;
    pthread_join(thr->id, &ret);
    return ret;
}

static int create_thread(thread_t *thr,
                         void *(*thread_func)(void *))
{
    int ret=0;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
    if(pthread_create(&thr->id, NULL, thread_func, (void *)thr) !=0) ret=-1;
    pthread_attr_destroy(&attr);
    return ret;
}



/** The Async struct */
struct Async {
    /** the task queue - MUST be first in the struct */
    pthread_mutex_t lock;              /**< a mutex for data integrity */
    pthread_mutex_t pool_lock;              /**< a mutex for data integrity */
    pthread_mutex_t wake_mutex;              /**< a mutex for data integrity */
    pthread_cond_t wake_cv;

    struct AsyncTask * volatile tasks;  /**< active tasks */
    struct AsyncTask * volatile pool;   /**< a task node pool */
    struct AsyncTask ** volatile pos;   /**< the position for new tasks */

    /** The pipe used for thread wakeup */
    // struct {
    //     int in;  /**< read incoming data (opaque data), used for wakeup */
    //     int out; /**< write opaque data (single byte),
    //                   used for wakeup signaling */
    // } pipe;

    int count; /**< the number of initialized threads */

    unsigned run : 1; /**< the running flag */

    /** the thread pool */
    //pthread_t threads[];
    thread_t threads[];
};

static thread_t* round_robin_schedule(async_p async)
{
    static int cur_thread_index = -1;
    cur_thread_index = (cur_thread_index + 1) % async->count ;
    return &async->threads[cur_thread_index];
}

/* Task Management - add a task and perform all tasks in queue */
//TODO: 50% of gprof
static int async_run(async_p async, void (*task)(void *), void *arg)
{
    struct AsyncTask *c;  /* the container, storing the task */
    //thread_t* thr;
    if (!async || !task) return -1;

    pthread_mutex_lock(&(async->lock));
    /* get a container from the pool of grab a new container */
    if (async->pool) {
        c = async->pool;
        async->pool = async->pool->next;
    } else {
        c = malloc(sizeof(*c));
        if (!c) {
            pthread_mutex_unlock(&async->lock);
            return -1;
        }
    }
    c->next = NULL;
    c->task = task;
    c->arg = arg;

    if (async->tasks) {
        *(async->pos) = c;
    } else {
        async->tasks = c;
    }
    //point pos the the pointer of next(null) AsyncTask
    async->pos = &(c->next);
    pthread_mutex_unlock(&async->lock);
    pthread_cond_signal(&(async->wake_cv));
    return 0;
    //FIFO

    // thr = round_robin_schedule(async);
    // if(thr->out == thr->in) {
    //     for(;thr->in<WK_QUESIZE;thr->in+=1) {
    //         if(async->tasks) {
    //             thr->work_queue[thr->in] = *async->tasks;
    //             async->tasks->next = async->pool;
    //             async->pool = async->tasks;
    //             async->tasks = async->tasks->next;
    //
    //         } else {
    //             //no task
    //             break;
    //         }
    //     }
    //      pthread_cond_signal(&(thr->wake_cv));
    //}
    // // if (async->tasks) {
    // //     *(async->pos) = c;
    // // } else {
    // //     async->tasks = c;
    // // }
    // //point pos the the pointer of next(null) AsyncTask
    // async->pos = &(c->next);
    //pthread_mutex_unlock(&async->lock);
    /* wake up any sleeping threads
     * any activated threads will ask to require the mutex
     * as soon as we write.
     * we need to unlock before we write, or we will have excess
     * context switches.
     */

    //get a thread give work and wakeup

    //pthread_cond_broadcast(&(async->wake_cv));


    //return 0;
}

/** Performs all the existing tasks in the queue. */
static void perform_tasks(async_p async)
{
    struct AsyncTask *c = NULL;
    do {
        pthread_mutex_lock(&(async->lock));
        if (c) {
            c->next = async->pool;
            async->pool = c;
        }
        c = async->tasks;
        if (c) {
            async->tasks = async->tasks->next;
        }
        pthread_mutex_unlock(&(async->lock));
        if(c) c->task(c->arg);
    }while (c);
}

/* The worker cycle */
static void *worker_thread_cycle(void *arg)
{
     thread_t *thr = arg;
     thr->on_flag = 1;
     while (thr->on_flag) {
        pthread_mutex_lock(&(thr->wake_mutex));
        pthread_cond_wait(&(thr->wake_cv), &(thr->wake_mutex));
        pthread_mutex_unlock(&(thr->wake_mutex));

        while(thr->in>thr->out){
            thr->work_queue[thr->out].task(thr->work_queue[thr->out].arg);
            thr->out+=1;
        }
        thr->in=0;
        thr->out=0;
     }
    // unsigned char waken_flag = 1;
    // int count=0;
    // struct AsyncTask *tail = NULL;
    // struct AsyncTask *c_now = NULL;
    // struct AsyncTask *c = NULL;
    //
    //
    //     pthread_mutex_lock(&(async->wake_mutex));
    //     //do not wait again after waked up
    //     while(waken_flag==0) {
    //         pthread_cond_wait(&(async->wake_cv), &(async->wake_mutex));
    //         waken_flag = 1;
    //     }pthread_mutex_unlock(&(async->wake_mutex));
    //
    //     //aquire lock only if task exist
    //     if(async->tasks || c){
    //         pthread_mutex_lock(&(async->lock));
    //         if (c) {
    //             tail->next = async->pool;
    //             async->pool = c;
    //         }
    //         c = async->tasks;
    //         if(c) {
    //             async->tasks = async->tasks->next;
    //             tail = c;
    //             while (tail->next && count<WK_QUESIZE) {
    //                 tail = async->tasks;
    //                 async->tasks = async->tasks->next;
    //                 count ++;
    //             }
    //         }
    //         pthread_mutex_unlock(&(async->lock));
    //     } else {
    //         //if cannot get any job, go to sleep
    //         waken_flag=0;
    //     }
    //
    //     /* perform the tasks */
    //     c_now = c;
    //     while(c_now) {
    //         c_now->task(c_now->arg);
    //         if(c_now==tail)break;
    //         c_now = c_now->next;
    //     }
    //     sched_yield();
    //     //if(!async->tasks) waken_flag=0;
    //     if(!async->run) break;
    //
    // }
    return 0;
}

/* Signal and finish */

static void async_signal(async_p async)
{

    /* send `async->count` number of wakeup signales.
     * data content is irrelevant. */
    //write(async->pipe.out, async, async->count);
    //TODO update to scheduler
    //pthread_cond_broadcast(&(async->wake_cv));
    async->run = 0;
    //async->run = 0;
}
static void* schedule(void *_async)
{
    struct Async *async = _async;
    thread_t* thr;
    while(1){
        if(async->tasks == NULL){
            pthread_mutex_lock(&(async->wake_mutex));
            pthread_cond_wait(&(async->wake_cv), &(async->wake_mutex));
            pthread_mutex_unlock(&(async->wake_mutex));
        }
        thr = round_robin_schedule(async);
        if(thr->out == thr->in) {
            //put work into thread_queue
            pthread_mutex_lock(&(async->lock));
            while(thr->in<WK_QUESIZE) {

                if(async->tasks) {
                thr->work_queue[thr->in] = *async->tasks;
                async->tasks->next = async->pool;
                async->pool = async->tasks;
                async->tasks = thr->work_queue[thr->in].next;
                thr->in+=1;
                } else break;
            }
            pthread_mutex_unlock(&async->lock);
            if(thr->in)
                pthread_cond_signal(&(thr->wake_cv));
        }
        sched_yield();
        //if shutting down, clean works
        if(async->run==0){
            while(async->tasks){
                thr = round_robin_schedule(async);
                //idle thread only? TODO: think about + wk dynamicly
                if(async->tasks && thr->out == thr->in) {
                    //put work into thread_queue
                    for(;thr->in<WK_QUESIZE;thr->in+=1) {
                        if(async->tasks==NULL) break;
                        pthread_mutex_lock(&(async->lock));
                        thr->work_queue[thr->in] = *async->tasks;


                        async->tasks->next = async->pool;
                        async->pool = async->tasks;
                        async->tasks = async->tasks->next;
                        pthread_mutex_unlock(&async->lock);

                    }
                    pthread_cond_signal(&(thr->wake_cv));
                }
            }
            break;
        }
    }
    for (int i = 0; i < async->count; i++) {
        //will block until return
        async->threads[i].on_flag = 0;
        pthread_cond_signal(&(async->threads[i].wake_cv));
    }
    return 0;
}

static void async_wait(async_p async)
{
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
    pthread_t thr_man;
    void *ret;


    if(pthread_create(&thr_man, NULL, schedule, async) !=0) return;
    pthread_attr_destroy(&attr);
    if (!async) return;

    /* join threads */
    for (int i = 0; i < async->count; i++) {

        //will block until return
        join_thread(&async->threads[i]);
    }
    pthread_join(thr_man, &ret);
    /* perform any pending tasks with main thread */
    perform_tasks(async);
    /* release queue memory and resources */
    async_destroy(async);
}

static void async_finish(async_p async)
{
    async_signal(async);
    async_wait(async);
}

/* Object creation and destruction */

/** Destroys the Async object, releasing its memory. */
static void async_destroy(async_p async)
{
    pthread_mutex_lock(&async->lock);
    struct AsyncTask *to_free;
    async->pos = NULL;
    /* free all tasks */
    struct AsyncTask *pos = async->tasks;
    while ((to_free = pos)) {
        pos = pos->next;
        free(to_free);
    }
    async->tasks = NULL;
    /* free task pool */
    pos = async->pool;
    while ((to_free = pos)) {
        pos = pos->next;
        free(to_free);
    }
    async->pool = NULL;
    // /* close pipe */
    // if (async->pipe.in) {
    //     close(async->pipe.in);
    //     async->pipe.in = 0;
    // }
    // if (async->pipe.out) {
    //     close(async->pipe.out);
    //     async->pipe.out = 0;
    // }
    pthread_mutex_unlock(&async->lock);
    pthread_mutex_destroy(&async->lock);
    pthread_mutex_destroy(&async->wake_mutex);
    pthread_cond_destroy(&async->wake_cv);

    free(async);
}

static async_p async_create(int threads)
{
    async_p async = malloc(sizeof(*async) + (threads * sizeof(thread_t)));
    async->tasks = NULL;
    async->pool = NULL;
    async->pos = NULL;
    // async->pipe.in = 0;
    // async->pipe.out = 0;
    if (pthread_mutex_init(&(async->lock), NULL)) {
        free(async);
        return NULL;
    };
    if (pthread_mutex_init(&(async->wake_mutex), NULL)) {
        free(async);
        return NULL;
    };
    if (pthread_cond_init(&(async->wake_cv), NULL)) {
        free(async);
        return NULL;
    };
    // if (pipe((int *) &(async->pipe))) {
    //     free(async);
    //     return NULL;
    // };
    // fcntl(async->pipe.out, F_SETFL, O_NONBLOCK | O_WRONLY);
    async->run = 1;
    /* create threads */
    for (async->count = 0; async->count < threads; async->count++) {
        if (create_thread(async->threads + async->count,
                          worker_thread_cycle)) {
            /* signal */
            async_signal(async);
            /* wait for threads and destroy object */
            async_wait(async);
            /* return error */
            return NULL;
        };
    }
    return async;
}

/* API gateway */
struct __ASYNC_API__ Async = {
    .create = async_create,
    .signal = async_signal,
    .wait = async_wait,
    .finish = async_finish,
    .run = async_run,
};
