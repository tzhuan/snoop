# <img src="assets/icon_32x32.png" alt="" width="24" height="24"> Snoop

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

## Installation

```sh
npm install
```

## Usage

```sh
npm start
```

Or with command-line options:

```sh
npm start -- -4 -h -top -grid
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
| `Alt+Arrow` | Adjust focus offset (X11 and Windows) |

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
npm start -- -4 -h -top
```

## Building

```sh
npm run package    # Create unpacked build
npm run make       # Create platform installers
```

## Platform Notes

- **macOS**: Requires screen recording permission (System Settings > Privacy & Security > Screen Recording).
- **X11 / Windows**: Alt+Arrow focus offset adjustment is available to compensate for cursor-capture coordinate mismatch.
- **Wayland**: Screen capture may have limitations depending on the compositor.
- **Active window coordinates**: Supported on macOS and Windows only (requires `active-win` native module).

## Known Issues

- **Cursor always rendered on X11 and Windows**: The screen capture includes the mouse cursor on X11 and Windows due to OS-level cursor compositing (`cursor: 'never'` is not effective). Use `Alt+Arrow` keys to adjust the focus offset as a workaround.
- **Active window coordinates unavailable on Unix-like systems**: The active window coordinate mode (`W`) relies on the `active-win` native module, which only supports macOS and Windows. On X11/Wayland this feature is disabled.
- **`d` key (show capture region) not implemented**: The original Snoop can temporarily show the captured screen region. This requires a transparent overlay window and is not implemented.
- **`#` key (grid toggle) not implemented**: The `#` shortcut is keyboard-layout dependent. Use `F3` instead.

## Credits

Based on the original [Snoop for Windows](https://www.csie.ntu.edu.tw/~cyy/projects/snoop_win/index.html) by [Yung-Yu Chuang](https://www.csie.ntu.edu.tw/~cyy).

## License

BSD 3-Clause
