# Known bugs

A running list of confirmed defects not yet fixed, most-recently-found first.
Each entry: symptom, root cause (if known), impact, and the likely fix. Delete
an entry when it lands a fix + regression test.

---

## GUI-test process aborts at exit (`vkDeviceWaitIdle` on a destroyed device)

**Found:** 2026-07-22, while adding the feature-tree drag-reorder GUI test.

**Symptom:** `designer --gui-tests --hidden` prints `SIGABRT`/`SIGSEGV` and
dumps core *after* the harness reports results. The crash handler backtrace is:

```
abort
vkDeviceWaitIdle
UnderlayRenderer::Shutdown()
std::unique_ptr<UnderlayRenderer>::~unique_ptr()
__run_exit_handlers        <-- atexit, i.e. after main() returned
exit
```

Reproduces on both the full suite and a single `-f <filter>` run; it is **not**
specific to any one test (seen on `app_boots_and_renders`, which touches no
model code).

**Root cause:** `g_UnderlayRenderer` is a **static/global** `unique_ptr`
(`src/main.cpp:99`). Its destructor runs during C++ static teardown at
`exit()` — *after* `main()` has already torn down the Vulkan device — and calls
`UnderlayRenderer::Shutdown()` → `vkDeviceWaitIdle(m_device)`
(`src/UnderlayRenderer.cpp:582`) on a device handle that is already destroyed,
which aborts. A static-destruction-order fiasco: the global renderer outlives
the Vulkan device it references.

**Impact:** cosmetic for CI/testing — the test harness reports pass/fail and the
`tools/gui-tests-xvfb.sh` wrapper captures the exit status *before* atexit runs,
so suites still gate correctly (they report `N/N passed`, wrapper exits 0). But
it is a real latent teardown bug: any orderly-shutdown path that relies on the
global's destructor (rather than an explicit reset before device destruction)
has the same use-after-free, and the core dumps are noise that can mask a real
teardown regression.

**Likely fix:** during `main()`'s orderly shutdown — before the Vulkan device is
destroyed — explicitly `g_UnderlayRenderer.reset()` (and any sibling global
renderers with Vulkan-dependent destructors), so no Vulkan call runs from a
static destructor at `exit()`. Alternatively, have `Shutdown()` no-op when the
device is already gone. Add a regression check that a `--gui-tests --hidden` run
exits with status 0 *and* no core dump.
