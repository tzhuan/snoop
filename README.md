# <img src="app/assets/icon_32x32.png" alt="" width="24" height="24"> Snoop

A cross-platform screen magnification and pixel inspection tool built with Electron. Originally a [Windows-only utility](https://www.csie.ntu.edu.tw/~cyy/projects/snoop_win/index.html), this re-implementation brings the same functionality to Windows, macOS, and Unix-like systems.

## Features

- **Screen magnification** with zoom levels 1-10, 16, and 32x
- **Pixel grid overlay** for precise pixel-level inspection
- **Ruler** with tick marks along viewport edges
- **Center pixel highlight** to mark the exact magnification center
- **RGB intensity graphs** — horizontal/vertical, step/linear modes
- **Statistics view** — histogram with min/max/average/median per channel
- **Gradient filter** — edge detection visualization
- **Display adjustments** — brightness, contrast, gamma, per-channel RGB toggle
- **Coordinate systems** — screen absolute or active window relative, with Y-axis inversion
- **Hex mode** for RGB values
- **Bound mode** to prevent viewing outside screen boundaries
- **Mouse and arrow key** input modes (mouse mode requires X11 or XWayland; Wayland without XWayland is arrow-mode only)
- **Track mode** — drag inside the magnifier to move the zoom center
- **Configuration persistence** with 10 save slots
- **Always on top** window mode
- **Copy** magnified view to clipboard
- **Cursor-free capture** on Linux (PipeWire) and macOS (ScreenCaptureKit)
- **Multi-monitor support** with seamless cross-boundary capture and dynamic hotplug
- **Multiple capture drivers** selectable via Drivers menu (platform-dependent)

## Project Structure

```
snoop/
├── packages/
│   ├── geometry/         # @snoop/geometry — native desktop geometry (active window, cursor, displays)
│   └── capture/          # @snoop/capture — native screen capture (PipeWire/XShm/DXGI/BitBlt/ScreenCaptureKit/eicc)
├── app/                  # Electron app
└── package.json          # npm workspace root
```

## Installation

```sh
npm install
```

### Build Dependencies (Linux)

Native addons require development headers:

```sh
sudo apt-get install cmake libx11-dev libxext-dev libpipewire-0.3-dev libdbus-1-dev
```

### Building Native Addons

```sh
npx cmake-js compile -d packages/geometry
npx cmake-js compile -d packages/capture
```

## Usage

```sh
npm start
```

Or with command-line options:

```sh
npm start -w app -- -- -4 -h -top -grid
```

## Keyboard Shortcuts

Keyboard shortcuts are active when the Snoop window is focused.

### Zoom

| Key | Action |
|-----|--------|
| `1`-`9` | Set zoom to 1x-9x |
| `0` | Set zoom to 10x |
| `+` / `=` | Zoom in (cycle through 1,2,3,4,6,8,10,16,32) |
| `-` | Zoom out |

### Display

| Key | Action |
|-----|--------|
| `F3` | Toggle grid |
| `R` | Toggle ruler |
| `C` | Toggle center highlight |
| `X` | Toggle hex mode |
| `Y` | Toggle inverse Y-axis |
| `B` | Toggle bound mode |
| `Tab` | Cycle filter (none / gradient) |

### Graph

| Key | Action |
|-----|--------|
| `O` | Graph off |
| `H` | Horizontal linear graph |
| `Shift+H` | Horizontal step graph |
| `V` | Vertical linear graph |
| `Shift+V` | Vertical step graph |
| `F4` | Statistics / histogram |
| `L` | Toggle graph highlight |
| `I` | Toggle statistics info text |

### Navigation

| Key | Action |
|-----|--------|
| `M` | Mouse mode |
| `A` | Arrow mode |
| Arrow keys | Move view (arrow mode) |
| `Shift+Arrow` | Fast move (configurable step, default 8px) |
| `Ctrl+Arrow` | Peg to screen boundary |
| `Space+Arrow` | Resize window |
| `Alt+Arrow` | Adjust focus offset (stream driver only) |

### Adjustments

| Key | Action |
|-----|--------|
| `Shift+PageUp/Down` | Brightness +/- |
| `Ctrl+PageUp/Down` | Contrast +/- |
| `Ctrl+Shift+PageUp/Down` | Gamma +/- |
| `PageUp/Down` | Histogram Y-axis scale (in statistics mode) |
| `Home` | Reset histogram scale |

### Configuration

| Key | Action |
|-----|--------|
| `F` | Save config |
| `Shift+F1`-`F9` | Save to config slot 1-9 |
| `Ctrl+F1`-`F9` | Load config slot 1-9 |
| `Backspace` | Reset to defaults |
| `Delete` | Clear all saved configs |

### General

| Key | Action |
|-----|--------|
| `Ctrl+C` | Copy magnified view to clipboard |
| `U` | Force refresh |
| `F1` | About |
| `F2` | Toggle always on top |
| `Q` / `Escape` | Quit |

## Command-Line Options

```
-top / -notop             Always on top
-grid / -nogrid           Grid overlay
-ruler / -noruler         Ruler display
-highlight / -nohighlight Graph highlight
-centermark / -nocentermark Center pixel highlight
-shift <delta>            Fast-move step size (default 8)
-f <n>                    Load config slot n (1-10) on startup
-coord <x> <y>            Window position
-size <w> <h>             Window size
-inversey                 Invert Y-axis
-hex                      Hex RGB display
-bound                    Bound mode
-<char>                   Execute single-char hotkey on startup
```

Example: start at 4x zoom with horizontal linear graph, always on top:

```sh
npm start -w app -- -- -4 -h -top
```

## Building

```sh
npm run dist -w app       # Create platform installers
```

## Screen Capture Drivers

Snoop supports multiple capture drivers per platform. On platforms with more than one driver, a **Drivers** menu appears in the menu bar to switch at runtime.

| Platform | Driver | Type | Multi-monitor | Notes |
|----------|--------|------|---------------|-------|
| Linux/GNOME (X11 or Wayland) | **PipeWire** (default) | Stream | Multi-instance (one stream per display) | Mutter ScreenCast D-Bus API |
| Linux/X11 (non-GNOME) | **XShm** | Polling | Native (root window spans all monitors) | Fallback when PipeWire/Mutter unavailable |
| Linux/X11 | Stream (desktopCapturer) | Stream | Via compositor | Electron's built-in video pipeline |
| Linux/Wayland | ext-image-copy-capture | Stream | Multi-instance | Standardized protocol with damage tracking (wlroots, KDE) |
| macOS | ScreenCaptureKit | Stream | Multi-instance | Uses sourceRect for viewport-only capture |
| Windows | **BitBlt** (default) | Polling | Native (virtual desktop spans all monitors) | Simple, no DirectX capture |
| Windows | DXGI | Stream | Multi-instance (one per output) | Captures DirectX exclusive fullscreen |

The default driver is selected automatically. Set `SNOOP_CAPTURE=x11|pipewire|eicc|bitblt|dxgi` to force a specific backend via environment variable.

## Multi-Monitor Support

Snoop supports multi-monitor setups on all platforms:

- **Polling drivers** (XShm, BitBlt): Capture any rect across all monitors in a single call. Multi-monitor is free with no additional overhead.
- **Stream drivers** (PipeWire, eicc, DXGI, ScreenCaptureKit): One capture instance per overlapping display. Instances start/stop automatically as the viewport moves across monitor boundaries. Frames are composited in the renderer.
- **Hotplug**: Monitors can be added/removed at runtime. X11 restarts the XShm buffer; stream backends update their instance set.
- **Arrow mode**: On X11, Windows, and macOS, arrow keys move the real system cursor (via native cursor warping), which naturally triggers multi-monitor capture switching. Wayland uses a virtual cursor.

## Platform Notes

- **macOS**: Requires screen recording permission (System Settings > Privacy & Security > Screen Recording).
- **Linux (GNOME)**: PipeWire capture with multi-monitor support. Requires PipeWire and D-Bus libraries at runtime.
- **Linux (Wayland)**: Mouse tracking works via XWayland (if `DISPLAY` is set). Without XWayland, only arrow-key mode is available. Wayland prohibits clients from warping the cursor, so arrow mode uses a virtual cursor.
- **Linux (X11 non-GNOME)**: Falls back to XShm if PipeWire/Mutter is unavailable. XShm captures the root window which spans all monitors.
- **Windows**: BitBlt (default) captures across all monitors. DXGI (opt-in via Drivers menu) captures DirectX game content but requires one stream per monitor.
- **Active window coordinates**: Supported on macOS, Windows, and Linux X11 (via native addon).
- **Cursor in capture**: The X11 Stream driver (desktopCapturer) composites the mouse cursor into the captured image. Use `Alt+Arrow` to adjust the focus offset as a workaround. All other drivers exclude the cursor.

## Credits

Based on the original [Snoop for Windows](https://www.csie.ntu.edu.tw/~cyy/projects/snoop_win/index.html) by [Yung-Yu Chuang](https://www.csie.ntu.edu.tw/~cyy).

## License

BSD 3-Clause
