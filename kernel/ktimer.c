/*
    RExOS - embedded RTOS
    Copyright (c) 2011-2014, Alexey Kramarenko
    All rights reserved.
*/

#include "ktimer.h"
#include "kernel_config.h"
#include "dbg.h"
#include "kernel.h"
#include <string.h>
#include "../userspace/error.h"

#define FREE_RUN                                        2000000

void hpet_start_stub(unsigned int value, void* param)
{
#if (KERNEL_INFO)
    printk("Warning: HPET start stub called\n\r");
#endif //KERNEL_INFO
    kprocess_error_current(ERROR_STUB_CALLED);
}

void hpet_stop_stub(void* param)
{
#if (KERNEL_INFO)
    printk("Warning: HPET stop stub called\n\r");
#endif //KERNEL_INFO
    kprocess_error_current(ERROR_STUB_CALLED);
}

unsigned int hpet_elapsed_stub(void* param)
{
#if (KERNEL_INFO)
    printk("Warning: HPET elapsed stub called\n\r");
#endif //KERNEL_INFO
    kprocess_error_current(ERROR_STUB_CALLED);
    return 0;
}

void ktimer_get_uptime(TIME* res)
{
    res->sec = __KERNEL->uptime.sec;
    res->usec = __KERNEL->uptime.usec + __KERNEL->cb_ktimer.elapsed(__KERNEL->cb_ktimer_param);
    while (res->usec >= 1000000)
    {
        res->sec++;
        res->usec -= 1000000;
    }
}

static inline void find_shoot_next()
{
    KTIMER* to_shoot;
    TIME uptime;

    do {
        to_shoot = NULL;
        if (__KERNEL->timers)
        {
            //ignore seconds adjustment
            uptime.sec = __KERNEL->uptime.sec;
            uptime.usec = __KERNEL->uptime.usec + __KERNEL->cb_ktimer.elapsed(__KERNEL->cb_ktimer_param);
            if (time_compare(&__KERNEL->timers->time, &uptime) >= 0)
            {
                to_shoot = __KERNEL->timers;
                dlist_remove_head((DLIST**)&__KERNEL->timers);
            }
            //add to this second events
            else if (__KERNEL->timers->time.sec == uptime.sec)
            {
                __KERNEL->uptime.usec += __KERNEL->cb_ktimer.elapsed(__KERNEL->cb_ktimer_param);
                __KERNEL->cb_ktimer.stop(__KERNEL->cb_ktimer_param);
                __KERNEL->hpet_value = __KERNEL->timers->time.usec - __KERNEL->uptime.usec;
                __KERNEL->cb_ktimer.start(__KERNEL->hpet_value, __KERNEL->cb_ktimer_param);
            }
            if (to_shoot)
            {
                __KERNEL->timer_executed = true;
                to_shoot->callback(to_shoot->param);
                __KERNEL->timer_executed = false;
            }
        }
    } while (to_shoot);
}

void ktimer_second_pulse()
{
    disable_interrupts();
    ++__KERNEL->uptime.sec;
    __KERNEL->hpet_value = 0;
    __KERNEL->cb_ktimer.stop(__KERNEL->cb_ktimer_param);
    __KERNEL->cb_ktimer.start(FREE_RUN, __KERNEL->cb_ktimer_param);
    __KERNEL->uptime.usec = 0;

    find_shoot_next();
    enable_interrupts();
}

void ktimer_hpet_timeout()
{
#if (KERNEL_TIMER_DEBUG)
    if (__KERNEL->hpet_value == 0)
        printk("Warning: HPET timeout on FREE RUN mode: second pulse is inactive or HPET configured improperly");
#endif
    disable_interrupts();
    __KERNEL->uptime.usec += __KERNEL->hpet_value;
    __KERNEL->hpet_value = 0;
    __KERNEL->cb_ktimer.start(FREE_RUN, __KERNEL->cb_ktimer_param);

    find_shoot_next();
    enable_interrupts();
}

void ktimer_setup(const CB_SVC_TIMER *cb_ktimer, void *cb_ktimer_param)
{
    if (__KERNEL->cb_ktimer.start == hpet_start_stub)
    {
        __KERNEL->cb_ktimer.start = cb_ktimer->start;
        __KERNEL->cb_ktimer.stop = cb_ktimer->stop;
        __KERNEL->cb_ktimer.elapsed = cb_ktimer->elapsed;
        __KERNEL->cb_ktimer_param = cb_ktimer_param;
        __KERNEL->cb_ktimer.start(FREE_RUN, __KERNEL->cb_ktimer_param);
    }
    else
        kprocess_error_current(ERROR_INVALID_SVC);
}

void ktimer_start(KTIMER* timer)
{
    TIME uptime;
    DLIST_ENUM de;
    KTIMER* cur;
    bool found = false;
    ktimer_get_uptime(&uptime);
    time_add(&uptime, &timer->time, &timer->time);
    dlist_enum_start((DLIST**)&__KERNEL->timers, &de);
    while (dlist_enum(&de, (DLIST**)&cur))
        if (time_compare(&cur->time, &timer->time) < 0)
        {
            dlist_add_before((DLIST**)&__KERNEL->timers, (DLIST*)cur, (DLIST*)timer);
            found = true;
            break;
        }
    if (!found)
        dlist_add_tail((DLIST**)&__KERNEL->timers, (DLIST*)timer);
    if (!__KERNEL->timer_executed)
        find_shoot_next();
}

void ktimer_stop(KTIMER* timer)
{
    dlist_remove((DLIST**)&__KERNEL->timers, (DLIST*)timer);
}

void ktimer_init()
{
    __KERNEL->cb_ktimer.start = hpet_start_stub;
    __KERNEL->cb_ktimer.stop = hpet_stop_stub;
    __KERNEL->cb_ktimer.elapsed = hpet_elapsed_stub;
}
