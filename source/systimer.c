#include "systimer.h"

// Linux link with -lrt

#if defined(OS_WINDOWS)
#include "thread-pool.h"
#include <Windows.h>
#pragma comment(lib, "Winmm.lib")
#else
#include <signal.h>
#include <time.h>
#endif

#include <assert.h>
#include <memory.h>
#include <stdlib.h>
#include <errno.h>

typedef struct _timer_context_t
{
	systimer_func callback;
	void* cbparam;

#if defined(OS_WINDOWS)
	UINT timerId;
	unsigned int period;
	unsigned int count;
	int locked;
#elif defined(OS_WINDOWS_ASYNC)
	HANDLE timerId;
#elif defined(OS_LINUX)
	timer_t timerId;
	int ontshot;
#endif
} timer_context_t;

#if defined(OS_WINDOWS)
struct 
{
	TIMECAPS tc;
	thread_pool_t pool;
} g_ctx;

#define TIMER_PERIOD 1000
#endif

#if defined(OS_WINDOWS)
static void timer_thread_worker(void *param)
{
	timer_context_t* ctx;
	ctx = (timer_context_t*)param;
	if(ctx->period > g_ctx.tc.wPeriodMax)
	{
		if(ctx->count == ctx->period / TIMER_PERIOD)
			ctx->callback((systimer_t)ctx, ctx->cbparam);
		ctx->count = (ctx->count + 1) % (ctx->period / TIMER_PERIOD + 1);
	}
	else
	{
		ctx->callback((systimer_t)ctx, ctx->cbparam);
	}

	InterlockedDecrement(&ctx->locked);
}

static void CALLBACK timer_schd_worker(UINT uTimerID, UINT uMsg, DWORD_PTR dwUser, DWORD_PTR dw1, DWORD_PTR dw2)
{
	timer_context_t* ctx;
	ctx = (timer_context_t*)dwUser;
	if(1 != InterlockedIncrement(&ctx->locked))
	{
		InterlockedDecrement(&ctx->locked);
	}
	else
	{
		// one timer only can be call in one thread
		thread_pool_push(g_ctx.pool, timer_thread_worker, ctx);
	}
}
#elif defined(OS_LINUX)
static void timer_schd_worker(union sigval v)
{
	timer_context_t* ctx;
	ctx = (timer_context_t*)v.sival_ptr;
	ctx->callback((systimer_t)ctx, ctx->cbparam);
}
#else
static int timer_schd_worker(void *param)
{
}
#endif

int systimer_init(void)
{
#if defined(OS_WINDOWS)
	timeGetDevCaps(&g_ctx.tc, sizeof(TIMECAPS));
	g_ctx.pool = thread_pool_create(2, 4, 1000);
#endif
	return 0;
}

int systimer_clean(void)
{
#if defined(OS_WINDOWS)
	thread_pool_destroy(g_ctx.pool);
#endif
	return 0;
}

#if defined(OS_WINDOWS)
static int systimer_create(systimer_t* id, unsigned int period, int oneshot, systimer_func callback, void* cbparam)
{
	UINT fuEvent;
	timer_context_t* ctx;

	if(oneshot && g_ctx.tc.wPeriodMin > period && period > g_ctx.tc.wPeriodMax)
		return -EINVAL;

	ctx = (timer_context_t*)malloc(sizeof(timer_context_t));
	if(!ctx)
		return -ENOMEM;

	memset(ctx, 0, sizeof(timer_context_t));
	ctx->callback = callback;
	ctx->cbparam = cbparam;
	ctx->period = period;
	ctx->count = 0;

	// check period value
	period = (period > g_ctx.tc.wPeriodMax) ?  TIMER_PERIOD : period;
	fuEvent = (oneshot?TIME_ONESHOT:TIME_PERIODIC)|TIME_CALLBACK_FUNCTION;
	ctx->timerId = timeSetEvent(period, 10, timer_schd_worker, (DWORD_PTR)ctx, fuEvent);
	if(0 == ctx->timerId)
	{
		free(ctx);
		return -EINVAL;
	}

	*id = (systimer_t)ctx;
	return 0;
}
#elif defined(OS_LINUX)
static int systimer_create(systimer_t* id, unsigned int period, int oneshot, systimer_func callback, void* cbparam)
{
	struct sigevent sev;
	struct itimerspec tv;
	timer_context_t* ctx;

	ctx = (timer_context_t*)malloc(sizeof(timer_context_t));
	if(!ctx)
		return -ENOMEM;;

	memset(ctx, 0, sizeof(timer_context_t));
	ctx->callback = callback;
	ctx->cbparam = cbparam;

	memset(&sev, 0, sizeof(sev));
	sev.sigev_notify = SIGEV_THREAD;
	sev.sigev_value.sival_ptr = ctx;
	sev.sigev_notify_function = timer_schd_worker;
	if(0 != timer_create(CLOCK_MONOTONIC, &sev, &ctx->timerId))
	{
		free(ctx);
		return -errno;
	}

	tv.it_interval.tv_sec = period / 1000;
	tv.it_interval.tv_nsec = (period % 1000) * 1000000; // 10(-9)second
	tv.it_value.tv_sec = tv.it_interval.tv_sec;
	tv.it_value.tv_nsec = tv.it_interval.tv_nsec;
	if(0 != timer_settime(ctx->timerId, 0, &tv, NULL))
	{
		timer_delete(ctx->timerId);
		free(ctx);
		return -errno;
	}

	*id = (systimer_t)ctx;
	return 0;
}
#elif defined(OS_WINDOWS_ASYNC)
static int systimer_create(systimer_t* id, unsigned int period, int oneshot, systimer_func callback, void* cbparam)
{
	LARGE_INTEGER tv;
	timer_context_t* ctx;
	ctx = (timer_context_t*)malloc(sizeof(timer_context_t));
	if(!ctx)
		return -ENOMEM;

	memset(ctx, 0, sizeof(timer_context_t));
	ctx->callback = callback;
	ctx->cbparam = cbparam;

	ctx->timerId = CreateWaitableTimer(NULL, FALSE, NULL);
	if(0 == ctx->timerId)
	{
		free(ctx);
		return -(int)GetLastError();
	}

	tv.QuadPart = -10000L * period; // in 100 nanosecond intervals
	if(!SetWaitableTimer(ctx->timerId, &tv, oneshot?0:period, timer_schd_worker, ctx, FALSE))
	{
		CloseHandle(ctx->timerId);
		free(ctx);
		return -(int)GetLastError();
	}

	*id = (systimer_t)ctx;
	return 0;
}
#else
static int systimer_create(systimer_t* id, int period, systimer_func callback, void* cbparam)
{
	ERROR: dont implemention
}
#endif

int systimer_oneshot(systimer_t *id, unsigned int period, systimer_func callback, void* cbparam)
{
	return systimer_create(id, period, 1, callback, cbparam);
}

int systimer_start(systimer_t* id, unsigned int period, systimer_func callback, void* cbparam)
{
	return systimer_create(id, period, 0, callback, cbparam);
}

int systimer_stop(systimer_t id)
{
	timer_context_t* ctx;
	if(!id) return -EINVAL;

	ctx = (timer_context_t*)id;

#if defined(OS_WINDOWS)
	timeKillEvent(ctx->timerId);
#elif defined(OS_LINUX)
	timer_delete(ctx->timerId);
#elif defined(OS_WINDOWS_ASYNC)
	CloseHandle(ctx->timerId);
#else
	ERROR: dont implemention
#endif

	free(ctx); // take care call-back function
	return 0;
}
