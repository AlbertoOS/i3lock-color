# TODO.md — i3lock-color Fixes and Improvements

> **Origin:** This file was produced by a deep multi-agent AI analysis of the i3lock-color codebase.
> Each item below was identified by reading the source code — not by running linters or static analyzers.
> Issues are grouped by category and severity (HIGH/MEDIUM/LOW). See `AGENTS.md` for the architectural
> context that informed these findings.
>
> Items marked HIGH should be addressed before any new features are added. MEDIUM items represent
> correctness or quality gaps. LOW items are polish or edge-case hardening.

## Security

### HIGH: `system()` calls enable shell injection

**Problem:** `handle_key_press()` (i3lock.c:752-835) calls `system(cmd_brightness_up)` etc. with strings from CLI arguments. While the strings come from the user's own invocation, if i3lock is called from a script that interpolates untrusted data into these flags, it's a shell injection vector. `system()` also inherits environment and spawns a shell.

**Fix:** Replace all `system()` calls with `fork()`+`execvp()` (split on spaces or use a simple tokenizer). This eliminates shell interpretation entirely. Alternatively, at minimum use `execl("/bin/sh", "sh", "-c", cmd, NULL)` in a forked child with a clean environment.

---

### HIGH: Debug mode prints password in plaintext

**Problem:** i3lock.c:988 — `DEBUG("current password = %.*s\n", input_position, password);` outputs the password to stderr when `--debug` is used. If stderr is redirected to a file or journal, the password is logged.

**Fix:** Remove this line entirely or replace with `DEBUG("password length = %d\n", input_position);`. A screen locker should never log credentials, even in debug mode.

---

### MEDIUM: PAM password copy not explicitly zeroed

**Problem:** `conv_callback()` (i3lock.c:1483) uses `strdup(password)` to create a copy for PAM. PAM's `pam_end()` should free this, but if PAM fails or the module doesn't free it, the copy persists. Additionally, `strdup` uses `malloc` which may leave the string in freed heap memory indefinitely.

**Fix:** No clean fix without PAM API changes. Document the risk. Consider using `pam_set_item(PAM_AUTHTOK)` if the PAM module supports it, or track the allocation and `explicit_bzero` it after `pam_authenticate` returns.

---

### MEDIUM: `--no-verify` flag bypasses all authentication

**Problem:** i3lock.c:589 — `if (no_verify) { ev_break(...); return; }` unlocks immediately without any password check. This is dangerous if someone copies a lock script and forgets to remove the flag, or if a wrapper script has a bug.

**Fix:** Print a prominent warning to stderr when `--no-verify` is active: `"WARNING: --no-verify is active, screen is NOT secured"`. Consider requiring `--debug` to be set simultaneously, or removing the feature entirely.

---

### MEDIUM: Slideshow path buffer overflow

**Problem:** `load_slideshow_images()` (i3lock.c:1720-1726) uses `char path_to_image[256]` and `strcpy`/`strcat` without bounds checking. If `path` (the directory) + `"/"` + `dir->d_name` exceeds 255 chars, stack buffer overflow occurs.

**Fix:** Use `snprintf(path_to_image, sizeof(path_to_image), "%s/%s", path, dir->d_name)` and check for truncation, or use `asprintf()`.

---

### MEDIUM: `get_colorpixel()` alpha channel bug

**Problem:** xcb.c:89 — `{hex[6], hex[4], '\0'}` should be `{hex[6], hex[7], '\0'}`. The alpha byte reads `hex[4]` (blue high nibble) instead of `hex[7]` (alpha low nibble). This means alpha is always wrong for the window background color.

**Fix:** Change line 89 to `{hex[6], hex[7], '\0'}`.

---

### LOW: raise_loop child inherits password buffer

**Problem:** The `fork()` at i3lock.c:2807 creates a child that has a copy of the entire address space, including the mlock'd password buffer. The child never uses it but it exists in its memory space.

**Fix:** Clear the password buffer in the child immediately after fork, or move the fork before password entry begins (it's already positioned that way, since password entry happens in the event loop, but the buffer exists and may contain stale data from a previous attempt if the fork happens to be delayed).

---

### LOW: `xcb_send_event` for key passthrough is unsafe

**Problem:** i3lock.c:851 — `xcb_send_event(conn, true, screen->root, XCB_EVENT_MASK_BUTTON_PRESS, (char *)event)` forwards media/power/screen keys to the root window. A malicious local application watching root events could detect lock-screen activity timing.

**Fix:** Document this as a known information leak. Consider making `--pass-media-keys` etc. opt-in only (they already are) and warn in docs.

---

## Performance

### HIGH: Blur is O(σ²) — catastrophically slow at high sigma on 4K

**Problem:** The blur algorithm repeats a 7×7 box filter `n = σ²/4` times. At 4K (8.3M pixels) with σ=20, that's 100 iterations × 2 passes × 8.3M × 7 = ~11.6 billion operations. Takes 800ms+ on modern hardware.

**Fix approaches (in order of impact):**

1. **Use a proper separable Gaussian with variable kernel width.** Instead of repeated 7×7 box blur, compute a single-pass Gaussian kernel of radius `3σ` (truncated). For σ=20, kernel width=121. One pass: 2 × W × H × 121 ≈ 4B ops. This is 3× faster than repeated box blur and produces better quality.

2. **Use the "three-pass box blur" with adaptive box width** (Kovesi's actual recommendation). Instead of fixed 7×7 repeated n times, compute 3 box filters with widths `w₁, w₂, w₃` chosen to approximate the target σ. This gives O(W×H×3×2) = O(W×H×6) regardless of σ (constant time!). Ivan Googol's algorithm.

3. **Stackblur algorithm** — O(W×H) constant-time regardless of radius. Used by Android, Chromium. No iteration needed.

4. **Downscale → blur → upscale** — Reduce image to 1/4 resolution, blur with σ/4, scale back up. 16× speedup for essentially the same visual result on a lock screen.

5. **Multi-threaded blur** — Split image into horizontal strips, process in parallel. Easy 4-8× speedup on multi-core.

6. **AVX2 SIMD** — Process 8 pixels at a time instead of 4 (SSE2). 2× speedup.

---

### HIGH: Expression re-compilation every frame

**Problem:** `render_lock()` in unlock_indicator.c:1003-1024 calls `te_compile()` for ~20 expressions on every single frame redraw. Expression compilation involves parsing, AST allocation, etc. This is called on every keypress, every clock tick, every GIF frame.

**Fix:** Compile expressions once at startup (after all options are parsed) and store them globally. They only reference variables by pointer, so they remain valid as long as the variable pointers don't change (they don't — they're stack variables in render_lock, but could be made static/global).

---

### MEDIUM: Full-screen redraw on every keypress

**Problem:** `redraw_screen()` creates a new pixmap, renders the entire screen (background + image + all indicators + all text), blits it, and frees it. On 4K, that's allocating and filling a 33MB buffer per keypress.

**Fix:** 
1. Cache the background (color/blur/image) in a persistent pixmap — it never changes after startup
2. Only redraw the indicator region (bounding box around the circle/bar)
3. Use `xcb_copy_area` to restore the cached background in the dirty region before redrawing the indicator

---

### MEDIUM: Image decode blocks startup

**Problem:** PNG/JPEG/GIF loading happens synchronously in `main()` before the lock window is shown. A large image (e.g., 4K JPEG at high quality) takes 100-500ms to decode, during which the screen is not yet locked.

**Fix:** Show the lock window with a solid color immediately (the grab already happens), then load the image asynchronously and update the display. The security-critical step (grab + window map) should not be delayed by cosmetic image loading.

---

### MEDIUM: Blur is done on startup before window map

**Problem:** i3lock.c:2759-2773 — screen capture + blur happens before the window is opened. For high sigma at 4K, this can add 1-3 seconds of delay where the screen is NOT locked.

**Fix:** Same as above — lock immediately with a solid/tinted overlay, then perform blur in background and update. Or pre-capture the screen before showing the window, but apply blur after the window is mapped and grab is active.

---

### LOW: `FcInit()`/`FcFini()` called per font face

**Problem:** `get_font_face()` in unlock_indicator.c calls `FcInit()` and `FcFini()` each time. While font faces are cached (only resolved once), the init/fini cycle is unnecessary after the first call and may cause issues if fontconfig has global state.

**Fix:** Call `FcInit()` once at startup. Never call `FcFini()` (most programs don't — it's only for leak checkers).

---

### LOW: GIF memory usage for many-frame animations

**Problem:** All GIF frames are loaded into memory simultaneously (`gif_img = malloc(gif->ImageCount * sizeof(struct gif))`). A 100-frame 4K GIF would require ~3.3GB of RAM.

**Fix:** Add a sanity check on `gif->ImageCount * width * height * 4` and reject GIFs that would exceed a reasonable limit (e.g., 500MB). Or decode frames on-demand.

---

## Correctness

### MEDIUM: Thread safety of `redraw_screen()`

**Problem:** When `--redraw-thread` is active, `start_time_redraw_tick_pthread()` calls `redraw_screen()` from a separate thread while the main thread may also call it (on key events, auth state changes, etc.). `redraw_screen()` uses the global XCB connection and modifies shared state (`bar_heights`, draw data). No mutex exists.

**Fix:** Add a mutex around `redraw_screen()`, or better, have the redraw thread just set a flag that the main loop checks, ensuring all rendering happens on the main thread.

---

### MEDIUM: Slideshow `load_image()` prototype mismatch

**Problem:** unlock_indicator.c:168 declares `cairo_surface_t* load_image(char* image_path);` but the actual function in i3lock.c:1651 has signature `cairo_surface_t *load_image(enum IMAGE_FORMAT format)`. This is undefined behavior.

**Fix:** Update the declaration in unlock_indicator.c to match the actual signature, or refactor the slideshow code to use `verify_image()` + `load_image()` correctly.

---

### MEDIUM: Xinerama fallback uses wrong variable

**Problem:** randr.c:271 — `for (int screen = 0; screen < xr_screens; screen++)` uses `xr_screens` but this hasn't been updated yet (it's still from a previous query or 0). Should use the local `screens` variable.

**Fix:** Change `screen < xr_screens` to `screen < screens`.

---

### LOW: `--screen` flag indexing confusion

**Problem:** unlock_indicator.c:1027 — `if (screen_number < 0 || screen_number > xr_screens)` allows `screen_number == xr_screens` which would be out-of-bounds (0-indexed array). The logic at line 1033 `screen_number == 0 ? 0 : screen_number - 1` treats 0 as "all screens" and 1-N as specific screens, but the validation should be `screen_number > xr_screens` (which it is, but the off-by-one in the comparison means N is valid when the array only has indices 0..N-1).

**Fix:** Change to `screen_number < 0 || screen_number > xr_screens` is actually correct since screen_number=1 means index 0. Document this 1-based indexing clearly.

---

### LOW: GIF disposal mode bug

**Problem:** i3lock.c:1246 — `data_size = width * height * ((int)sizeof(width))`. `sizeof(width)` is `sizeof(int)` which is 4 on most platforms, coincidentally correct for ARGB32. But this is fragile and wrong on platforms where int ≠ 4 bytes.

**Fix:** Use `width * height * sizeof(uint32_t)` or `stride * height`.

---

### LOW: GIF `memset` with color value

**Problem:** i3lock.c:1256 — `memset(data, bg_color, data_size)` uses `memset` with a uint32_t color value. `memset` fills byte-by-byte, so only the lowest byte of `bg_color` is used. The background fill is incorrect for any color where the bytes aren't all the same.

**Fix:** Use a loop: `for (int i = 0; i < width * height; i++) data[i] = bg_color;`

---

### LOW: GIF data_prev doesn't handle DISPOSE_PREVIOUS

**Problem:** The GIF decoder only handles `DISPOSE_DO_NOT` and `DISPOSE_BACKGROUND`. `DISPOSE_PREVIOUS` (restore to previous frame) is not implemented — it falls through to the default which zeroes the frame.

**Fix:** Implement `DISPOSE_PREVIOUS` by keeping a copy of the frame before rendering the current one, and restoring from it.

---

## Build / Maintenance

### MEDIUM: PAM config sed in configure.ac is fragile

**Problem:** configure.ac:83-84 uses `sed -i` to modify `../pam/i3lock` during configuration. This modifies the source file in-place, fails in out-of-tree builds, and makes the build non-reproducible.

**Fix:** Use `AC_CONFIG_FILES` to generate the PAM file from a template (`pam/i3lock.in`), using an autoconf substitution variable for the `include` target.

---

### LOW: No `-msse2` flag explicitly passed

**Problem:** The build relies on `__SSE2__` being defined implicitly (it is on x86_64), but on 32-bit x86, SSE2 might not be the default. There's no configure check or explicit `-msse2` flag.

**Fix:** Add an `AC_CHECK_COMPILE_FLAG([-msse2])` and conditionally add it for 32-bit builds, or add a `--disable-simd` configure option.

---

### LOW: Missing `-lpthread` in LDADD

**Problem:** `Makefile.am` doesn't explicitly link `-lpthread` despite using `pthread_create`. The `-pthread` CFLAG (from configure.ac:124) handles both compilation and linking on most systems, but not all.

**Fix:** Add `$(PTHREAD_LIBS)` to `i3lock_LDADD` or use `AX_PTHREAD` macro for proper detection.

---

## Wishlist (Low Priority)

- [ ] Wayland support (or at least a clear "use swaylock" message — already done)
- [ ] Config file support (instead of 100+ CLI flags)
- [ ] Proper dirty-region rendering (only redraw indicator area)
- [ ] GPU-accelerated blur (OpenGL/Vulkan compute shader)
- [ ] Password length indicator (dots) option
- [ ] Accessibility: screen reader support, high-contrast mode
- [ ] Proper signal handling (SIGTERM for graceful exit)
- [ ] Valgrind-clean shutdown (many cairo/xcb resources are leaked on exit)
