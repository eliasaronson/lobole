# Wayland: what the session move actually broke

The desktop session is Wayland now, and the app still assumes X11 in three
places. None of them stops it running — the window opens, the GPU works, the
remote port serves — but each one either spams the log, throws away a
preference, or does real work for nothing.

## The three

**1. A font-atlas rebuild per configure event.** The content-scale callback
sets a *pending* flag whenever it fires, and the main loop treats "fired" as
"changed": it destroys the font texture, re-applies the whole theme, rebuilds
the atlas and re-uploads it. On X11 that callback fires when a window moves
between monitors — rarely. On Wayland `wp_fractional_scale_v1` re-sends the
preferred scale on configure, so dragging or resizing the window rebuilds the
atlas repeatedly. The fix is to compare the value against the applied one and
do nothing when they match, which is also correct on X11.

This is the item with a cost attached; the other two are hygiene.

**2. Window position.** Wayland does not provide or accept window positions by
design — a client may not know or choose where it sits. We call
`glfwSetWindowPos` when restoring saved geometry (one error at startup) and
`glfwGetWindowPos` every time geometry is recorded (an error per window event).
The second one is not only noise: `glfwGetWindowPos` zeroes its out-params
before failing, so the recorded rect becomes x=0, y=0, which differs from what
is stored, which marks settings dirty and **writes 0,0 to settings.json**. A
Wayland session therefore destroys the window position an X11 session saved.

**3. The X11 `XMapWindow` show path.** `ShowWindowNonBlocking` exists to dodge
a GLFW deadlock under tiling WMs, and already falls back to `glfwShowWindow`
when the X11 handles come back null — but it *asks* first, so GLFW logs two
`X11: Platform not initialized` errors before the fallback. Ask the platform
instead of asking for a handle.

## Shape of the fix

One helper, `glfwGetPlatform() == GLFW_PLATFORM_WAYLAND`, and four guards in
`src/app/main.cpp`: the content-scale comparison, the position set, the
position get, and the show path. No behaviour changes on X11.

## 4. The DPI scale was applied TWICE

Found after the three above, by comparing the app against other programs on
the same screen: everything in it was visibly larger. It was, by 1.4x.

On Wayland GLFW scales the framebuffer itself — measured, window 1350x1465
against a framebuffer of 1890x2051 — and the ImGui backend reports that ratio
as `io.DisplayFramebufferScale`, so every ImGui coordinate is already
multiplied by 1.4 on its way to the screen. The theme then multiplied its
metrics and font sizes by the content scale as well: 1.4 x 1.4 = **1.96**.
Measured in-frame: `DisplaySize 1350x1490, FramebufferScale 1.40,
fontSize 22.0` — 22 logical pixels is 30.8 physical, where a 1.4 display
wants 22.4.

On X11 the framebuffer IS the window, that ratio is 1.0, and multiplying by
the content scale is exactly right. Which is why this had never shown up:
until this week every window this app drew was an X11 one.

The two are separate knobs and `ApplyTheme` now takes both:

- **style metrics** scale by `uiScale / framebufferScale` — ImGui's own
  coordinate space, logical.
- **the font atlas** is rasterized at `uiScale`, i.e. PHYSICAL pixels, and
  `io.FontGlobalScale = 1 / framebufferScale` divides it back down for
  layout. That is what keeps text crisp: a 22 px atlas shown at 22 physical
  pixels, rather than a 16 px atlas magnified 1.4 times.

On X11 both reduce to today's behaviour exactly (`fb == 1`), which the GUI
suite — which runs under Xvfb, on X11 — is what protects.

## Not a bug, worth knowing

The UI scale arrives LATE on Wayland. The surface has no output and no
preferred scale when the window is created, so `glfwGetWindowContentScale`
answers 1.0; the compositor sends the real value (1.4 here, from
`wp_fractional_scale_v1`'s 168/120) a moment later, and the theme rebuilds
once. That rebuild is correct and has to stay. It also means the startup log
line reports 1.00 and is immediately superseded — the second line is the
truth.

## How it is verified

Run the windowed app in this Wayland session and read the log: no GLFW errors,
one `UI scale` line after the compositor's scale arrives, and no further
rebuild while the window is dragged and resized. `settings.json` keeps the
window x/y it had. The GUI suite runs under Xvfb (X11) and must stay green,
which is what covers the unchanged path.
