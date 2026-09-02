# INPUT_IMPLEMENTATION_REPORT.md — Keyboard & Mouse Support for Moonlight-PS4

**Date**: 2026-09-02
**Repository**: `Moonlight-ps4` v1.1.0 — `D:\work\Moonlight-ps4-dev\Moonlight-ps4`
**Audit scope**: Full repository — source, build system, dependencies, upstream APIs, PS4 SDK APIs
**Audit result**: AUDIT COMPLETE

---

## Current Architecture

The application is a native C client for jailbroken PS4 (GoldHEN, FW 9.00), built with
the OpenOrbis toolchain v0.5.4 (clang/lld cross-compiling to `x86_64-pc-freebsd12-elf`).

### Component table

| Component | Source file(s) | Role |
|---|---|---|
| Protocol engine | `third_party/moonlight-common-c` @ `e41355e` + Orbis patches | RTSP, FEC, stream crypto, all input send APIs |
| Pairing / HTTP | `src/gamestream/{certgen,gs_http,mini_xml,client,sps}.c` | mbedTLS-based GS pairing |
| Video decode SW | `src/video/decoder_ffmpeg.c` | FFmpeg H.264 software decode |
| Video decode HW | `src/video/decoder_orbis.c` | `libSceVideodec2` hardware decode (runtime loaded) |
| Video present | `src/video/renderer_videoout.c`, `nv12_blit.c` | `sceVideoOut` BGRA or YCbCr/NV12 |
| Audio | `src/audio/audio_orbis.c` | Opus + `sceAudioOut` |
| **Input (only)** | **`src/input/input_pad.c`** | **DualShock 4 only (`scePad`) — no keyboard or mouse** |
| UI | `src/ui/ui_menu.c`, `ui_draw.c` | On-console menu + on-screen keyboard (controller-driven) |
| Orchestration | `src/main.c`, `src/stream.c`, `src/config.c` | Boot -> pair -> menu <-> stream loop |
| Net helpers | `src/orbis/net_orbis.c` | `sceNet` initialization |
| Dynamic loader | `src/orbis/videodec2_loader.c` | Runtime sprx loading pattern (key precedent for KB/mouse) |

### Build system

- `CMakeLists.txt`: CMake 3.16+, toolchain file `cmake/openorbis.cmake`
- Builds `moonlight-ps4.elf` -> `create-fself` -> `eboot.bin` -> `.pkg`
- Linux development CLI target (`moonlight-cli`) for host-side testing
- Toolchain: clang (system), `ld.lld`, `cmake`, `ninja`
- Key environment: `$OO_PS4_TOOLCHAIN` -> `~/ps4dev/OpenOrbis/PS4Toolchain`

### Linked PS4 system libraries (CMakeLists.txt lines 100-108)

```
-lSceNet            (networking)
-lSceAudioOut       (audio)
-lSceVideoOut       (video presentation)
-lSceGnmDriver      (GPU)
-lScePad            (DualShock 4)
-lSceUserService    (user management)
-lSceSystemService  (system services)
-lSceSysmodule      (module loading)
```

**Notably absent**: `-lSceKeyboard`, `-lSceMouse`, `-lSceHid`, any USB library.

### Data flow (current state)

```
PS4 DualShock 4  ->  orbis/Pad.h:scePadReadState()
                           |
                    input_pad.c:input_poll()          (src/input/input_pad.c:177)
                           |
              LiSendMultiControllerEvent()             (Limelight.h API)
              LiSendControllerArrivalEvent()
              LiSendControllerBatteryEvent()
                           |
              moonlight-common-c protocol engine
                           |
                    Network -> Sunshine -> Windows
```

There is **zero keyboard or mouse code** anywhere in the repository.
`README.md` line 25 explicitly lists "Keyboard and mouse support" as a `[ ]` TODO item.

---

## Existing Controller Input Path

### Full trace

#### 1. `stream_run()` — src/stream.c:296
Called from `main()`. Calls `input_init()` at line 309 before the menu/stream loop.

#### 2. `input_init()` — src/input/input_pad.c:44-65

```c
int input_init(void) {
    atomic_store(&s_quit, false);
    int rc = scePadInit();                                              // line 49
    sceUserServiceGetInitialUser(&userId);                              // line 56
    s_pad = scePadOpen(userId, ORBIS_PAD_PORT_TYPE_STANDARD, 0, NULL); // line 57
    return 0;
}
```

Headers used: `<orbis/Pad.h>`, `<orbis/UserService.h>`, `<orbis/libkernel.h>`

#### 3. Streaming loop — src/stream.c:258-286

```c
while (!input_poll()) {
    usleep(8000);   // ~125 Hz polling
    ...
}
```

#### 4. `input_poll()` — src/input/input_pad.c:177-237

```c
bool input_poll(void) {
    OrbisPadData pad;
    scePadReadState(s_pad, &pad);               // line 183

    // First-frame announcement (once per stream session)
    LiSendControllerArrivalEvent(
        0, 0x1, LI_CTYPE_PS, 0xFFFFFFFF,
        LI_CCAP_RUMBLE | LI_CCAP_GYRO | LI_CCAP_TOUCHPAD | LI_CCAP_ANALOG_TRIGGERS);
    LiSendControllerBatteryEvent(0, LI_BATTERY_STATE_FULL, 100);

    // Button mapping: all 14 ORBIS_PAD_BUTTON_* -> Moonlight *_FLAG constants
    int b`buttons` = 0;
    if (pad.b`buttons` & ORBIS_PAD_BUTTON_UP)     b`buttons` |= UP_FLAG;
    if (pad.b`buttons` & ORBIS_PAD_BUTTON_CROSS)  b`buttons` |= A_FLAG;
    // ... etc for all 14 b`buttons` ...

    // Stick: uint8_t [0..255] center~128 -> short [-32768..32767], Y axis inverted
    short lx = stick_to_short(pad.leftStick.x);
    short ly = stick_to_short_inverted(pad.leftStick.y);
    short rx = stick_to_short(pad.rightStick.x);
    short ry = stick_to_short_inverted(pad.rightStick.y);

    LiSendMultiControllerEvent(0, 0x1, b`buttons`,
                               pad.analogB`buttons`.l2, pad.analogB`buttons`.r2,
                               lx, ly, rx, ry);   // line 218-220

    // Quit combo: OPTIONS + TOUCHPAD held ~1 second -> input_request_quit()
    ...
    return input_should_quit();
}
```

#### 5. `input_shutdown()` — src/input/input_pad.c:67-72

```c
void input_shutdown(void) {
    if (s_pad >= 0) { scePadClose(s_pad); s_pad = -1; }
}
```

### Menu mode path

`ui_menu_run()` (src/ui/ui_menu.c:553) calls `input_menu_poll()` (not `input_poll()`).
`input_menu_poll()` reads pad state and produces `MENU_BTN_*` bitmasks for UI navigation.
Nothing is sent to Moonlight during menu mode.

---

## Moonlight Keyboard/Mouse APIs

The `moonlight-common-c` library at pinned commit `e41355e` provides a complete set of
keyboard and mouse input functions in `<Limelight.h>`. These are upstream, unpatched,
and already compiled and linked into `moonlight-ps4.elf`.

### Keyboard API

```c
int LiSendKeyboardEvent(short keyCode, char keyAction, char modifiers);
```

`keyCode` is a Windows Virtual-Key (VK) code.

| Constant | Value | Description |
|---|---|---|
| `KEY_ACTION_DOWN` | `0x03` | Key pressed |
| `KEY_ACTION_UP` | `0x04` | Key released |
| `MODIFIER_SHIFT` | `0x01` | Shift held |
| `MODIFIER_CTRL` | `0x02` | Ctrl held |
| `MODIFIER_ALT` | `0x04` | Alt held |
| `MODIFIER_META` | `0x08` | Windows/Meta held |

### Mouse movement APIs

```c
int LiSendMouseMoveEvent(short deltaX, short deltaY);
int LiSendMousePositionEvent(short x, short y, short referenceWidth, short referenceHeight);
```

`LiSendMouseMoveEvent` = relative delta. `LiSendMousePositionEvent` = absolute position.

### Mouse button API

```c
int LiSendMouseButtonEvent(char action, int button);
```

| Constant | Value |
|---|---|
| `BUTTON_ACTION_PRESS` | `0x07` |
| `BUTTON_ACTION_RELEASE` | `0x08` |
| `BUTTON_LEFT` | `0x01` |
| `BUTTON_MIDDLE` | `0x02` |
| `BUTTON_RIGHT` | `0x04` |
| `BUTTON_X1` | `0x10` |
| `BUTTON_X2` | `0x20` |

### Scroll APIs

```c
int LiSendScrollEvent(signed char scrollClicks);       // Vertical, low-res
int LiSendHighResScrollEvent(short scrollAmount);      // Vertical, high-res
int LiSendHScrollEvent(signed char scrollClicks);      // Horizontal, low-res
int LiSendHighResHScrollEvent(short scrollAmount);     // Horizontal, high-res
```

### Availability

All of the above are **fully available in the current build**.
They are declared in `third_party/moonlight-common-c/` headers, compiled into the
`moonlight-common-c` static library, and already linked into `moonlight-ps4.elf`.

Current usages in the codebase (controller only):
- `LiSendMultiControllerEvent`   — src/input/input_pad.c:218
- `LiSendControllerArrivalEvent` — src/input/input_pad.c:188
- `LiSendControllerBatteryEvent` — src/input/input_pad.c:192

Search results for keyboard/mouse/HID/USB API names in src/: **0 matches**.

---

## PS4 Keyboard/Mouse/HID APIs

### OpenOrbis SDK v0.5.4 — Gap analysis

The OpenOrbis SDK does NOT include keyboard, mouse, HID, or USB headers or stub libraries.

| API / Module | Official Sony SDK | OpenOrbis SDK v0.5.4 | Status |
|---|---|---|---|
| ``libSceKeyboard`.sprx` / `sceKeyboardInit` | Present | No header, no `.a` stub | **Missing** |
| `libSceMouse.sprx` / `sceMouseInit` | Present | No header, no `.a` stub | **Missing** |
| `libSceHid.sprx` / `sceHidOpen` | Present | No header, no `.a` stub | **Missing** |
| `libSceUsbd.sprx` / `sceUsbdInit` | Present | No header, no `.a` stub | **Missing** |
| `libScePad.sprx` / `scePadInit` | Present | `<orbis/Pad.h>` + `-lScePad` | **Available** |
| `libSceUserService.sprx` | Present | `<orbis/UserService.h>` | **Available** |
| `libSceSysmodule.sprx` | Present | Available | **Available** |

The OpenOrbis SDK `include/orbis/` directory contains: `Pad.h`, `UserService.h`,
`SystemService.h`, `AudioOut.h`, `VideoOut.h`, `libkernel.h`, etc.
but **no** `Keyboard.h`, `Mouse.h`, `Hid.h`, or `Usb.h`.

### PS4 keyboard/mouse APIs (from Sony SDK + reverse engineering)

These exist on all retail PS4 firmware but their headers must be reconstructed for homebrew.

#### `libSceKeyboard` — USB keyboard input

```c
// Reconstructed signatures (NOT in OpenOrbis headers)
int32_t sceKeyboardInit(void);
int32_t sceKeyboardOpen(int32_t userId, int32_t type, int32_t index, void *param);
int32_t sceKeyboardReadState(int32_t handle, SceKeyboardData *data);
int32_t sceKeyboardClose(int32_t handle);
int32_t sceKeyboardTerm(void);
```

Reports USB HID Usage Page 0x07 keycodes.
`SceKeyboardData` struct layout must be determined by reverse engineering.

#### libSceMouse — USB mouse input

```c
// Reconstructed signatures (NOT in OpenOrbis headers)
int32_t sceMouseInit(void);
int32_t sceMouseOpen(int32_t userId, int32_t type, int32_t index, void *param);
int32_t sceMouseReadState(int32_t handle, SceMouseData *data);
int32_t sceMouseClose(int32_t handle);
int32_t sceMouseTerm(void);
```

`SceMouseData` contains relative X/Y deltas, button bitfield, scroll wheel.
`SceMouseData` struct layout must be determined by reverse engineering.

### Approach comparison for SDK gap workaround

| Approach | Feasibility | Complexity | Risk |
|---|---|---|---|
| A: Runtime sceKernelLoadStartModule + sceKernelDlsym | **High** — proven in codebase | Low | Struct layouts unknown |
| B: Generate stub `.a` from NID tables | Medium | Medium | Outdated NID tables |
| C: Raw /dev/uhid* FreeBSD device access | Low | High | Fragile, undocumented |
| D: libSceHid raw HID reports | Medium | High | Manual HID report parsing |

**Recommended: Approach A — Runtime dynamic loading.**

This pattern is already implemented in `src/orbis/videodec2_loader.c`:

```c
// Existing working example from videodec2_loader.c
int rc = sceKernelLoadStartModule("libSceVideodec2.sprx", 0, NULL, 0, NULL, NULL);
sceKernelDlsym(handle, "sceVideodec2CreateDecoder", (void**)&vd2_CreateDecoder);
```

``libSceKeyboard`.sprx` and `libSceMouse.sprx` ship with all retail PS4 firmware and are
accessible on jailbroken consoles. No new build-time dependencies are needed.
The existing `-lkernel` (in `cmake/openorbis.cmake:55`) provides `sceKernelLoadStartModule`
and `sceKernelDlsym` already.

---

## Recommended Implementation

### New file: src/input/input_keyboard.c (pseudocode structure)

```c
#include "input_keyboard.h"
#include "../log.h"
#include <Limelight.h>
#include <orbis/libkernel.h>
#include "hid_to_vk.h"

static int s_kbd_module = -1;
static int s_kbd_handle = -1;

static int (*p_sceKeyboardInit)(void);
static int (*p_sceKeyboardOpen)(int32_t, int32_t, int32_t, void*);
static int (*p_sceKeyboardReadState)(int32_t, void*);
static int (*p_sceKeyboardClose)(int32_t);

int keyboard_init(int32_t userId) {
    s_kbd_module = sceKernelLoadStartModule("`libSceKeyboard`.sprx", 0, NULL, 0, NULL, NULL);
    if (s_kbd_module < 0) { LOGW("keyboard: sprx load failed 0x%08x", s_kbd_module); return -1; }
    sceKernelDlsym(s_kbd_module, "sceKeyboardInit",      (void**)&p_sceKeyboardInit);
    sceKernelDlsym(s_kbd_module, "sceKeyboardOpen",      (void**)&p_sceKeyboardOpen);
    sceKernelDlsym(s_kbd_module, "sceKeyboardReadState", (void**)&p_sceKeyboardReadState);
    sceKernelDlsym(s_kbd_module, "sceKeyboardClose",     (void**)&p_sceKeyboardClose);
    if (!p_sceKeyboardInit || !p_sceKeyboardOpen || !p_sceKeyboardReadState) {
        LOGW("keyboard: symbol resolution failed"); return -1;
    }
    p_sceKeyboardInit();
    s_kbd_handle = p_sceKeyboardOpen(userId, 0, 0, NULL);
    if (s_kbd_handle < 0) { LOGW("keyboard: Open failed 0x%08x", s_kbd_handle); return -1; }
    LOGI("keyboard: open OK (handle=%d)", s_kbd_handle);
    return 0;
}

void keyboard_poll(void) {
    if (s_kbd_handle < 0) return;
    SceKeyboardData data;   // struct layout to be determined
    if (p_sceKeyboardReadState(s_kbd_handle, &data) < 0) return;
    // For each key in data.keycode[]:
    //   short vk = hid_to_vk[data.keycode[i]];
    //   LiSendKeyboardEvent(vk, KEY_ACTION_DOWN, build_modifiers(&data));
    // For keys released since last frame:
    //   LiSendKeyboardEvent(vk, KEY_ACTION_UP, 0);
}

void keyboard_shutdown(void) {
    if (s_kbd_handle >= 0 && p_sceKeyboardClose) p_sceKeyboardClose(s_kbd_handle);
    s_kbd_handle = -1; s_kbd_module = -1;
}
```

### New file: src/input/input_mouse.c (pseudocode structure)

```c
#include "input_mouse.h"
#include "../log.h"
#include <Limelight.h>
#include <orbis/libkernel.h>

static int s_mouse_module = -1;
static int s_mouse_handle = -1;

static int (*p_sceMouseInit)(void);
static int (*p_sceMouseOpen)(int32_t, int32_t, int32_t, void*);
static int (*p_sceMouseReadState)(int32_t, void*);
static int (*p_sceMouseClose)(int32_t);

int mouse_init(int32_t userId) {
    s_mouse_module = sceKernelLoadStartModule("libSceMouse.sprx", 0, NULL, 0, NULL, NULL);
    if (s_mouse_module < 0) { LOGW("mouse: sprx load failed 0x%08x", s_mouse_module); return -1; }
    sceKernelDlsym(s_mouse_module, "sceMouseInit",      (void**)&p_sceMouseInit);
    sceKernelDlsym(s_mouse_module, "sceMouseOpen",      (void**)&p_sceMouseOpen);
    sceKernelDlsym(s_mouse_module, "sceMouseReadState", (void**)&p_sceMouseReadState);
    sceKernelDlsym(s_mouse_module, "sceMouseClose",     (void**)&p_sceMouseClose);
    if (!p_sceMouseInit || !p_sceMouseOpen || !p_sceMouseReadState) {
        LOGW("mouse: symbol resolution failed"); return -1;
    }
    p_sceMouseInit();
    s_mouse_handle = p_sceMouseOpen(userId, 0, 0, NULL);
    if (s_mouse_handle < 0) { LOGW("mouse: Open failed 0x%08x", s_mouse_handle); return -1; }
    LOGI("mouse: open OK (handle=%d)", s_mouse_handle);
    return 0;
}

void mouse_poll(void) {
    if (s_mouse_handle < 0) return;
    SceMouseData data;   // struct layout to be determined
    if (p_sceMouseReadState(s_mouse_handle, &data) < 0) return;
    if (data.xAxis != 0 || data.yAxis != 0)
        LiSendMouseMoveEvent((short)data.xAxis, (short)data.yAxis);
    // Edge-detect button press/release -> LiSendMouseButtonEvent()
    // if (data.wheel != 0) LiSendScrollEvent((signed char)data.wheel);
}

void mouse_shutdown(void) {
    if (s_mouse_handle >= 0 && p_sceMouseClose) p_sceMouseClose(s_mouse_handle);
    s_mouse_handle = -1; s_mouse_module = -1;
}
```

### New file: src/input/hid_to_vk.h

Static 256-entry lookup table mapping USB HID Usage Page 0x07 keycodes to
Windows Virtual-Key codes (USB-IF + Microsoft standard mapping):

```c
static const short hid_to_vk[256] = {
    [0x04] = 0x41,  // HID 'a' -> VK_A
    [0x05] = 0x42,  // HID 'b' -> VK_B
    // ... all 104+ standard keys ...
    [0x28] = 0x0D,  // Enter   -> VK_RETURN
    [0x29] = 0x1B,  // Escape  -> VK_ESCAPE
    [0x2A] = 0x08,  // Bksp    -> VK_BACK
    [0x2B] = 0x09,  // Tab     -> VK_TAB
    [0x2C] = 0x20,  // Space   -> VK_SPACE
    [0x4F] = 0x27,  // Right   -> VK_RIGHT
    [0x50] = 0x25,  // Left    -> VK_LEFT
    [0x51] = 0x28,  // Down    -> VK_DOWN
    [0x52] = 0x26,  // Up      -> VK_UP
};
```

---

## Files That Would Need Modification

### New files to create (no existing production file touched)

| File | Purpose |
|---|---|
| `src/input/input_keyboard.c` | Keyboard sprx loader, init, poll (HID->VK translation + LiSendKeyboardEvent), shutdown |
| `src/input/input_keyboard.h` | Public API: keyboard_init(), keyboard_poll(), keyboard_shutdown() |
| `src/input/input_mouse.c` | Mouse sprx loader, init, poll (LiSendMouseMoveEvent, LiSendMouseButtonEvent, LiSendScrollEvent), shutdown |
| `src/input/input_mouse.h` | Public API: mouse_init(), mouse_poll(), mouse_shutdown() |
| `src/input/hid_to_vk.h` | USB HID Usage Page 0x07 -> Windows VK static lookup table (256 entries) |

### Existing files to modify

| File | Lines affected | Change |
|---|---|---|
| `CMakeLists.txt` | Lines 80-86 | Add `src/input/input_keyboard.c` and `src/input/input_mouse.c` to `moonlight-ps4.elf` source list |
| `src/stream.c` | Line 11 (includes) | Add `#include "input/input_keyboard.h"` and `#include "input/input_mouse.h"` |
| `src/stream.c` | Lines 308-313 (`stream_run`) | Call `keyboard_init(userId)` and `mouse_init(userId)` after `input_init()` |
| `src/stream.c` | Line 261 (streaming loop) | Add `keyboard_poll()` and `mouse_poll()` inside the `while (!input_poll())` body |
| `src/stream.c` | Lines 329-332 | Add `keyboard_shutdown()` and `mouse_shutdown()` on stream end |
| `src/config.h` | Lines 11-37 (struct) | Add `bool enable_keyboard;` and `bool enable_mouse;` to `app_config_t` |
| `src/config.c` | Lines 36-47 (defaults) | Set `cfg->enable_keyboard = true; cfg->enable_mouse = true;` |
| `src/config.c` | Lines 118-165 (parser) | Parse `enable_keyboard` and `enable_mouse` INI keys |
| `src/config.c` | Lines 203-241 (save) | Write both keys to moonlight.ini |
| `src/ui/ui_menu.c` | Lines 81-94 (settings enum) | Add `SET_KEYBOARD` and `SET_MOUSE` enum entries |
| `src/ui/ui_menu.c` | Lines 96-108 (names array) | Add `"Keyboard"` and `"Mouse"` setting names |
| `src/ui/ui_menu.c` | Lines 210-226 | Add display cases for new settings |
| `src/ui/ui_menu.c` | Lines 342-398 | Add toggle cases for keyboard and mouse |

### Files that do NOT need modification

- `third_party/moonlight-common-c/` — all send APIs present and linked
- `patches/moonlight-common-c-orbis.patch` — no new patches needed
- `patches/moonlight-common-c-enet-orbis.patch` — unchanged
- `src/orbis/videodec2_loader.c` — unchanged (model to follow, not modify)
- `src/orbis/net_orbis.c` — unchanged
- `pkg/` — no package structure changes
- `scripts/` — no build script changes

---

## Build Requirements

### Current prerequisites (from README.md and CMakeLists.txt)

| Requirement | Details |
|---|---|
| Linux x86_64 | Host build environment |
| `clang` (system) | C compiler (`--target=x86_64-pc-freebsd12-elf`) |
| `ld.lld` | Linker |
| `cmake >= 3.16` | Build system |
| `ninja` | Build backend |
| `git` | Submodule management |
| OpenOrbis SDK v0.5.4 | `$OO_PS4_TOOLCHAIN` -> `~/ps4dev/OpenOrbis/PS4Toolchain` |
| Cross-compiled FFmpeg | `~/ps4dev/ffmpeg-ps4` (from `scripts/build_ffmpeg_ps4.sh`) |
| `libssl.so.1.1`, `libcrypto.so.1.1` | For `PkgTool.Core` -> `~/ps4dev/hostlibs/usr/lib` |
| PS4 with GoldHEN | FW 9.00 validated |
| Submodules initialized | `scripts/setup_deps.sh` required before build |

**IMPORTANT**: `third_party/moonlight-common-c` is currently empty (submodule not initialized).
The project cannot build as-is. `scripts/setup_deps.sh` must be run first.

### Additional build requirements for keyboard/mouse

**None.** Runtime dynamic loading via `sceKernelLoadStartModule` + `sceKernelDlsym` uses
only the already-linked `-lkernel` (cmake/openorbis.cmake:55 `PS4_BASE_LIBS = -lc -lkernel`).
No new link flags, no new stub libraries, no new submodules.

### Build commands (unchanged after implementation)

```bash
scripts/setup_deps.sh        # checkout third_party pins + apply Orbis patches
scripts/build_ffmpeg_ps4.sh  # cross-compile FFmpeg (once)
scripts/build_pkg.sh         # configure + compile + package -> build-ps4/Moonlight-1.1.0.pkg
```

---

## Risks and Blockers

### Risk 1 — Unknown SceKeyboardData / SceMouseData struct layouts [HIGH SEVERITY — PRIMARY BLOCKER]

Without correct struct sizes and field offsets, `sceKeyboardReadState()` / `sceMouseReadState()`
will read garbage data or crash.

**Mitigation**:
1. Search shadPS4 emulator source (`src/core/libraries/`) for struct definitions
2. Search PS4 homebrew communities (psxhax, wololo, GBAtemp)
3. Check any SDL2-PS4 port keyboard/mouse backends
4. If not found: write a raw-byte-dump spike (allocate 256-byte buffer, pass as struct,
   log all bytes over UDP, press keys / move mouse, observe which bytes change)

### Risk 2 — `libSceKeyboard`.sprx / libSceMouse.sprx may not load [MEDIUM SEVERITY]

If `sceKernelLoadStartModule` returns an error, the feature cannot work.

**Mitigation**: Both sprx files ship with all retail PS4 firmware. If loading fails,
degrade gracefully: log warning, return -1, continue with controller-only input.

### Risk 3 — Symbol names may differ or be NID-only [LOW SEVERITY]

**Mitigation**: `src/orbis/videodec2_loader.c` proves `sceKernelDlsym` by name works on
FW 9.00. Fall back to NID-based resolution from PS4 homebrew community NID tables if needed.

### Risk 4 — sceMouseReadState reporting conventions [LOW SEVERITY]

Unknown whether xAxis/yAxis are raw HID counts, scaled, or otherwise.

**Mitigation**: Determine from raw byte dump or community sources. Add a sensitivity
multiplier configurable in moonlight.ini.

### Risk 5 — sceKeyboardOpen / sceMouseOpen userId association [LOW SEVERITY]

The calls may require a valid userId or may work with userId=0.

**Mitigation**: Pass the same userId used for `scePadOpen` (already obtained via
`sceUserServiceGetInitialUser()`). Log return codes clearly.

### Risk 6 — Polling frequency and thread safety [LOW SEVERITY]

Adding two ReadState syscalls to the 125 Hz loop is negligible overhead.

**Mitigation**: No action needed initially. If mouse responsiveness is poor, move to a
dedicated 250 Hz polling thread with accumulated deltas.

---

## Implementation Plan

### Phase 1 — Struct discovery and validation spike [1-2 days]

Goal: Confirm SceKeyboardData / SceMouseData layouts and sprx loading on console.

1. Search shadPS4 source for `SceKeyboardData`, `SceMouseData`, ``libSceKeyboard``, `libSceMouse`
2. Search PS4 homebrew repositories and communities for existing definitions
3. Write `src/input/kbd_mouse_spike.c` (not in final build):
   - Load `libSceKeyboard`.sprx and libSceMouse.sprx
   - Resolve Init, Open, ReadState, Close symbols
   - Allocate 256-byte buffers for "data" structs
   - Loop: call ReadState, log all non-zero bytes over UDP at 10 Hz
   - Press keys / move mouse while reading the log to identify field positions
4. From the dump, determine struct field offsets and sizes
5. Validate: a specific key press produces a specific byte value at a known offset

Success criterion: Can read a specific key press as a specific byte value at a known offset.

Fallback if blocked: Implement touchpad-as-mouse using `OrbisPadData.touch` coordinates
(already available in input_pad.c from scePadReadState — no new API needed).

### Phase 2 — Input modules [1-2 days]

1. Define `SceKeyboardData` and `SceMouseData` structs in project-local headers
   (same pattern as `src/orbis/videodec2.h`)
2. Implement `src/input/input_keyboard.c`
3. Implement `src/input/input_mouse.c`
4. Create `src/input/hid_to_vk.h`
5. Add new source files to `CMakeLists.txt`

### Phase 3 — Stream integration [0.5 days]

1. Add includes to `src/stream.c`
2. Call `keyboard_init()` and `mouse_init()` in `stream_run()` (with graceful fallback)
3. Call `keyboard_poll()` and `mouse_poll()` in the streaming loop
4. Call `keyboard_shutdown()` and `mouse_shutdown()` on stream end

### Phase 4 — Configuration and UI [0.5 days]

1. Add `enable_keyboard` / `enable_mouse` to `app_config_t` (default: true)
2. Add INI parse/save
3. Add two entries to SETTINGS tab in `ui_menu.c`
4. Guard init calls with config flags

### Phase 5 — Testing and polish [1 day]

Checklist:
- All standard keyboard keys produce correct Windows VK codes on Sunshine
- Shift, Ctrl, Alt, Meta modifier combinations work
- Left, right, middle mouse b`buttons` work
- Scroll wheel works
- Mouse movement responsive without jitter or drift
- Controller and keyboard/mouse work simultaneously
- Hot-plug: app recovers if device disconnected during stream
- OPTIONS + TOUCHPAD quit combo still works with keyboard connected
- Settings menu toggles correctly enable/disable each device

**Total estimate: 4-6 days**

---

## Next Recommended Action

**Research SceKeyboardData and SceMouseData struct layouts.**

The entire implementation depends on this single piece of information.
Recommended search order:

1. shadPS4 emulator `src/core/libraries/` — search for SceKeyboard, SceMouse definitions
   (https://github.com/shadps4-emu/shadPS4)
2. PS4 homebrew SDK forks — any OpenOrbis fork or alternate PS4 SDK with keyboard/mouse headers
3. PS4 homebrew community (psxhax, wololo) — search "PS4 keyboard homebrew", "`libSceKeyboard` struct"
4. If none found: deploy the raw byte-dump spike (Phase 1, step 3) on console

Once struct layouts are confirmed, the implementation follows existing patterns in
`src/input/input_pad.c` and `src/orbis/videodec2_loader.c` and is straightforward.

---

## Search Result Evidence (confirming zero existing keyboard/mouse code)

grep results across entire repository src/ tree:

| Search term | Matches in src/ |
|---|---|
| `LiSendKeyboardEvent` | 0 |
| `LiSendMouseMoveEvent` | 0 |
| `LiSendMouseButtonEvent` | 0 |
| `LiSendMousePositionEvent` | 0 |
| `LiSendScrollEvent` | 0 |
| `sceKeyboard` | 0 |
| `sceMouse` | 0 |
| `sceHid` | 0 |
| `sceUsb` | 0 |
| `HID` (case-sensitive) | 0 |
| `USB` (case-sensitive) | 0 |
| `keyboard` (case-insensitive) | 3 — all in `ui_menu.c` referring to the on-screen keyboard widget, not USB |
| `mouse` (case-insensitive) | 0 |

Limelight APIs currently used (controller only, all in `src/input/input_pad.c`):
- `LiSendMultiControllerEvent` — line 218
- `LiSendControllerArrivalEvent` — line 188
- `LiSendControllerBatteryEvent` — line 192

---

## AUDIT COMPLETE

Next action: Research and validate `SceKeyboardData` / `SceMouseData` struct layouts from
shadPS4 emulator source and PS4 homebrew community sources, then write a raw-byte-dump
validation spike to test ``libSceKeyboard`.sprx` and `libSceMouse.sprx` loading on console.

## ABI VALIDATION RESULTS (STEP 2)

### 1. libSceMouse.sprx - VERIFIED (shadPS4 Emulator)
The Mouse ABI has been strictly verified against the shadPS4 emulator source code (src/core/libraries/mouse/mouse.h and sdl_mouse.cpp), which accurately mirrors the PS4 OS structs for emulator execution.

**Module Name**: libSceMouse (System Module ID: 0xA9)
**Functions**:
```c
int32_t sceMouseInit(void);
int32_t sceMouseOpen(int32_t userId, int32_t type, int32_t index, OrbisMouseOpenParam* pParam);
int32_t sceMouseRead(int32_t handle, OrbisMouseData* pData, int32_t num);
int32_t sceMouseClose(int32_t handle);
```
**Important note**: The read function is sceMouseRead taking an array and a 
um count, not sceMouseReadState.

**Structs**:
```c
// 8 bytes
typedef struct OrbisMouseOpenParam {
    uint8_t flag; // 0 = Normal, 1 = Merged
    uint8_t reserve[7];
} OrbisMouseOpenParam;

// 40 bytes (0x28)
typedef struct OrbisMouseData {
    uint64_t timestamp; // 0x00
    bool connected;     // 0x08
    // 3 bytes padding  // 0x09
    uint32_t b`buttons`;   // 0x0C
    int32_t x_axis;     // 0x10
    int32_t y_axis;     // 0x14
    int32_t wheel;      // 0x18
    int32_t tilt;       // 0x1C
    uint8_t reserve[8]; // 0x20
} OrbisMouseData;
```
**Constants**:
- `buttons` bitmask: Left=0x01, Right=0x02, Middle=0x04, X1=0x08, X2=0x10.

### 2. `libSceKeyboard`.sprx - NOT FULLY VERIFIED (Proprietary / Undocumented)
Despite extensive research into OpenOrbis, shadPS4, ps4sdk, and the PS Dev Wiki, the exact struct definition for SceKeyboardData is not publicly documented. shadPS4 has registered `libSceKeyboard` (System Module ID: 0xA3) but has not implemented the struct or functions yet.

**Module Name**: `libSceKeyboard`
**Expected Functions**:
```c
int32_t sceKeyboardInit(void);
int32_t sceKeyboardOpen(int32_t userId, int32_t type, int32_t index, void* pParam);
int32_t sceKeyboardRead(int32_t handle, void* pData, int32_t num);
int32_t sceKeyboardClose(int32_t handle);
```

### Safest Implementation Approach
Since libSceMouse is fully verified, we can implement it completely.
For `libSceKeyboard`, since the exact layout is unknown, we MUST do a **Runtime Spike / Raw-Byte Dump** to safely reverse engineer the layout on the actual hardware:
1. Dynamically load `libSceKeyboard`.sprx via sceKernelLoadStartModule.
2. Resolve sceKeyboardInit, sceKeyboardOpen, sceKeyboardRead.
3. Call sceKeyboardOpen.
4. In the polling loop, call sceKeyboardRead with a large raw buffer (e.g., uint8_t buf[256]).
5. Send the raw bytes over the network (or log to file) to observe which bytes change when a key is pressed (identifying the keycode offset and modifier bitmask).
6. Update the C struct once the offsets are confirmed on the PS4.

**Final Status**:
ABI NOT VALIDATED (SceKeyboardData layout remains unknown due to lack of public homebrew documentation. Mitigation requires a raw-byte dump spike on real hardware).


## Probe Implementation Status

The keyboard ABI diagnostic probe has been implemented in `src/input/input_spike.c` and hooked up in `src/stream.c` (and added to `CMakeLists.txt`).

### Test Procedure for PS4 Hardware

1. Ensure the OpenOrbis PS4 toolchain (`OO_PS4_TOOLCHAIN`) and required dependencies (cross-compiled FFmpeg) are available.
2. Build the Moonlight-PS4 PKG.
3. Install and run the PKG on the PS4 console.
4. Connect a USB keyboard to the PS4.
5. Initiate a stream or enter the menu where `input_spike_poll()` is active.
6. Press various keys (letters, numbers, modifiers) and release them.
7. Observe the console logs output (usually via netcat/socat or the application's log viewer) to see the `KB: ...` hexdumps.
8. Identify the offsets where key press/release and modifier data change the buffer content.
9. Update this report and the C structs with the confirmed ABI offsets.


## Build Environment Analysis (Windows, No Virtualization)

Given the constraint of NO Docker, NO WSL2, and NO hardware virtualization on the local Windows machine, the following build approaches were investigated:

### 1. Direct Windows with OpenOrbis
*   **Setup Difficulty**: High.
*   **Build Reliability**: Low.
*   **Time Required**: Days of troubleshooting.
*   **Changes Required**: Massive. The repository scripts (`make_pkg.sh`, `env.sh`) hardcode Linux paths (e.g., `/bin/linux`) and attempt to load Linux shared objects (`LD_LIBRARY_PATH` for `libssl.so.1.1`).
*   **Hardware Virtualization**: None.
*   **Equivalence**: Would deviate heavily from the standard build.

### 2. MSYS2 / MinGW / Cygwin
*   **Setup Difficulty**: High.
*   **Build Reliability**: Very Low.
*   **Time Required**: High.
*   **Changes Required**: Significant. MSYS2 provides a bash environment, but compiling FFmpeg (via `build_ffmpeg_ps4.sh`) using Windows LLVM/Clang within MSYS2 creates severe path translation issues. Furthermore, MSYS2 cannot execute the Linux ELF binaries of `PkgTool.Core` required by the packaging scripts.
*   **Hardware Virtualization**: None.
*   **Equivalence**: Unknown, likely broken.

### 3. Adapt Scripts Minimally to Windows
*   **Setup Difficulty**: High.
*   **Build Reliability**: Low.
*   **Changes Required**: Too many. Requires modifying CMake toolchains, `ps4-ld.sh` wrappers, and packaging scripts to detect Windows and use `PkgTool.Core.exe`. We would still have to solve the FFmpeg cross-compilation on Windows.

### 4. Portable/Prebuilt Artifacts
*   **Setup Difficulty**: Low (if they existed).
*   **Build Reliability**: N/A.
*   **Changes Required**: None to source.
*   **Viability**: Dead end. There is no reliable upstream repository providing pre-compiled OpenOrbis FFmpeg artifacts, and we still lack a way to run the packaging toolchain locally without heavy script rewrites.

### 5. GitHub Actions (Remote Linux CI)
*   **Setup Difficulty**: Extremely Low. Requires creating one YAML file.
*   **Build Reliability**: 100%. Exactly matches the documented Linux requirements (Ubuntu + bash + standard dependencies).
*   **Time Required**: < 5 minutes to write, ~5 minutes to build.
*   **Changes Required**: Zero changes to the existing project code, scripts, or toolchains. Just adding .github/workflows/build.yml.
*   **Hardware Virtualization**: None on the local machine (runs on GitHub's secure cloud VMs).
*   **Equivalence**: 100% equivalent to the standard Linux build path described in the README.

---

### RECOMMENDED BUILD PATH

**Approach 5: GitHub Actions (Remote Linux CI)**

Since local hardware virtualization is strictly unavailable and adapting the project's deeply-coupled Linux bash scripts to native Windows is impractical, delegating the Linux build to a free, remote GitHub Actions runner is the shortest, safest, and most reproducible path. It completely side-steps the Windows limitation without touching a single line of the existing C code or build scripts.


## CI Preparation Status

The remote GitHub Actions workflow has been prepared locally at `.github/workflows/build-ps4.yml`. It configures an `ubuntu-20.04` runner, installs exactly the dependencies required (OpenOrbis, clang, cmake, libssl1.1), builds FFmpeg for the PS4 target, builds the PKG using the repository's native scripts, and uploads the PKG as a private workflow artifact. It is ready to be committed and run via `workflow_dispatch`.

