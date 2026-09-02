#include "mouse_ps4.h"
#include "../log.h"
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include <stddef.h>
#include <orbis/libkernel.h>
#include <orbis/UserService.h>
#include <Limelight.h>

#define WINDOWS_WHEEL_DELTA 120

typedef struct OrbisMouseOpenParam {
    uint8_t flag; // 0 = Normal, 1 = Merged
    uint8_t reserve[7];
} OrbisMouseOpenParam;

typedef struct OrbisMouseData {
    uint64_t timestamp;
    bool connected;
    uint8_t padding[3];
    uint32_t buttons;
    int32_t x_axis;
    int32_t y_axis;
    int32_t wheel;
    int32_t tilt;
    uint8_t reserve[8];
} OrbisMouseData;

_Static_assert(sizeof(OrbisMouseOpenParam) == 8, "unexpected mouse open ABI");
_Static_assert(sizeof(OrbisMouseData) == 40, "unexpected mouse data ABI");
_Static_assert(offsetof(OrbisMouseData, buttons) == 0x0C, "unexpected buttons offset");
_Static_assert(offsetof(OrbisMouseData, x_axis) == 0x10, "unexpected X offset");
_Static_assert(offsetof(OrbisMouseData, wheel) == 0x18, "unexpected wheel offset");

static int s_mouse_mod = -1;
static int s_mouse_handle = -1;

typedef int32_t (*sceMouseInit_t)(void);
typedef int32_t (*sceMouseOpen_t)(int32_t userId, int32_t type, int32_t index, OrbisMouseOpenParam* pParam);
typedef int32_t (*sceMouseRead_t)(int32_t handle, OrbisMouseData* pData, int32_t num);
typedef int32_t (*sceMouseClose_t)(int32_t handle);

static sceMouseInit_t sceMouseInit_func;
static sceMouseOpen_t sceMouseOpen_func;
static sceMouseRead_t sceMouseRead_func;
static sceMouseClose_t sceMouseClose_func;

static OrbisMouseData s_prev_data;

bool mouse_ps4_init(void) {
    s_mouse_mod = sceKernelLoadStartModule("/system/common/lib/libSceMouse.sprx", 0, NULL, 0, NULL, NULL);
    if (s_mouse_mod < 0) s_mouse_mod = sceKernelLoadStartModule("libSceMouse.sprx", 0, NULL, 0, NULL, NULL);

    if (s_mouse_mod < 0) {
        LOGE("Failed to load libSceMouse.sprx");
        return false;
    }

    int init_sym = sceKernelDlsym(s_mouse_mod, "sceMouseInit", (void**)&sceMouseInit_func);
    int open_sym = sceKernelDlsym(s_mouse_mod, "sceMouseOpen", (void**)&sceMouseOpen_func);
    int read_sym = sceKernelDlsym(s_mouse_mod, "sceMouseRead", (void**)&sceMouseRead_func);
    int close_sym = sceKernelDlsym(s_mouse_mod, "sceMouseClose", (void**)&sceMouseClose_func);

    if (init_sym < 0 || open_sym < 0 || read_sym < 0 ||
        !sceMouseInit_func || !sceMouseOpen_func || !sceMouseRead_func) {
        LOGE("mouse: symbols init=%08x open=%08x read=%08x close=%08x",
             init_sym, open_sym, read_sym, close_sym);
        return false;
    }

    int rc = sceMouseInit_func();
    if (rc < 0) {
        LOGE("sceMouseInit() failed: 0x%08x", rc);
        return false;
    }

    int32_t userId = 0;
    rc = sceUserServiceGetInitialUser(&userId);
    if (rc < 0) {
        LOGE("mouse: get initial user failed: 0x%08x", rc);
        return false;
    }

    OrbisMouseOpenParam param;
    memset(&param, 0, sizeof(param));
    param.flag = 0;

    s_mouse_handle = sceMouseOpen_func(userId, 0, 0, &param);
    if (s_mouse_handle < 0) {
        LOGE("sceMouseOpen() failed: 0x%08x", s_mouse_handle);
        return false;
    }

    LOGI("libSceMouse loaded and opened (handle: 0x%08x)", s_mouse_handle);
    memset(&s_prev_data, 0, sizeof(s_prev_data));
    return true;
}

static void send_mouse_button(uint32_t changed, uint32_t current, uint32_t mask, int moonlight_btn) {
    if (changed & mask) {
        char action = (current & mask) ? BUTTON_ACTION_PRESS : BUTTON_ACTION_RELEASE;
        LiSendMouseButtonEvent(action, moonlight_btn);
    }
}

static void send_relative_axis(int32_t x, int32_t y) {
    short clamped_x = (short)(x > SHRT_MAX ? SHRT_MAX : x < SHRT_MIN ? SHRT_MIN : x);
    short clamped_y = (short)(y > SHRT_MAX ? SHRT_MAX : y < SHRT_MIN ? SHRT_MIN : y);
    LiSendMouseMoveEvent(clamped_x, clamped_y);
}

static void send_wheel(int32_t clicks, bool horizontal) {
    int32_t clamped = clicks > 100 ? 100 : clicks < -100 ? -100 : clicks;
    short amount = (short)(clamped * WINDOWS_WHEEL_DELTA);
    if (horizontal)
        LiSendHighResHScrollEvent(amount);
    else
        LiSendHighResScrollEvent(amount);
}

void mouse_ps4_release_all(void) {
    uint32_t buttons = s_prev_data.buttons;
    if (buttons) {
        send_mouse_button(buttons, 0, 0x01, BUTTON_LEFT);
        send_mouse_button(buttons, 0, 0x02, BUTTON_RIGHT);
        send_mouse_button(buttons, 0, 0x04, BUTTON_MIDDLE);
        send_mouse_button(buttons, 0, 0x08, BUTTON_X1);
        send_mouse_button(buttons, 0, 0x10, BUTTON_X2);
    }
    memset(&s_prev_data, 0, sizeof(s_prev_data));
}

void mouse_ps4_poll(void) {
    if (s_mouse_handle < 0 || !sceMouseRead_func) return;

    OrbisMouseData data;
    memset(&data, 0, sizeof(data));
    int num = sceMouseRead_func(s_mouse_handle, &data, 1);

    if (num > 0) {
        if (!data.connected) {
            if (s_prev_data.connected)
                mouse_ps4_release_all();
            return;
        }

        if (data.x_axis != 0 || data.y_axis != 0) {
            send_relative_axis(data.x_axis, data.y_axis);
        }

        if (data.wheel != 0) {
            send_wheel(data.wheel, false);
        }
        if (data.tilt != 0) {
            send_wheel(data.tilt, true);
        }

        uint32_t changed = s_prev_data.buttons ^ data.buttons;
        if (changed) {
            send_mouse_button(changed, data.buttons, 0x01, BUTTON_LEFT);
            send_mouse_button(changed, data.buttons, 0x02, BUTTON_RIGHT);
            send_mouse_button(changed, data.buttons, 0x04, BUTTON_MIDDLE);
            send_mouse_button(changed, data.buttons, 0x08, BUTTON_X1);
            send_mouse_button(changed, data.buttons, 0x10, BUTTON_X2);
        }

        s_prev_data = data;
    }
}

void mouse_ps4_shutdown(void) {
    if (s_mouse_handle >= 0 && sceMouseClose_func) {
        sceMouseClose_func(s_mouse_handle);
        s_mouse_handle = -1;
    }
    sceMouseRead_func = NULL;
    sceMouseClose_func = NULL;
}
