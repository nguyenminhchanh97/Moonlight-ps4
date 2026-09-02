#include "input_usb.h"
#include "keyboard_ps4.h"
#include "mouse_ps4.h"
#include "../log.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <orbis/libkernel.h>

#define INPUT_POLL_US 4000u

static pthread_t s_thread;
static atomic_bool s_running;
static int s_started;

static void *input_usb_thread(void *unused) {
    (void)unused;
    uint64_t last = sceKernelGetProcessTime();
    uint64_t interval_total = 0, process_max = 0;
    unsigned polls = 0;

    while (atomic_load_explicit(&s_running, memory_order_acquire)) {
        uint64_t begin = sceKernelGetProcessTime();
        if (polls) interval_total += begin - last;
        last = begin;
        keyboard_ps4_poll();
        mouse_ps4_poll();
        uint64_t elapsed = sceKernelGetProcessTime() - begin;
        if (elapsed > process_max) process_max = elapsed;
        polls++;
        if (elapsed < INPUT_POLL_US)
            sceKernelUsleep((uint32_t)(INPUT_POLL_US - elapsed));
    }
    LOGI("input-usb: stopped polls=%u avg_interval=%.2fms max_process=%.2fms",
         polls, polls > 1 ? (double)interval_total / (polls - 1) / 1000.0 : 0.0,
         (double)process_max / 1000.0);
    return NULL;
}

int input_usb_start(void) {
    if (s_started) return 0;
    atomic_store_explicit(&s_running, true, memory_order_release);
    int rc = pthread_create(&s_thread, NULL, input_usb_thread, NULL);
    if (rc != 0) {
        atomic_store_explicit(&s_running, false, memory_order_release);
        LOGE("input-usb: pthread_create failed: %d", rc);
        return -1;
    }
    s_started = 1;
    LOGI("input-usb: polling at 250 Hz (4 ms), independent of video");
    return 0;
}

void input_usb_stop(void) {
    if (!s_started) return;
    atomic_store_explicit(&s_running, false, memory_order_release);
    pthread_join(s_thread, NULL);
    s_started = 0;
}
