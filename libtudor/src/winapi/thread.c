#include <pthread.h>
#include <errno.h>
#include "internal.h"

typedef DWORD __winfnc THREAD_START_ROUTINE(void *param);

#define CREATE_SUSPENDED 0x00000004

struct win_thread {
    struct win_sync_object sync_obj;
    HANDLE handle;

    pthread_mutex_t lock;
    pthread_t thread;
    DWORD thread_id;
    bool has_detached;

    bool exited;
    pthread_cond_t exit_cond;

    pthread_cond_t suspend_cond;
    int suspend_cntr;

    pthread_cond_t start_cond;
    struct winmodule *start_module;
    THREAD_START_ROUTINE *start_proc;
    void *start_param;
};

static void thread_destr(struct win_thread *thread) {
    //Free memory
    cant_fail_ret(pthread_mutex_lock(&thread->lock));
    if(!thread->has_detached) { cant_fail_ret(pthread_detach(thread->thread)); thread->has_detached = TRUE; }
    cant_fail_ret(pthread_mutex_unlock(&thread->lock));
    cant_fail_ret(pthread_mutex_destroy(&thread->lock));
    cant_fail_ret(pthread_cond_destroy(&thread->suspend_cond));
    cant_fail_ret(pthread_cond_destroy(&thread->exit_cond));
    free(thread);
}

//Wait for the thread to finish via a completion condition rather than joining,
//so finished threads can be detached/reaped immediately (see thread_entry) even
//when the driver leaks their handles - otherwise zombie threads accumulate per
//capture and exhaust the thread limit (pthread_create EAGAIN) during enroll.
static DWORD thread_wait(struct win_thread *thread, DWORD timeout) {
    DWORD ret = 0;
    cant_fail_ret(pthread_mutex_lock(&thread->lock));
    if(!thread->exited) {
        if(timeout == INFINITE) {
            while(!thread->exited) cant_fail_ret(pthread_cond_wait(&thread->exit_cond, &thread->lock));
        } else {
            struct timespec ts;
            cant_fail(clock_gettime(CLOCK_REALTIME, &ts));
            ts.tv_sec += timeout / 1000L;
            ts.tv_nsec += (timeout % 1000L) * 1000000L;
            if(ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            while(!thread->exited) {
                int err = pthread_cond_timedwait(&thread->exit_cond, &thread->lock, &ts);
                if(err == ETIMEDOUT) { ret = WAIT_TIMEOUT; break; }
                cant_fail(err);
            }
        }
    }
    cant_fail_ret(pthread_mutex_unlock(&thread->lock));
    return ret;
}

static void thread_wait_resume(struct win_thread *thread) {
    cant_fail_ret(pthread_mutex_lock(&thread->lock));
    while(thread->suspend_cntr > 0) cant_fail_ret(pthread_cond_wait(&thread->suspend_cond, &thread->lock));
    cant_fail_ret(pthread_mutex_unlock(&thread->lock));
}

static void *thread_entry(void *arg) {
    struct win_thread *thread = (struct win_thread*) arg;

    cant_fail_ret(pthread_mutex_lock(&thread->lock));

    //Initialize state
    winmodule_set_cur(thread->start_module);
    thread->thread_id = win_get_thread_id();
    win_init_tib();

    //Signal start thread that we've started
    cant_fail_ret(pthread_cond_signal(&thread->start_cond));
    cant_fail_ret(pthread_mutex_unlock(&thread->lock));

    //Call the thread start routine
    thread_wait_resume(thread);
    thread->start_proc(thread->start_param);

    //Mark finished and self-detach so the pthread is reaped immediately even if
    //the driver never closes our handle (prevents per-capture thread buildup).
    cant_fail_ret(pthread_mutex_lock(&thread->lock));
    thread->exited = TRUE;
    cant_fail_ret(pthread_cond_broadcast(&thread->exit_cond));
    if(!thread->has_detached) { cant_fail_ret(pthread_detach(thread->thread)); thread->has_detached = TRUE; }
    cant_fail_ret(pthread_mutex_unlock(&thread->lock));

    return (void*) 0;
}

__winfnc HANDLE CreateThread(void *security_attrs, SIZE_T stack_size, THREAD_START_ROUTINE *start_proc, void *param, DWORD flags, DWORD *id) {
    //Allocate thread
    struct win_thread *thread = (struct win_thread*) malloc(sizeof(struct win_thread));
    if(!thread) { winerr_set_errno(); return NULL; }
    thread->sync_obj.wait_fnc = (win_sync_obj_wait_fnc*) thread_wait;

    cant_fail_ret(pthread_mutex_init(&thread->lock, NULL));
    thread->exited = FALSE;
    cant_fail_ret(pthread_cond_init(&thread->exit_cond, NULL));
    thread->suspend_cntr = ((flags & CREATE_SUSPENDED) != 0) ? 1 : 0;
    cant_fail_ret(pthread_cond_init(&thread->suspend_cond, NULL));

    cant_fail_ret(pthread_cond_init(&thread->start_cond, NULL));
    thread->start_module = winmodule_get_cur();
    thread->start_proc = start_proc;
    thread->start_param = param;

    thread->handle = winhandle_create(thread, (winhandle_destr_fnc*) thread_destr);

    //Create the actual thread
    thread->has_detached = FALSE;
    cant_fail_ret(pthread_mutex_lock(&thread->lock));
    cant_fail_ret(pthread_create(&thread->thread, NULL, thread_entry, thread));
    cant_fail_ret(pthread_cond_wait(&thread->start_cond, &thread->lock));
    cant_fail_ret(pthread_cond_destroy(&thread->start_cond));
    cant_fail_ret(pthread_mutex_unlock(&thread->lock));

    if(id) *id = thread->thread_id;

    return thread->handle;
}
WINAPI(CreateThread)

__winfnc DWORD GetThreadId(HANDLE handle) {
    struct win_thread *thread = (struct win_thread*) handle->data;
    return thread->thread_id;
}
WINAPI(GetThreadId)

__winfnc DWORD ResumeThread(HANDLE handle) {
    struct win_thread *thread = (struct win_thread*) handle->data;

    //Decrement the suspend counter
    cant_fail_ret(pthread_mutex_lock(&thread->lock));
    DWORD suspend_ctr = thread->suspend_cntr;
    if(thread->suspend_cntr > 0) thread->suspend_cntr--;
    cant_fail_ret(pthread_cond_signal(&thread->suspend_cond));
    cant_fail_ret(pthread_mutex_unlock(&thread->lock));

    return suspend_ctr;
}
WINAPI(ResumeThread)

__winfnc void ExitThread(DWORD exit_code) {
    pthread_exit((void*) (uintptr_t) exit_code);
}
WINAPI(ExitThread)