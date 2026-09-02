#include "input_spike.h"
#include "../log.h"
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <orbis/libkernel.h>
#include <orbis/UserService.h>

static int s_kb_mod = -1;

typedef int32_t (*sceKeyboardInit_t)(void);
typedef int32_t (*sceKeyboardOpen_t)(int32_t userId, int32_t type, int32_t index, void* pParam);
typedef int32_t (*sceKeyboardRead_t)(int32_t handle, void* pData, int32_t num);
typedef int32_t (*sceKeyboardClose_t)(int32_t handle);

static sceKeyboardInit_t sceKeyboardInit_func;
static sceKeyboardOpen_t sceKeyboardOpen_func;
static sceKeyboardRead_t sceKeyboardRead_func;
static sceKeyboardClose_t sceKeyboardClose_func;

static int s_kb_handle = -1;
static uint8_t s_prev_kb_data[256];
static int s_first_read = 1;

void input_spike_init(void) {
    s_kb_mod = sceKernelLoadStartModule("/system/common/lib/libSceKeyboard.sprx", 0, NULL, 0, NULL, NULL);
    if (s_kb_mod < 0) s_kb_mod = sceKernelLoadStartModule("libSceKeyboard.sprx", 0, NULL, 0, NULL, NULL);
    LOGI("libSceKeyboard.sprx mod = 0x%08x", (unsigned)s_kb_mod);

    if (s_kb_mod >= 0) {
        sceKernelDlsym(s_kb_mod, "sceKeyboardInit", (void**)&sceKeyboardInit_func);
        sceKernelDlsym(s_kb_mod, "sceKeyboardOpen", (void**)&sceKeyboardOpen_func);
        sceKernelDlsym(s_kb_mod, "sceKeyboardRead", (void**)&sceKeyboardRead_func);
        sceKernelDlsym(s_kb_mod, "sceKeyboardClose", (void**)&sceKeyboardClose_func);

        if (sceKeyboardInit_func && sceKeyboardOpen_func && sceKeyboardRead_func) {
            int rc = sceKeyboardInit_func();
            LOGI("sceKeyboardInit() = 0x%08x", rc);
            
            int32_t userId = 0;
            sceUserServiceGetInitialUser(&userId);
            s_kb_handle = sceKeyboardOpen_func(userId, 0, 0, NULL);
            LOGI("sceKeyboardOpen() = 0x%08x", s_kb_handle);
        } else {
            LOGI("Keyboard symbols missing");
        }
    }
}

static void hexdump(const char *prefix, const uint8_t *data, size_t size) {
    char buf[256];
    size_t out = 0;
    for (size_t i = 0; i < size && out < sizeof(buf) - 4; i++) {
        out += snprintf(buf + out, sizeof(buf) - out, "%02x ", data[i]);
    }
    LOGI("%s: %s", prefix, buf);
}

void input_spike_poll(void) {
    if (s_kb_handle >= 0 && sceKeyboardRead_func) {
        uint8_t kb_data[256];
        memset(kb_data, 0, sizeof(kb_data));
        int num = sceKeyboardRead_func(s_kb_handle, kb_data, 1);
        
        if (num > 0) {
            // Only log if the buffer actually changed
            if (s_first_read || memcmp(s_prev_kb_data, kb_data, 64) != 0) {
                hexdump("KB", kb_data, 64);
                memcpy(s_prev_kb_data, kb_data, sizeof(kb_data));
                s_first_read = 0;
            }
        }
    }
}

