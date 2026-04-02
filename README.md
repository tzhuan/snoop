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
- **Mouse and arrow key** input modes
- **Track mode** — drag inside the magnifier to move the zoom center
- **Configuration persistence** with 10 save slots
- **Always on top** window mode
- **Copy** magnified view to clipboard
- **Cursor-free capture** on Linux (PipeWire) and macOS (ScreenCaptureKit)

## Project Structure

```
snoop/
├── packages/
│   ├── geometry/         # @snoop/geometry — native desktop geometry (active window, cursor, displays)
│   └── capture/          # @snoop/capture — native screen capture (PipeWire/XShm/DXGI/ScreenCaptureKit)
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
npm run make -w app       # Create platform installers
```

## Screen Capture Backends

| Platform | Backend | Cursor-free | Notes |
|----------|---------|-------------|-------|
| Linux (GNOME) | PipeWire via Mutter ScreenCast | Yes | Works on both X11 and Wayland |
| Linux (KDE, etc.) | PipeWire via xdg-desktop-portal | Yes | May show a permission dialog |
| Linux (non-compositing) | X11 XShm | No | Fallback for basic X11 WMs |
| macOS | ScreenCaptureKit | Yes | Requires macOS 12.3+ |
| Windows | DXGI Desktop Duplication | Yes | |

The capture backend is selected automatically at runtime. Set `SNOOP_CAPTURE=x11` or `SNOOP_CAPTURE=pipewire` to force a specific Linux backend.

## Platform Notes

- **macOS**: Requires screen recording permission (System Settings > Privacy & Security > Screen Recording).
- **Linux**: Native capture addon requires PipeWire and D-Bus libraries at runtime. Falls back to XShm if unavailable.
- **Active window coordinates**: Supported on macOS, Windows, and Linux X11 (via native addon).

## Credits

Based on the original [Snoop for Windows](https://www.csie.ntu.edu.tw/~cyy/projects/snoop_win/index.html) by [Yung-Yu Chuang](https://www.csie.ntu.edu.tw/~cyy).

## License

BSD 3-Clause
