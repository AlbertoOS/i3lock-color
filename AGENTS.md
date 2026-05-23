# AGENTS.md — i3lock-color Technical Reference

> **Origin:** This file was produced by a deep multi-agent AI analysis of the i3lock-color codebase.
> It is intended as a technical reference for AI agents working on this repository — covering architecture,
> key file roles, security model, and known concerns. It complements `TODO.md`, which captures the
> actionable fixes discovered during the same analysis session.
>
> The analysis combined static code reading, cross-file tracing of call paths, and security/performance
> reasoning across all C source files. No automated tooling was used — the findings are based entirely
> on code comprehension.

## Overview

i3lock-color is a fork of [i3lock](https://github.com/i3/i3lock) that adds extensive customization: colors, blur, clock/date display, bar indicator, GIF animation, JPEG support, text positioning via mathematical expressions, hotkey passthrough, and more. Upstream i3lock is intentionally minimal; this fork is feature-rich and community-maintained by Raymond Li (Raymo111).

## Architecture

### Process Model

```
main() ──→ fork() [after MapNotify]
 │            └─ parent exits (lets caller know lock is active)
 │            └─ child continues as the lock process
 │
 ├─ fork() [before MapNotify]
 │    └─ raise_loop child: separate XCB connection,
 │       watches for visibility changes and re-raises the window
 │
 └─ main event loop (libev)
      ├─ xcb_check_cb: polls XCB events (key press, visibility, configure, xkb)
      ├─ time_redraw_tick: periodic timer for clock updates
      ├─ gif_anim_loop: timer for GIF frame advancement
      ├─ clear_auth_wrong_timeout: 2s timer after failed auth
      ├─ clear_indicator_timeout: 1s timer to hide indicator
      └─ discard_passwd_timeout: 3min timer to clear forgotten password
```

**Optional threading:** `--redraw-thread` spawns a pthread that calls `redraw_screen()` at `refresh_rate` intervals instead of using libev's periodic timer. This is a workaround for when the ev loop is blocked (e.g., during PAM auth).

### PAM Flow (Linux)

1. `pam_start("i3lock", username, &conv, &pam_handle)` — initializes PAM with the `i3lock` service
2. PAM config file: `/etc/pam.d/i3lock` (includes `system-local-login` on Arch/Gentoo, `login` on Debian)
3. On Enter: `pam_authenticate(pam_handle, 0)` — **synchronous, blocks the event loop**
4. On success: `pam_setcred(pam_handle, PAM_REFRESH_CRED)` refreshes Kerberos tickets etc.
5. On exit: `pam_end(pam_handle, PAM_SUCCESS)` cleanup
6. `conv_callback()` supplies the password to PAM when asked (`PAM_PROMPT_ECHO_OFF` or `PAM_PROMPT_ECHO_ON`)

**OpenBSD:** Uses `auth_userokay()` (BSD Auth) instead of PAM.

### Display / X11 Model

- Window: fullscreen, `override_redirect=1`, depth 32 (ARGB), covers entire root
- Uses `_NET_WM_BYPASS_COMPOSITOR` to disable compositing flicker
- Optional: `--composite` uses XComposite overlay window as parent
- Keyboard/pointer: grabbed exclusively via `xcb_grab_pointer`/`xcb_grab_keyboard` (up to 10000 retries)
- Monitor detection: RandR 1.5 → RandR 1.4 → Xinerama fallback
- Background: either solid color, blurred screenshot, or image (PNG/JPEG/GIF/raw)
- Rendering: cairo on an in-memory ARGB32 surface, then blitted to XCB surface

### Unlock Flow

1. User types password → stored in `static char password[512]` (mlock'd)
2. Enter/Ctrl+M/Ctrl+J triggers `finish_input()` → `input_done()`
3. `input_done()` calls `pam_authenticate()` (blocking)
4. Success → `ev_break()` exits loop → `pam_end()` → restore focus → exit
5. Failure → `STATE_AUTH_WRONG` for 2 seconds, failed_attempts++, password cleared
6. During `STATE_AUTH_WRONG`, new Enter queues `retry_verification`

## Key Files

| File | Purpose |
|------|---------|
| `i3lock.c` | Main entry point, event loop, PAM, key handling, image loading, CLI parsing |
| `xcb.c` | XCB helpers: window creation, pixmaps, cursor, keyboard grab, screen capture |
| `xcb.h` | XCB declarations, extern `conn`/`screen` |
| `unlock_indicator.c` | All rendering: indicator circle, bar, text, image scaling, per-monitor layout |
| `unlock_indicator.h` | State enums (`unlock_state_t`, `auth_state_t`), `DrawData` struct |
| `blur.c` | Gaussian blur via repeated box filter (scalar fallback + SSE2 dispatch) |
| `blur_simd.c` | SSE2 implementation of horizontal blur pass |
| `blur.h` | Blur constants: `KERNEL_SIZE=7`, `SIGMA_AV=2`, `HALF_KERNEL=3` |
| `randr.c` | Multi-monitor detection: RandR 1.5/1.4/Xinerama |
| `randr.h` | `Rect` struct, `xr_screens`, `xr_resolutions` |
| `dpi.c` | DPI detection from `Xft.dpi` resource or physical screen size |
| `jpg.c` | JPEG loading via libjpeg into cairo-compatible BGRA buffer |
| `jpg.h` | `JPEG_INFO` struct, `read_JPEG_file()`, `file_is_jpg()` |
| `tinyexpr.c/h` | Embedded math expression evaluator for position strings |
| `fonts.h` | `text_t` struct with font, color, position, alignment |
| `rgba.h` | `rgba_t` (doubles 0-1) and `rgba_str_t` (hex string pairs) |
| `cursors.h` | Cursor type enum (`CURS_NONE`, `CURS_WIN`, `CURS_DEFAULT`) |
| `i3lock.h` | `DEBUG` macro, `NANOSECONDS_IN_SECOND` |
| `pam/i3lock` | PAM service config file |
| `Makefile.am` | Autotools build definition |
| `configure.ac` | Autoconf: dependencies, flags, PAM detection, version |
| `build.sh` | Quick build script: `autoreconf -fiv && mkdir build && configure && make` |

## How Blur Works

### Algorithm

The blur uses **repeated box filtering** to approximate a Gaussian blur, based on [Peter Kovesi's paper](http://www.peterkovesi.com/papers/FastGaussianSmoothing.pdf).

1. A fixed 7×7 box filter (KERNEL_SIZE=7) has σ_av = √((7²-1)/12) = 2.0
2. Repeating it `n` times gives effective σ_n = √(n) × σ_av
3. Therefore: `n = (σ_desired / σ_av)² = σ² / 4`
4. Minimum n=3 (enforced)

Each iteration performs **two horizontal passes with transposition**:
- Pass 1: blur horizontally, write transposed (row→column)
- Pass 2: blur the transposed image horizontally (effectively vertical), write transposed back

This avoids a separate vertical pass and improves cache locality on the second pass.

### Why High Sigma is Slow at 4K

**Complexity: O(n × 2 × W × H × KERNEL_SIZE)**

| sigma | n (iterations) | Operations at 3840×2160 | Approx time |
|-------|----------------|-------------------------|-------------|
| 5 | 6 | 580M | ~50ms |
| 10 | 25 | 2.4B | ~200ms |
| 20 | 100 | 9.7B | ~800ms |
| 40 | 400 | 38.7B | ~3s |

The KERNEL_SIZE is fixed at 7. The only way to increase blur strength is to increase iterations. At 4K, each iteration touches ~33MB of pixel data (2× for read+write), so cache pressure is extreme.

### SSE2 Path

When `__SSE2__` is defined (default on x86_64), `blur_impl_horizontal_pass_sse2()` is used. It processes 4 pixels at a time via 128-bit XMM registers and uses `_mm_mul_ps` for the averaging. The generic fallback processes one pixel at a time with scalar integer math.

**Notable:** No AVX2/AVX-512 path exists. No GPU compute path. No multi-threaded blur.

## How Image Loading Works

### Supported Formats

| Format | Detection | Library | Notes |
|--------|-----------|---------|-------|
| PNG | 8-byte magic header | cairo (built-in) | `cairo_image_surface_create_from_png()` |
| JPEG | 2-byte magic `0xFFD8` | libjpeg | Decoded to BGRA → cairo ARGB32 surface |
| GIF | "GIF" + version header | giflib | All frames loaded; animated via ev_timer |
| Raw | `--raw` flag | custom | Format string: `WIDTHxHEIGHT:pixfmt` |

### Scaling Path

Image scaling is done in `draw_image()` (unlock_indicator.c) using **cairo matrix transforms**:
- `NONE`: paint at 0,0 (no scaling)
- `TILE`: `CAIRO_EXTEND_REPEAT`
- `CENTER`: translate to center of each monitor
- `SCALE`: stretch to fill (distorts)
- `FILL`: scale to cover (crops)
- `MAX`: scale to fit (letterboxes)

Scaling is per-monitor: each monitor gets its own transform matrix.

### Slideshow

- Directory mode: regex-matches `*.jpg` and `*.png` files
- Hard limit: 256 images (`img_slideshow[256]`)
- Interval: `--slideshow-interval` (default 10s)
- Random selection: `--slideshow-random-selection`
- Re-reads directory each cycle (allows adding images while locked)

## Security Model

### Privileges Required

- **No root required** — i3lock runs as the invoking user
- `mlock()` on the password buffer requires `RLIMIT_MEMLOCK` (default sufficient on Linux since 2.6.9)
- PAM module may require the user to be in certain groups depending on config
- The PAM service file (`/etc/pam.d/i3lock`) determines authentication backend

### Security Measures

1. **Password memory**: `mlock()`'d to prevent swapping, cleared with `explicit_bzero()` after auth
2. **Keyboard/pointer grab**: exclusive grab prevents other apps from receiving input
3. **override_redirect**: window cannot be managed/moved/hidden by WM
4. **Visibility monitoring**: raise_loop child re-raises window if obscured
5. **Window stacking**: always raised to top on visibility change
6. **XSS_SLEEP_LOCK_FD**: integrates with `xss-lock` for proper suspend/resume locking
7. **No network**: all auth is local via PAM
8. **Wayland rejection**: exits with error if `WAYLAND_DISPLAY` is set

### Known Security Concerns

1. **`system()` for hotkey commands** — shell injection if commands are attacker-controlled (they come from CLI args, so only a concern if the i3lock invocation itself is compromised)
2. **Debug mode prints password** — `DEBUG("current password = %.*s\n", ...)` outputs plaintext to stderr
3. **`--no-verify` bypasses auth** — intended for testing, should never be used in production
4. **PAM strdup leaks** — `conv_callback` uses `strdup(password)` which PAM frees, but if PAM has a bug, the copy persists in memory
5. **raise_loop child** — has access to the password buffer memory (it inherits it via fork), though it doesn't use it
6. **X11 inherent weaknesses** — other X11 clients with sufficient access can read the screen, inject events if they bypass the grab; this is an X11 limitation, not an i3lock bug

## Build System

### Dependencies

```
xcb xcb-xkb xcb-xinerama xcb-randr xcb-composite
xcb-image xcb-event xcb-util xcb-atom xcb-xrm
xkbcommon xkbcommon-x11
cairo cairo-xcb cairo-ft
libjpeg
fontconfig
giflib
libev
pam (except OpenBSD)
iconv
```

### Building

```sh
# Quick build
./build.sh

# Manual
autoreconf -fiv
mkdir build && cd build
../configure --prefix=/usr --sysconfdir=/etc
make
sudo make install
```

### Configure Flags

- `--enable-debug`: enables ASan, debug assertions
- `--prefix=/usr --sysconfdir=/etc`: standard install paths
- PAM config auto-detects Arch/Gentoo vs Debian via `/etc/arch-release`

### Compiler Flags

- `-O2 -funroll-loops -pthread` (from configure.ac)
- `-Wall` (from `AX_FLAGS_WARN_ALL`)
- SSE2 is auto-detected via `__SSE2__` preprocessor macro (always defined on x86_64)

## Upstream Divergence

i3lock-color diverged significantly from upstream i3lock (last upstream: 2.13, Oct 2020). Key additions:

- All color customization (indicator, ring, text, backgrounds)
- Clock/date display with fonts
- Blur (the entire blur subsystem)
- JPEG and GIF support
- Bar indicator (alternative to circle)
- Hotkey passthrough and custom commands
- tinyexpr for position arithmetic
- Slideshow mode
- Greeter text
- Text outline support
- DPI-aware scaling
- Composite window support

Upstream stripped features (DPMS) and stays minimal. This fork will never re-merge with upstream.

## Conventions for This Fork

### Code Style
- Tabs for indentation in older code, spaces (4) in newer — `.clang-format` exists but is not consistently applied
- Global state via `extern` declarations across files
- Color arrays: 9-char strings (`"rrggbbaa\0"`)

### Thread Safety
- `redraw_screen()` is called from both main thread and optional redraw pthread — **no locking exists**, this is a latent race condition
- XCB connection is shared globally — XCB is thread-safe for requests but not for event polling

### Expression Evaluation
- `tinyexpr` is used for all position strings (indicator, time, date, bar, etc.)
- Variables available: `w`, `h`, `x`, `y`, `ix`, `iy`, `tx`, `ty`, `dx`, `dy`, `bw`, `bx`, `by`, `r`
- Expressions are **recompiled every frame** in `render_lock()` — should be cached
