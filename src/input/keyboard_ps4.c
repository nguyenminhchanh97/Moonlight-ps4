#include "keyboard_ps4.h"
#include "../log.h"

#include <Limelight.h>
#include <orbis/UserService.h>
#include <orbis/libkernel.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define RAW_SIZE 256
#define KEY_COUNT_OFFSET 0x14
#define MODIFIERS_OFFSET 0x1C
#define KEYS_OFFSET 0x20
#define KEY_COUNT 32
#define DIAGNOSTIC_SIZE 64
#define PHYSICAL_KEY_FLAG 0x8000

/*
 * OpenOrbis models this as a 96-byte state containing nkeys at 0x14, 32-bit
 * HID modifiers at 0x1c, and up to 32 16-bit HID usages at 0x20. Its own
 * header still questions the timestamp width, so the oversized aligned raw
 * buffer and diagnostics remain until captures from real hardware confirm it.
 */
typedef struct KeyboardState {
    uint8_t modifiers;
    uint16_t keys[KEY_COUNT];
} KeyboardState;

_Static_assert(KEYS_OFFSET + KEY_COUNT * sizeof(uint16_t) <= RAW_SIZE,
               "decoder exceeds raw buffer");

enum {
    VK_BACK = 0x08, VK_TAB = 0x09, VK_RETURN = 0x0D, VK_PAUSE = 0x13,
    VK_CAPITAL = 0x14, VK_ESCAPE = 0x1B, VK_SPACE = 0x20, VK_PRIOR = 0x21,
    VK_NEXT = 0x22, VK_END = 0x23, VK_HOME = 0x24, VK_LEFT = 0x25,
    VK_UP = 0x26, VK_RIGHT = 0x27, VK_DOWN = 0x28, VK_SNAPSHOT = 0x2C,
    VK_INSERT = 0x2D, VK_DELETE = 0x2E, VK_LWIN = 0x5B, VK_RWIN = 0x5C,
    VK_APPS = 0x5D, VK_NUMPAD0 = 0x60, VK_MULTIPLY = 0x6A, VK_ADD = 0x6B,
    VK_SUBTRACT = 0x6D, VK_DECIMAL = 0x6E, VK_DIVIDE = 0x6F, VK_F1 = 0x70,
    VK_NUMLOCK = 0x90, VK_SCROLL = 0x91, VK_LSHIFT = 0xA0,
    VK_RSHIFT = 0xA1, VK_LCONTROL = 0xA2, VK_RCONTROL = 0xA3,
    VK_LMENU = 0xA4, VK_RMENU = 0xA5, VK_OEM_1 = 0xBA,
    VK_OEM_PLUS = 0xBB, VK_OEM_COMMA = 0xBC, VK_OEM_MINUS = 0xBD,
    VK_OEM_PERIOD = 0xBE, VK_OEM_2 = 0xBF, VK_OEM_3 = 0xC0,
    VK_OEM_4 = 0xDB, VK_OEM_5 = 0xDC, VK_OEM_6 = 0xDD, VK_OEM_7 = 0xDE
};

static const uint16_t hid_to_vk[256] = {
    [0x04] = 'A', [0x05] = 'B', [0x06] = 'C', [0x07] = 'D',
    [0x08] = 'E', [0x09] = 'F', [0x0A] = 'G', [0x0B] = 'H',
    [0x0C] = 'I', [0x0D] = 'J', [0x0E] = 'K', [0x0F] = 'L',
    [0x10] = 'M', [0x11] = 'N', [0x12] = 'O', [0x13] = 'P',
    [0x14] = 'Q', [0x15] = 'R', [0x16] = 'S', [0x17] = 'T',
    [0x18] = 'U', [0x19] = 'V', [0x1A] = 'W', [0x1B] = 'X',
    [0x1C] = 'Y', [0x1D] = 'Z',
    [0x1E] = '1', [0x1F] = '2', [0x20] = '3', [0x21] = '4',
    [0x22] = '5', [0x23] = '6', [0x24] = '7', [0x25] = '8',
    [0x26] = '9', [0x27] = '0',
    [0x28] = VK_RETURN, [0x29] = VK_ESCAPE, [0x2A] = VK_BACK,
    [0x2B] = VK_TAB, [0x2C] = VK_SPACE, [0x2D] = VK_OEM_MINUS,
    [0x2E] = VK_OEM_PLUS, [0x2F] = VK_OEM_4, [0x30] = VK_OEM_6,
    [0x31] = VK_OEM_5, [0x33] = VK_OEM_1, [0x34] = VK_OEM_7,
    [0x35] = VK_OEM_3, [0x36] = VK_OEM_COMMA, [0x37] = VK_OEM_PERIOD,
    [0x38] = VK_OEM_2, [0x39] = VK_CAPITAL,
    [0x3A] = VK_F1, [0x3B] = VK_F1 + 1, [0x3C] = VK_F1 + 2,
    [0x3D] = VK_F1 + 3, [0x3E] = VK_F1 + 4, [0x3F] = VK_F1 + 5,
    [0x40] = VK_F1 + 6, [0x41] = VK_F1 + 7, [0x42] = VK_F1 + 8,
    [0x43] = VK_F1 + 9, [0x44] = VK_F1 + 10, [0x45] = VK_F1 + 11,
    [0x46] = VK_SNAPSHOT, [0x47] = VK_SCROLL,
    [0x48] = VK_PAUSE, [0x49] = VK_INSERT,
    [0x4A] = VK_HOME, [0x4B] = VK_PRIOR, [0x4C] = VK_DELETE,
    [0x4D] = VK_END, [0x4E] = VK_NEXT, [0x4F] = VK_RIGHT,
    [0x50] = VK_LEFT, [0x51] = VK_DOWN, [0x52] = VK_UP,
    [0x53] = VK_NUMLOCK, [0x54] = VK_DIVIDE, [0x55] = VK_MULTIPLY,
    [0x56] = VK_SUBTRACT, [0x57] = VK_ADD,
    [0x58] = VK_RETURN, [0x59] = VK_NUMPAD0 + 1,
    [0x5A] = VK_NUMPAD0 + 2, [0x5B] = VK_NUMPAD0 + 3,
    [0x5C] = VK_NUMPAD0 + 4, [0x5D] = VK_NUMPAD0 + 5,
    [0x5E] = VK_NUMPAD0 + 6, [0x5F] = VK_NUMPAD0 + 7,
    [0x60] = VK_NUMPAD0 + 8, [0x61] = VK_NUMPAD0 + 9,
    [0x62] = VK_NUMPAD0, [0x63] = VK_DECIMAL,
    [0x65] = VK_APPS
};

typedef int32_t (*KeyboardInitFn)(void);
typedef int32_t (*KeyboardOpenFn)(int32_t, int32_t, int32_t, void *);
typedef int32_t (*KeyboardReadStateFn)(int32_t, void *);
typedef int32_t (*KeyboardCloseFn)(int32_t);

static int s_module = -1;
static int32_t s_handle = -1;
static KeyboardReadStateFn s_read_state;
static KeyboardCloseFn s_close;
static KeyboardState s_previous;
static uint8_t s_previous_raw[DIAGNOSTIC_SIZE];
static unsigned s_raw_changes;
static bool s_have_raw;
static int s_last_read_class = -1;

static bool decode(const uint8_t raw[RAW_SIZE], KeyboardState *state) {
    int32_t count;
    uint32_t modifiers;
    memcpy(&count, raw + KEY_COUNT_OFFSET, sizeof(count));
    memcpy(&modifiers, raw + MODIFIERS_OFFSET, sizeof(modifiers));
    if (count < 0 || count > KEY_COUNT) {
        LOGW("keyboard: invalid decoded key count %d; ABI mismatch", count);
        return false;
    }
    memset(state, 0, sizeof(*state));
    state->modifiers = (uint8_t)modifiers;
    memcpy(state->keys, raw + KEYS_OFFSET, (size_t)count * sizeof(state->keys[0]));
    return true;
}

static uint8_t modifiers_for(uint8_t hid) {
    uint8_t result = 0;
    if (hid & 0x11) result |= MODIFIER_CTRL;
    if (hid & 0x22) result |= MODIFIER_SHIFT;
    if (hid & 0x44) result |= MODIFIER_ALT;
    if (hid & 0x88) result |= MODIFIER_META;
    return result;
}

static bool has_key(const uint16_t keys[KEY_COUNT], uint16_t key) {
    for (size_t i = 0; i < KEY_COUNT; i++)
        if (keys[i] == key) return true;
    return false;
}

static void send_modifier(uint8_t changed, uint8_t current, uint8_t mask,
                          uint16_t vk, uint8_t modifiers) {
    if (changed & mask)
        LiSendKeyboardEvent((short)(vk | PHYSICAL_KEY_FLAG),
                            (current & mask) ? KEY_ACTION_DOWN : KEY_ACTION_UP,
                            (char)modifiers);
}

static void send_modifier_changes(uint8_t previous, uint8_t current) {
    uint8_t changed = previous ^ current;
    uint8_t modifiers = modifiers_for(current);
    send_modifier(changed, current, 0x01, VK_LCONTROL, modifiers);
    send_modifier(changed, current, 0x02, VK_LSHIFT, modifiers);
    send_modifier(changed, current, 0x04, VK_LMENU, modifiers);
    send_modifier(changed, current, 0x08, VK_LWIN, modifiers);
    send_modifier(changed, current, 0x10, VK_RCONTROL, modifiers);
    send_modifier(changed, current, 0x20, VK_RSHIFT, modifiers);
    send_modifier(changed, current, 0x40, VK_RMENU, modifiers);
    send_modifier(changed, current, 0x80, VK_RWIN, modifiers);
}

static void log_raw(const uint8_t raw[RAW_SIZE]) {
    if (s_have_raw && memcmp(s_previous_raw, raw, sizeof(s_previous_raw)) == 0) return;
    memcpy(s_previous_raw, raw, sizeof(s_previous_raw));
    s_have_raw = true;
    s_raw_changes++;
    if (s_raw_changes > 32 && (s_raw_changes % 125) != 0) return;

    char text[DIAGNOSTIC_SIZE * 3 + 1];
    size_t used = 0;
    for (size_t i = 0; i < DIAGNOSTIC_SIZE; i++)
        used += (size_t)snprintf(text + used, sizeof(text) - used, "%02x ", raw[i]);
    LOGI("keyboard: raw[%u] %s", s_raw_changes, text);
}

bool keyboard_ps4_init(void) {
    KeyboardInitFn init = NULL;
    KeyboardOpenFn open = NULL;

    s_module = sceKernelLoadStartModule("/system/common/lib/libSceKeyboard.sprx",
                                        0, NULL, 0, NULL, NULL);
    if (s_module < 0)
        s_module = sceKernelLoadStartModule("libSceKeyboard.sprx",
                                            0, NULL, 0, NULL, NULL);
    if (s_module < 0) {
        LOGW("keyboard: module load failed: 0x%08x", s_module);
        return false;
    }

    int init_sym = sceKernelDlsym(s_module, "sceKeyboardInit", (void **)&init);
    int open_sym = sceKernelDlsym(s_module, "sceKeyboardOpen", (void **)&open);
    int read_sym = sceKernelDlsym(s_module, "sceKeyboardReadState", (void **)&s_read_state);
    int close_sym = sceKernelDlsym(s_module, "sceKeyboardClose", (void **)&s_close);
    if (init_sym < 0 || open_sym < 0 || read_sym < 0 ||
        !init || !open || !s_read_state) {
        LOGE("keyboard: symbols init=%08x open=%08x readState=%08x close=%08x",
             init_sym, open_sym, read_sym, close_sym);
        return false;
    }

    int32_t result = init();
    if (result < 0) {
        LOGE("keyboard: sceKeyboardInit failed: 0x%08x", result);
        return false;
    }
    int32_t user_id = 0;
    result = sceUserServiceGetInitialUser(&user_id);
    if (result < 0) {
        LOGE("keyboard: get initial user failed: 0x%08x", result);
        return false;
    }
    s_handle = open(user_id, 0, 0, NULL);
    if (s_handle < 0) {
        LOGE("keyboard: sceKeyboardOpen failed: 0x%08x", s_handle);
        return false;
    }

    memset(&s_previous, 0, sizeof(s_previous));
    memset(s_previous_raw, 0, sizeof(s_previous_raw));
    s_raw_changes = 0;
    s_have_raw = false;
    s_last_read_class = -1;
    LOGI("keyboard: opened handle=0x%08x; decoder ABI from OpenOrbis", s_handle);
    return true;
}

void keyboard_ps4_poll(void) {
    if (s_handle < 0 || !s_read_state) return;

    _Alignas(16) uint8_t raw[RAW_SIZE] = {0};
    int32_t result = s_read_state(s_handle, raw);
    int read_class = result < 0 ? 0 : 1;
    if (read_class != s_last_read_class) {
        LOGI("keyboard: sceKeyboardReadState result=0x%08x", result);
        s_last_read_class = read_class;
    }
    if (result < 0) {
        keyboard_ps4_release_all();
        return;
    }

    log_raw(raw);
    KeyboardState current;
    if (!decode(raw, &current)) return;

    uint8_t modifiers = modifiers_for(current.modifiers);
    for (size_t i = 0; i < KEY_COUNT; i++) {
        uint16_t usage = s_previous.keys[i];
        uint16_t vk = usage < 256 ? hid_to_vk[usage] : 0;
        if (usage && vk && !has_key(current.keys, usage))
            LiSendKeyboardEvent((short)(vk | PHYSICAL_KEY_FLAG),
                                KEY_ACTION_UP, (char)modifiers);
    }
    send_modifier_changes(s_previous.modifiers, current.modifiers);
    for (size_t i = 0; i < KEY_COUNT; i++) {
        uint16_t usage = current.keys[i];
        uint16_t vk = usage < 256 ? hid_to_vk[usage] : 0;
        if (usage && vk && !has_key(s_previous.keys, usage))
            LiSendKeyboardEvent((short)(vk | PHYSICAL_KEY_FLAG),
                                KEY_ACTION_DOWN, (char)modifiers);
    }
    s_previous = current;
}

void keyboard_ps4_release_all(void) {
    uint8_t modifiers = modifiers_for(s_previous.modifiers);
    for (size_t i = 0; i < KEY_COUNT; i++) {
        uint16_t usage = s_previous.keys[i];
        uint16_t vk = usage < 256 ? hid_to_vk[usage] : 0;
        if (usage && vk)
            LiSendKeyboardEvent((short)(vk | PHYSICAL_KEY_FLAG),
                                KEY_ACTION_UP, (char)modifiers);
    }
    send_modifier_changes(s_previous.modifiers, 0);
    memset(&s_previous, 0, sizeof(s_previous));
}

void keyboard_ps4_shutdown(void) {
    if (s_handle >= 0 && s_close) s_close(s_handle);
    s_handle = -1;
    s_read_state = NULL;
    s_close = NULL;
}
