import { app, BrowserWindow, clipboard, desktopCapturer, dialog, ipcMain, Menu, nativeImage, screen, shell, systemPreferences } from 'electron'
import path from 'node:path'
import { fileURLToPath } from 'node:url'
import { uIOhook, UiohookKey } from 'uiohook-napi'
import { getActiveWindow as nativeGetActiveWindow, setCursorPosition } from '@snoop/geometry'
import pkg from '../package.json' with { type: 'json' }
import { DEFAULT_CONFIG } from './config-defaults.js'

const __filename = fileURLToPath(import.meta.url)
const __dirname = path.dirname(__filename)

const IS_LINUX = process.platform === 'linux'
const IS_DARWIN = process.platform === 'darwin'
const IS_WIN32 = process.platform === 'win32'
const IS_X11 = process.env.XDG_SESSION_TYPE === 'x11'
const IS_WAYLAND = process.env.XDG_SESSION_TYPE === 'wayland'
// Active window tracking: macOS, Windows, and Linux X11 (via native addon)
const SUPPORTS_ACTIVE_WINDOW = IS_DARWIN || IS_WIN32 || IS_X11
const CAN_WARP_CURSOR = !IS_WAYLAND && typeof setCursorPosition === 'function'

// Multi-instance needed for stream-based backends (each captures one monitor).
// Evaluated as a function since captureDriver can change at runtime.
function needsMultiInstance() {
  return IS_WAYLAND || IS_DARWIN || (IS_WIN32 && CONFIG.captureDriver === 'dxgi')
}

let MAIN_WINDOW
let cursorPos = { x: 0, y: 0 } // Logical cursor position (mouse or arrow mode)

// Display state for multi-monitor support
let displays = []              // Electron Display objects
let displayIdMap = new Map()   // Electron displayId → platform display ID string
let activeDisplayIds = new Set() // Currently-capturing display IDs
let nativeDisplays = []        // Native displays from capture addon (connector, x, y, w, h)

const CONFIG = structuredClone(DEFAULT_CONFIG)

const ZOOM_FACTORS = [1, 2, 3, 4, 6, 8, 10, 16, 32]

const FILTERS = ['none', 'gradient']

function sendToRenderer(channel, ...args) {
  if (MAIN_WINDOW && !MAIN_WINDOW.isDestroyed()) {
    MAIN_WINDOW.webContents.send(channel, ...args)
  }
}

function sendConfigToRenderer() {
  sendToRenderer('config-change', CONFIG)
}

function updateTitle() {
  if (MAIN_WINDOW && !MAIN_WINDOW.isDestroyed()) {
    const mode = CONFIG.inputMode === 'arrow' ? 'A' : 'M'
    MAIN_WINDOW.setTitle(`Snoop ×${CONFIG.zoomLevel} [${mode}] 32-bit`)
  }
}

function setZoom(level) {
  CONFIG.zoomLevel = level
  updateTitle()
  sendConfigToRenderer()
}

function zoomIn() {
  const next = ZOOM_FACTORS.find(f => f > CONFIG.zoomLevel)
  if (next !== undefined) setZoom(next)
}

function zoomOut() {
  for (let i = ZOOM_FACTORS.length - 1; i >= 0; i--) {
    if (ZOOM_FACTORS[i] < CONFIG.zoomLevel) { setZoom(ZOOM_FACTORS[i]); return }
  }
}

function cycleFilter() {
  const idx = FILTERS.indexOf(CONFIG.currentFilter)
  CONFIG.currentFilter = FILTERS[(idx + 1) % FILTERS.length]
  sendConfigToRenderer()
  buildAppMenu()
}

function resetToDefaults() {
  const defaults = structuredClone(DEFAULT_CONFIG)
  Object.assign(CONFIG, defaults)
  buildAppMenu()
  updateTitle()
  sendConfigToRenderer()
  if (MAIN_WINDOW && !MAIN_WINDOW.isDestroyed()) {
    MAIN_WINDOW.setAlwaysOnTop(CONFIG.alwaysOnTop)
  }
}

function switchCaptureDriver(driver) {
  CONFIG.captureDriver = driver
  // Notify preload to restart capture with new driver
  sendToRenderer('capture-driver-change', driver)
  sendConfigToRenderer()
  buildAppMenu()
}

function adjustDisplayOption(key, delta, min, max) {
  let val = CONFIG.displayOptions[key] + delta
  if (val < min) val = min
  if (val > max) val = max
  CONFIG.displayOptions[key] = key === 'gamma' ? Math.round(val * 100) / 100 : val
  sendConfigToRenderer()
}

const createWindow = () => {
  MAIN_WINDOW = new BrowserWindow({
    width: 800,
    height: 600,
    minWidth: 500,
    minHeight: 500,
    title: `Snoop ×${CONFIG.zoomLevel} [${CONFIG.inputMode === 'arrow' ? 'A' : 'M'}] 32-bit`,
    icon: path.join(__dirname, '..', 'assets', 'icon_256x256.png'),
    webPreferences: {
      preload: path.join(__dirname, 'preload.js'),
      contextIsolation: false,
      sandbox: false,
      backgroundThrottling: false,
    },
  })

  MAIN_WINDOW.loadFile(path.join(__dirname, 'index.html'))

  if (!IS_LINUX) {
    // Allow getDisplayMedia (macOS/Windows)
    MAIN_WINDOW.webContents.session.setDisplayMediaRequestHandler((_request, callback) => {
      desktopCapturer.getSources({ types: ['screen'] }).then((sources) => {
        if (sources.length > 0) {
          callback({ video: sources[0] })
        } else {
          callback({})
        }
      })
    })
  }

  // Prevent title from being overwritten by <title> tag
  MAIN_WINDOW.on('page-title-updated', (e) => e.preventDefault())

  MAIN_WINDOW.on('close', () => {
    if (CONFIG.autoSaveConfig) {
      saveConfig()
    }
  })

  MAIN_WINDOW.on('minimize', () => {
    sendToRenderer('capture-suspend')
  })

  MAIN_WINDOW.on('restore', () => {
    sendToRenderer('capture-resume')
  })

  MAIN_WINDOW.on('closed', () => {
    MAIN_WINDOW = null
  })
}

function showRefreshRateDialog() {
  const dlg = new BrowserWindow({
    width: 300,
    height: 150,
    parent: MAIN_WINDOW,
    modal: true,
    resizable: false,
    minimizable: false,
    maximizable: false,
    autoHideMenuBar: true,
    webPreferences: {
      preload: path.join(__dirname, 'preload-dialog.js'),
    },
  })

  dlg.loadURL(`data:text/html;charset=utf-8,${encodeURIComponent(`<!DOCTYPE html>
<html><head><style>
  body { font-family: system-ui, sans-serif; font-size: 13px; padding: 16px; margin: 0; background: #f0f0f0; }
  label { display: flex; align-items: center; gap: 6px; margin-bottom: 12px; }
  .row { display: flex; align-items: center; gap: 6px; margin-bottom: 16px; }
  input[type=number] { width: 80px; padding: 2px 4px; }
  .buttons { display: flex; justify-content: flex-end; gap: 8px; }
  button { padding: 4px 16px; }
</style></head><body>
  <label><input type="checkbox" id="enabled" ${CONFIG.refreshEnabled ? 'checked' : ''}> Enable refresh timer</label>
  <div class="row">interval <input type="number" id="interval" value="${CONFIG.refreshInterval}" min="1"> (1/100 sec)</div>
  <div class="buttons">
    <button id="ok">OK</button>
    <button id="cancel">Cancel</button>
  </div>
  <script>
    document.getElementById('ok').addEventListener('click', () => {
      const enabled = document.getElementById('enabled').checked
      const interval = parseInt(document.getElementById('interval').value) || 1
      window.dialogAPI.submit({ enabled, interval })
    })
    document.getElementById('cancel').addEventListener('click', () => {
      window.dialogAPI.cancel()
    })
  </script>
</body></html>`)}`)

}

function showDisplayOptionsDialog() {
  const opts = CONFIG.displayOptions
  const dlg = new BrowserWindow({
    width: 480,
    height: 380,
    parent: MAIN_WINDOW,
    modal: true,
    resizable: false,
    minimizable: false,
    maximizable: false,
    autoHideMenuBar: true,
    webPreferences: {
      preload: path.join(__dirname, 'preload-dialog.js'),
    },
  })

  dlg.loadURL(`data:text/html;charset=utf-8,${encodeURIComponent(`<!DOCTYPE html>
<html><head><style>
  body { font-family: system-ui, sans-serif; font-size: 13px; padding: 16px; margin: 0; background: #f0f0f0; }
  .row { display: flex; align-items: center; gap: 12px; margin-bottom: 10px; }
  .group { border: 1px solid #999; padding: 8px; margin-bottom: 10px; }
  .group-title { font-weight: bold; margin-bottom: 6px; }
  .slider-row { display: flex; align-items: center; gap: 8px; margin-bottom: 8px; }
  .slider-row label { width: 80px; text-align: right; }
  .slider-row input[type=range] { flex: 1; }
  .slider-row input[type=number] { width: 60px; }
  input[type=number] { width: 60px; padding: 2px 4px; }
  .buttons { display: flex; justify-content: flex-end; gap: 8px; margin-top: 12px; }
  button { padding: 4px 16px; }
</style></head><body>
  <div class="row">
    <label><input type="checkbox" id="red" ${opts.red ? 'checked' : ''}> Red</label>
    <label><input type="checkbox" id="green" ${opts.green ? 'checked' : ''}> Green</label>
    <label><input type="checkbox" id="blue" ${opts.blue ? 'checked' : ''}> Blue</label>
  </div>
  <div class="row">
    <div class="group">
      <div class="group-title">Auto Normalize</div>
      <label><input type="checkbox" id="anHist" ${opts.autoNormHistogram ? 'checked' : ''}> Histogram</label><br>
      <label><input type="checkbox" id="anGrad" ${opts.autoNormGradient ? 'checked' : ''}> Gradient</label>
    </div>
    <div class="group">
      <div class="group-title">Reference</div>
      <div class="row">Histogram <input type="number" id="refHist" value="${opts.refHistogram}" min="1"></div>
      <div class="row">Gradient <input type="number" id="refGrad" value="${opts.refGradient}" min="1"></div>
    </div>
  </div>
  <div class="slider-row">
    <label>Brightness</label>
    <input type="range" id="brightnessR" min="-100" max="100" step="1" value="${opts.brightness}">
    <input type="number" id="brightnessN" value="${opts.brightness}">
  </div>
  <div class="slider-row">
    <label>Contrast</label>
    <input type="range" id="contrastR" min="-100" max="100" step="1" value="${opts.contrast}">
    <input type="number" id="contrastN" value="${opts.contrast}">
  </div>
  <div class="slider-row">
    <label>Gamma</label>
    <input type="range" id="gammaR" min="0" max="200" value="${Math.round(opts.gamma * 100)}">
    <input type="number" id="gammaN" value="${opts.gamma}" step="0.01">
  </div>
  <div class="buttons">
    <button id="reset">Reset</button>
    <button id="ok">OK</button>
    <button id="update">Update</button>
    <button id="cancel">Cancel</button>
  </div>
  <script>
    // Sync sliders and number inputs
    function syncPair(sliderId, numberId, factor) {
      const s = document.getElementById(sliderId)
      const n = document.getElementById(numberId)
      s.addEventListener('input', () => { n.value = factor ? (s.value / factor).toFixed(1) : s.value })
      n.addEventListener('input', () => { s.value = factor ? Math.round(n.value * factor) : n.value })
    }
    syncPair('brightnessR', 'brightnessN')
    syncPair('contrastR', 'contrastN')
    syncPair('gammaR', 'gammaN', 100)

    function gather() {
      return {
        red: document.getElementById('red').checked,
        green: document.getElementById('green').checked,
        blue: document.getElementById('blue').checked,
        autoNormHistogram: document.getElementById('anHist').checked,
        autoNormGradient: document.getElementById('anGrad').checked,
        refHistogram: parseInt(document.getElementById('refHist').value) || 4000,
        refGradient: parseInt(document.getElementById('refGrad').value) || 255,
        brightness: parseInt(document.getElementById('brightnessN').value) || 0,
        contrast: parseInt(document.getElementById('contrastN').value) || 0,
        gamma: parseFloat(document.getElementById('gammaN').value) || 1.0,
      }
    }

    document.getElementById('ok').addEventListener('click', () => {
      window.dialogAPI.submit({ type: 'display-options', data: gather(), close: true })
    })
    document.getElementById('update').addEventListener('click', () => {
      window.dialogAPI.submit({ type: 'display-options', data: gather(), close: false })
    })
    document.getElementById('cancel').addEventListener('click', () => {
      window.dialogAPI.cancel()
    })
    document.getElementById('reset').addEventListener('click', () => {
      document.getElementById('brightnessR').value = 0
      document.getElementById('brightnessN').value = 0
      document.getElementById('contrastR').value = 0
      document.getElementById('contrastN').value = 0
      document.getElementById('gammaR').value = 100
      document.getElementById('gammaN').value = 1.0
    })
  </script>
</body></html>`)}`)

}

function buildAppMenu() {
  const template = [
    {
      label: '&Edit',
      submenu: [
        {
          label: 'Copy',
          accelerator: 'CmdOrCtrl+C',
          click: () => sendToRenderer('copy-request'),
        },
        { type: 'separator' },
        {
          label: 'Update',
          accelerator: 'U',
          click: () => sendToRenderer('update'),
        },
        { type: 'separator' },
        {
          label: 'Save Config',
          accelerator: 'F',
          click: () => saveConfig(),
        },
        {
          label: 'Auto save config',
          type: 'checkbox',
          checked: CONFIG.autoSaveConfig,
          click: (menuItem) => {
            CONFIG.autoSaveConfig = menuItem.checked
          },
        },
        { type: 'separator' },
        {
          label: 'About',
          accelerator: 'F1',
          click: () => {
            dialog.showMessageBox(MAIN_WINDOW, {
              type: 'info',
              title: 'About Snoop',
              message: `Snoop v${app.getVersion()}`,
              detail: `${pkg.description}\n\n${pkg.homepage}`,
            })
          },
        },
        { type: 'separator' },
        {
          label: 'Quit',
          accelerator: 'Q',
          click: () => app.quit(),
        },
        // Hidden zoom accelerators
        ...Array.from({ length: 9 }, (_, i) => ({
          label: `Zoom ×${i + 1}`,
          accelerator: `${i + 1}`,
          visible: false,
          click: () => setZoom(i + 1),
        })),
        {
          label: 'Zoom ×10',
          accelerator: '0',
          visible: false,
          click: () => setZoom(10),
        },
        {
          label: 'Quit',
          accelerator: 'Escape',
          visible: false,
          click: () => app.quit(),
        },
      ],
    },
    {
      label: '&Options',
      submenu: [
        {
          label: 'Refresh rate...',
          click: () => showRefreshRateDialog(),
        },
        {
          label: 'Display Options...',
          click: () => showDisplayOptionsDialog(),
        },
        {
          label: 'Always On Top',
          type: 'checkbox',
          checked: CONFIG.alwaysOnTop,
          accelerator: 'F2',
          click: (menuItem) => {
            CONFIG.alwaysOnTop = menuItem.checked
            if (MAIN_WINDOW && !MAIN_WINDOW.isDestroyed()) {
              MAIN_WINDOW.setAlwaysOnTop(CONFIG.alwaysOnTop)
            }
          },
        },
        {
          label: 'Grid',
          type: 'checkbox',
          checked: CONFIG.showGrid,
          accelerator: 'F3',
          click: (menuItem) => {
            CONFIG.showGrid = menuItem.checked
            sendConfigToRenderer()
          },
        },
        {
          label: 'Ruler',
          type: 'checkbox',
          checked: CONFIG.showRuler,
          accelerator: 'R',
          click: (menuItem) => {
            CONFIG.showRuler = menuItem.checked
            sendConfigToRenderer()
          },
        },
        {
          label: 'Center Highlight',
          type: 'checkbox',
          checked: CONFIG.centerHighlight,
          accelerator: 'C',
          click: (menuItem) => {
            CONFIG.centerHighlight = menuItem.checked
            sendConfigToRenderer()
          },
        },
        { type: 'separator' },
        {
          label: 'Screen',
          type: 'radio',
          checked: CONFIG.coordMode === 'screen',
          accelerator: 'S',
          click: () => {
            CONFIG.coordMode = 'screen'
            sendConfigToRenderer()
            buildAppMenu()
          },
        },
        {
          label: 'Active Window',
          type: 'radio',
          checked: CONFIG.coordMode === 'window',
          enabled: SUPPORTS_ACTIVE_WINDOW,
          accelerator: 'W',
          click: () => {
            CONFIG.coordMode = 'window'
            sendConfigToRenderer()
            buildAppMenu()
          },
        },
        {
          label: 'Inverse',
          type: 'checkbox',
          checked: CONFIG.inverseY,
          accelerator: 'Y',
          click: (menuItem) => {
            CONFIG.inverseY = menuItem.checked
            sendConfigToRenderer()
          },
        },
        {
          label: 'Hex mode',
          type: 'checkbox',
          checked: CONFIG.hexMode,
          accelerator: 'X',
          click: (menuItem) => {
            CONFIG.hexMode = menuItem.checked
            sendConfigToRenderer()
          },
        },
        {
          label: 'Bound',
          type: 'checkbox',
          checked: CONFIG.boundMode,
          accelerator: 'B',
          click: (menuItem) => {
            CONFIG.boundMode = menuItem.checked
            sendConfigToRenderer()
          },
        },
        { type: 'separator' },
        {
          label: 'Mouse Mode',
          type: 'radio',
          checked: CONFIG.inputMode === 'mouse',
          accelerator: 'M',
          click: () => {
            CONFIG.inputMode = 'mouse'
            updateTitle()
            sendConfigToRenderer()
            buildAppMenu()
          },
        },
        {
          label: 'Arrow Mode',
          type: 'radio',
          checked: CONFIG.inputMode === 'arrow',
          accelerator: 'A',
          click: () => {
            CONFIG.inputMode = 'arrow'
            updateTitle()
            sendConfigToRenderer()
            buildAppMenu()
          },
        },
      ],
    },
    {
      label: '&Filter',
      submenu: [
        {
          label: 'None',
          type: 'radio',
          checked: CONFIG.currentFilter === 'none',
          click: () => {
            CONFIG.currentFilter = 'none'
            sendConfigToRenderer()
            buildAppMenu()
          },
        },
        {
          label: 'Gradient',
          type: 'radio',
          checked: CONFIG.currentFilter === 'gradient',
          click: () => {
            CONFIG.currentFilter = 'gradient'
            sendConfigToRenderer()
            buildAppMenu()
          },
        },
      ],
    },
    {
      label: '&Graph',
      submenu: [
        {
          label: 'Graph Off',
          type: 'radio',
          checked: CONFIG.currentGraph === 'off',
          accelerator: 'O',
          click: () => {
            CONFIG.currentGraph = 'off'
            sendConfigToRenderer()
            buildAppMenu()
          },
        },
        {
          label: 'Horizontal Step',
          type: 'radio',
          checked: CONFIG.currentGraph === 'hstep',
          accelerator: 'Shift+H',
          click: () => {
            CONFIG.currentGraph = 'hstep'
            sendConfigToRenderer()
            buildAppMenu()
          },
        },
        {
          label: 'Horizontal Linear',
          type: 'radio',
          checked: CONFIG.currentGraph === 'hlinear',
          accelerator: 'H',
          click: () => {
            CONFIG.currentGraph = 'hlinear'
            sendConfigToRenderer()
            buildAppMenu()
          },
        },
        {
          label: 'Vertical Step',
          type: 'radio',
          checked: CONFIG.currentGraph === 'vstep',
          accelerator: 'Shift+V',
          click: () => {
            CONFIG.currentGraph = 'vstep'
            sendConfigToRenderer()
            buildAppMenu()
          },
        },
        {
          label: 'Vertical Linear',
          type: 'radio',
          checked: CONFIG.currentGraph === 'vlinear',
          accelerator: 'V',
          click: () => {
            CONFIG.currentGraph = 'vlinear'
            sendConfigToRenderer()
            buildAppMenu()
          },
        },
        {
          label: 'Statistics',
          type: 'radio',
          checked: CONFIG.currentGraph === 'statistics',
          accelerator: 'F4',
          click: () => {
            CONFIG.currentGraph = 'statistics'
            sendConfigToRenderer()
            buildAppMenu()
          },
        },
        { type: 'separator' },
        {
          label: 'Highlight',
          type: 'checkbox',
          checked: CONFIG.graphHighlight,
          enabled: ['hstep', 'hlinear', 'vstep', 'vlinear'].includes(CONFIG.currentGraph),
          accelerator: 'L',
          click: (menuItem) => {
            CONFIG.graphHighlight = menuItem.checked
            sendConfigToRenderer()
          },
        },
        {
          label: 'Statistics Info',
          type: 'checkbox',
          checked: CONFIG.statisticsInfo,
          enabled: CONFIG.currentGraph === 'statistics',
          accelerator: 'I',
          click: (menuItem) => {
            CONFIG.statisticsInfo = menuItem.checked
            sendConfigToRenderer()
          },
        },
      ],
    },
    ...(IS_WIN32 ? [{
      label: '&Drivers',
      submenu: [
        {
          label: 'BitBlt (default)',
          type: 'radio',
          checked: CONFIG.captureDriver !== 'dxgi',
          click: () => switchCaptureDriver(null),
        },
        {
          label: 'DXGI (DirectX)',
          type: 'radio',
          checked: CONFIG.captureDriver === 'dxgi',
          click: () => switchCaptureDriver('dxgi'),
        },
      ],
    }] : []),
    ...(IS_WAYLAND ? [{
      label: '&Drivers',
      submenu: [
        {
          label: 'PipeWire (default)',
          type: 'radio',
          checked: CONFIG.captureDriver !== 'eicc',
          click: () => switchCaptureDriver(null),
        },
        {
          label: 'ext-image-copy-capture',
          type: 'radio',
          checked: CONFIG.captureDriver === 'eicc',
          click: () => switchCaptureDriver('eicc'),
        },
      ],
    }] : []),
  ]

  const menu = Menu.buildFromTemplate(template)
  Menu.setApplicationMenu(menu)
}


const arrowKeyMap = {
  [UiohookKey.ArrowUp]: 'up',
  [UiohookKey.ArrowDown]: 'down',
  [UiohookKey.ArrowLeft]: 'left',
  [UiohookKey.ArrowRight]: 'right',
}

let spaceHeld = false


// Provide desktop capturer sources to renderer
ipcMain.handle('get-sources', async () => {
  // All platforms now use native capture addon
  return [{ id: '__native__', name: 'Native Capture' }]
})

// Copy image data to clipboard
ipcMain.on('copy-image', (_event, dataURL) => {
  const image = nativeImage.createFromDataURL(dataURL)
  clipboard.writeImage(image)
})

// Dialog handlers
ipcMain.on('dialog-submit', (event, data) => {
  const win = BrowserWindow.fromWebContents(event.sender)

  if (data.type === 'display-options') {
    CONFIG.displayOptions = data.data
    sendConfigToRenderer()
    if (data.close && win) win.close()
    return
  }

  // Refresh rate dialog
  CONFIG.refreshEnabled = data.enabled
  CONFIG.refreshInterval = Math.max(1, data.interval)
  sendToRenderer('capture-rate', Math.round(100 / CONFIG.refreshInterval))
  sendConfigToRenderer()
  if (win) win.close()
})

ipcMain.on('dialog-cancel', (event) => {
  const win = BrowserWindow.fromWebContents(event.sender)
  if (win) win.close()
})

// Handle native display list from preload (for Wayland/macOS connector mapping)
ipcMain.on('native-displays-result', (_event, result) => {
  nativeDisplays = result || []
  if (nativeDisplays.length > 0) {
    buildDisplayIdMap()
  }
})

/** Request native display list from preload and rebuild displayIdMap */
function requestNativeDisplays() {
  if (!MAIN_WINDOW || MAIN_WINDOW.isDestroyed()) return
  sendToRenderer('list-native-displays')
}

/** Match Electron displays to native displays by bounds proximity */
function buildDisplayIdMap() {
  if (!needsMultiInstance()) return
  displayIdMap.clear()

  if (IS_DARWIN) {
    // macOS: Electron display.id IS the CGDirectDisplayID
    for (const d of displays) {
      displayIdMap.set(d.id, String(d.id))
    }
  } else if (IS_WIN32 && CONFIG.captureDriver === 'dxgi') {
    // Windows/DXGI: map to output index by sorted position
    const sorted = [...displays].sort((a, b) =>
      a.bounds.x !== b.bounds.x ? a.bounds.x - b.bounds.x : a.bounds.y - b.bounds.y
    )
    for (let i = 0; i < sorted.length; i++) {
      displayIdMap.set(sorted[i].id, String(i))
    }
  } else if (nativeDisplays.length > 0) {
    // Wayland (PipeWire/eicc): match Electron display bounds to native connector strings
    // by finding the native display with the closest position match
    for (const d of displays) {
      let bestMatch = null
      let bestDist = Infinity
      for (const nd of nativeDisplays) {
        const dx = d.bounds.x - nd.x
        const dy = d.bounds.y - nd.y
        const dist = dx * dx + dy * dy
        if (dist < bestDist) {
          bestDist = dist
          bestMatch = nd
        }
      }
      if (bestMatch) {
        displayIdMap.set(d.id, bestMatch.connector)
      }
    }
  } else {
    // Fallback: use Electron display id (won't match native connectors but won't crash)
    for (const d of displays) {
      displayIdMap.set(d.id, String(d.id))
    }
  }
}

let activeWindowTrackerId = null

function getActiveWindowPos() {
  if (!MAIN_WINDOW || MAIN_WINDOW.isDestroyed()) return
  try {
    const win = nativeGetActiveWindow()
    if (win) {
      sendToRenderer('active-window-pos', {
        x: win.x, y: win.y, width: win.width, height: win.height,
      })
    }
  } catch {
    // Silently ignore errors (e.g. no active window)
  }
}

function startActiveWindowTracking() {
  activeWindowTrackerId = setInterval(getActiveWindowPos, 200)
}

function stopActiveWindowTracking() {
  if (activeWindowTrackerId) {
    clearInterval(activeWindowTrackerId)
    activeWindowTrackerId = null
  }
}

function sendScreenInfo() {
  const primaryDisplay = screen.getPrimaryDisplay()
  const { width, height } = primaryDisplay.size
  displays = screen.getAllDisplays()
  sendToRenderer('screen-info', {
    width, height,
    displays: displays.map(d => ({
      id: d.id,
      bounds: d.bounds,
      scaleFactor: d.scaleFactor,
    })),
  })
}

/** Return displays whose bounds intersect the given viewport rect */
function getOverlappingDisplays(vx, vy, vw, vh) {
  return displays.filter(d => {
    const b = d.bounds
    return vx < b.x + b.width && vx + vw > b.x &&
           vy < b.y + b.height && vy + vh > b.y
  })
}

/** Refresh display list and build displayIdMap for multi-instance backends */
function refreshDisplays() {
  displays = screen.getAllDisplays()
  if (!needsMultiInstance()) return

  // For Wayland, request fresh native display list (async — buildDisplayIdMap on result)
  if (IS_WAYLAND) {
    requestNativeDisplays()
  }
  buildDisplayIdMap()
}

function updateCaptureRegion() {
  if (!MAIN_WINDOW || MAIN_WINDOW.isDestroyed()) return
  const [winW, winH] = MAIN_WINDOW.getSize()
  const halfW = Math.ceil(winW / CONFIG.zoomLevel / 2) + 1
  const halfH = Math.ceil(winH / CONFIG.zoomLevel / 2) + 1
  const vx = cursorPos.x - halfW
  const vy = cursorPos.y - halfH
  const vw = halfW * 2
  const vh = halfH * 2

  // Always send the global viewport rect (used for coordinate mapping)
  sendToRenderer('capture-region', { x: vx, y: vy, w: vw, h: vh })

  if (needsMultiInstance()) {
    // Compute which displays overlap the viewport
    const overlapping = getOverlappingDisplays(vx, vy, vw, vh)
    const newIds = new Set(overlapping.map(d => displayIdMap.get(d.id)).filter(Boolean))

    // Only send activate-displays if the set changed or regions moved
    const displayList = overlapping.map(d => {
      const platformId = displayIdMap.get(d.id)
      if (!platformId) return null
      const b = d.bounds
      // Compute region local to this display's coordinate space
      const localX = vx - b.x
      const localY = vy - b.y
      return {
        displayId: platformId,
        bounds: { x: b.x, y: b.y, width: b.width, height: b.height },
        region: { x: localX, y: localY, w: vw, h: vh },
      }
    }).filter(Boolean)

    sendToRenderer('activate-displays', displayList)
    activeDisplayIds = newIds
  }
}

function startMouseTracking() {
  uIOhook.on('mousemove', (e) => {
    if (MAIN_WINDOW && !MAIN_WINDOW.isDestroyed()) {
      MAIN_WINDOW.webContents.send('cursor-position', { x: e.x, y: e.y })

      if (CONFIG.inputMode === 'mouse') {
        cursorPos.x = e.x
        cursorPos.y = e.y
        updateCaptureRegion()
      }
    }
  })

  uIOhook.on('keydown', (e) => {
    // Track space key state for Space+Arrow resize
    if (e.keycode === UiohookKey.Space) { spaceHeld = true; return }

    // Arrow keys
    const dir = arrowKeyMap[e.keycode]
    if (dir) {
      if (spaceHeld) {
        // Space+Arrow: resize window
        if (!MAIN_WINDOW || MAIN_WINDOW.isDestroyed()) return
        const [w, h] = MAIN_WINDOW.getSize()
        const step = (e.shiftKey ? CONFIG.shiftDelta : 1) * CONFIG.zoomLevel
        if (dir === 'up') MAIN_WINDOW.setSize(w, Math.max(200, h - step))
        else if (dir === 'down') MAIN_WINDOW.setSize(w, h + step)
        else if (dir === 'left') MAIN_WINDOW.setSize(Math.max(200, w - step), h)
        else if (dir === 'right') MAIN_WINDOW.setSize(w + step, h)
      } else if (e.ctrlKey) {
        // Ctrl+Arrow: peg to screen boundary (arrow mode only)
        if (CONFIG.inputMode === 'arrow') {
          sendToRenderer('arrow-move', { dir, peg: true })
          // Mirror peg in main for capture region — use nearest display, not primary
          const disp = screen.getDisplayNearestPoint(cursorPos)
          const { x: dx, y: dy, width: sw, height: sh } = disp.bounds
          const [winW, winH] = MAIN_WINDOW ? MAIN_WINDOW.getSize() : [800, 600]
          const srcW = winW / CONFIG.zoomLevel / 2
          const srcH = winH / CONFIG.zoomLevel / 2
          if (dir === 'up') cursorPos.y = dy + Math.floor(srcH)
          else if (dir === 'down') cursorPos.y = dy + sh - Math.floor(srcH)
          else if (dir === 'left') cursorPos.x = dx + Math.floor(srcW)
          else if (dir === 'right') cursorPos.x = dx + sw - Math.floor(srcW)
          if (CAN_WARP_CURSOR) setCursorPosition(cursorPos.x, cursorPos.y)
          updateCaptureRegion()
        }
      } else if (e.shiftKey) {
        // Shift+Arrow: fast move (arrow mode only)
        if (CONFIG.inputMode === 'arrow') {
          sendToRenderer('arrow-move', { dir, delta: CONFIG.shiftDelta })
          const d = CONFIG.shiftDelta
          if (dir === 'up') cursorPos.y -= d
          else if (dir === 'down') cursorPos.y += d
          else if (dir === 'left') cursorPos.x -= d
          else if (dir === 'right') cursorPos.x += d
          if (CAN_WARP_CURSOR) setCursorPosition(cursorPos.x, cursorPos.y)
          updateCaptureRegion()
        }
      } else if (CONFIG.inputMode === 'arrow') {
        // Arrow: move 1 pixel
        sendToRenderer('arrow-move', { dir, delta: 1 })
        if (dir === 'up') cursorPos.y--
        else if (dir === 'down') cursorPos.y++
        else if (dir === 'left') cursorPos.x--
        else if (dir === 'right') cursorPos.x++
        if (CAN_WARP_CURSOR) setCursorPosition(cursorPos.x, cursorPos.y)
        updateCaptureRegion()
      }
      return
    }

    // PageUp / PageDown
    if (e.keycode === UiohookKey.PageUp || e.keycode === UiohookKey.PageDown) {
      const up = e.keycode === UiohookKey.PageUp
      if (e.shiftKey && e.ctrlKey) {
        // Ctrl+Shift+PageUp/Down: gamma
        adjustDisplayOption('gamma', up ? 0.02 : -0.02, 0.02, 2.0)
      } else if (e.shiftKey) {
        // Shift+PageUp/Down: brightness
        adjustDisplayOption('brightness', up ? 2 : -2, -100, 100)
      } else if (e.ctrlKey) {
        // Ctrl+PageUp/Down: contrast
        adjustDisplayOption('contrast', up ? 2 : -2, -100, 100)
      } else {
        // PageUp/Down: histogram scale
        sendToRenderer('histogram-scale', up ? 'up' : 'down')
      }
      return
    }

    // Home: reset histogram scale
    if (e.keycode === UiohookKey.Home) {
      sendToRenderer('histogram-scale', 'reset')
      return
    }

    // Tab: cycle filter
    if (e.keycode === UiohookKey.Tab) {
      cycleFilter()
      return
    }

    // +/=/- zoom
    if (e.keycode === UiohookKey.Equal || e.keycode === UiohookKey.NumpadAdd) {
      zoomIn()
      return
    }
    if (e.keycode === UiohookKey.Minus || e.keycode === UiohookKey.NumpadSubtract) {
      zoomOut()
      return
    }

    // Backspace: reset to defaults
    if (e.keycode === UiohookKey.Backspace) {
      resetToDefaults()
      return
    }

    // Delete: clear all saved configs
    if (e.keycode === UiohookKey.Delete) {
      sendToRenderer('clear-all-configs')
      return
    }

    // Ctrl+F1-F9: load config slot, Shift+F1-F9: save config slot
    const fKeyMap = {
      [UiohookKey.F1]: 1, [UiohookKey.F2]: 2, [UiohookKey.F3]: 3,
      [UiohookKey.F4]: 4, [UiohookKey.F5]: 5, [UiohookKey.F6]: 6,
      [UiohookKey.F7]: 7, [UiohookKey.F8]: 8, [UiohookKey.F9]: 9,
    }
    const fNum = fKeyMap[e.keycode]
    if (fNum !== undefined) {
      if (e.ctrlKey && !e.shiftKey) {
        sendToRenderer('load-config-slot', fNum)
        return
      }
      if (e.shiftKey && !e.ctrlKey) {
        if (MAIN_WINDOW && !MAIN_WINDOW.isDestroyed()) {
          const [w, h] = MAIN_WINDOW.getSize()
          const [x, y] = MAIN_WINDOW.getPosition()
          CONFIG.windowWidth = w
          CONFIG.windowHeight = h
          CONFIG.windowX = x
          CONFIG.windowY = y
        }
        sendToRenderer('save-config-slot', { slot: fNum, config: CONFIG })
        return
      }
    }
  })

  uIOhook.on('keyup', (e) => {
    if (e.keycode === UiohookKey.Space) spaceHeld = false
  })

  uIOhook.start()
}

// Config persistence
function saveConfig() {
  if (MAIN_WINDOW && !MAIN_WINDOW.isDestroyed()) {
    const [w, h] = MAIN_WINDOW.getSize()
    const [x, y] = MAIN_WINDOW.getPosition()
    CONFIG.windowWidth = w
    CONFIG.windowHeight = h
    CONFIG.windowX = x
    CONFIG.windowY = y
  }
  sendToRenderer('save-config', CONFIG)
}

function applyLoadedConfig(savedConfig) {
  if (!savedConfig) return
  const savedDisplayOptions = savedConfig.displayOptions
  Object.assign(CONFIG, savedConfig)
  if (savedDisplayOptions) {
    CONFIG.displayOptions = { ...DEFAULT_CONFIG.displayOptions, ...savedDisplayOptions }
  }
  buildAppMenu()
  updateTitle()
  sendConfigToRenderer()
  if (MAIN_WINDOW && !MAIN_WINDOW.isDestroyed()) {
    MAIN_WINDOW.setAlwaysOnTop(CONFIG.alwaysOnTop)
    if (CONFIG.windowWidth && CONFIG.windowHeight) {
      MAIN_WINDOW.setSize(CONFIG.windowWidth, CONFIG.windowHeight)
    }
    if (CONFIG.windowX !== undefined && CONFIG.windowY !== undefined) {
      MAIN_WINDOW.setPosition(CONFIG.windowX, CONFIG.windowY)
    }
  }
}

ipcMain.on('config-loaded', (_event, savedConfig) => {
  applyLoadedConfig(savedConfig)
})

ipcMain.on('config-slot-loaded', (_event, savedConfig) => {
  applyLoadedConfig(savedConfig)
})

async function checkScreenCapturePermission() {
  if (process.platform !== 'darwin') return true

  const status = systemPreferences.getMediaAccessStatus('screen')
  if (status === 'granted') return true

  // On macOS, we cannot programmatically request screen recording permission.
  // Show a dialog guiding the user to System Preferences.
  const { response } = await dialog.showMessageBox({
    type: 'warning',
    title: 'Screen Recording Permission Required',
    message: 'Snoop needs screen recording permission to capture screen content.',
    detail: 'Please grant screen recording access in System Settings > Privacy & Security > Screen Recording, then restart Snoop.',
    buttons: ['Open System Settings', 'Quit', 'Continue Anyway'],
    defaultId: 0,
    cancelId: 1,
  })

  if (response === 0) {
    shell.openExternal('x-apple.systempreferences:com.apple.preference.security?Privacy_ScreenCapture')
    app.quit()
    return false
  } else if (response === 1) {
    app.quit()
    return false
  }

  return true
}

// Parse command-line arguments (skip electron/app paths)
function parseCommandLine() {
  const args = process.argv.slice(app.isPackaged ? 1 : 2)
  for (let i = 0; i < args.length; i++) {
    const arg = args[i]
    switch (arg) {
      case '-top': CONFIG.alwaysOnTop = true; break
      case '-notop': CONFIG.alwaysOnTop = false; break
      case '-grid': CONFIG.showGrid = true; break
      case '-nogrid': CONFIG.showGrid = false; break
      case '-ruler': CONFIG.showRuler = true; break
      case '-noruler': CONFIG.showRuler = false; break
      case '-highlight': CONFIG.graphHighlight = true; break
      case '-nohighlight': CONFIG.graphHighlight = false; break
      case '-centermark': CONFIG.centerHighlight = true; break
      case '-nocentermark': CONFIG.centerHighlight = false; break
      case '-inversey': CONFIG.inverseY = true; break
      case '-hex': CONFIG.hexMode = true; break
      case '-bound': CONFIG.boundMode = true; break
      case '-shift': {
        const val = parseInt(args[++i])
        if (!isNaN(val)) CONFIG.shiftDelta = val
        break
      }
      case '-f': {
        const val = parseInt(args[++i])
        if (val >= 1 && val <= 10) CONFIG._loadSlot = val
        break
      }
      case '-coord': {
        const x = parseInt(args[++i])
        const y = parseInt(args[++i])
        if (!isNaN(x) && !isNaN(y)) CONFIG._windowPos = { x, y }
        break
      }
      case '-size': {
        const w = parseInt(args[++i])
        const h = parseInt(args[++i])
        if (!isNaN(w) && !isNaN(h)) CONFIG._windowSize = { w, h }
        break
      }
      default:
        // Single-char hotkey shortcuts (e.g. -4 sets zoom to 4, -h sets hlinear)
        if (arg.length === 2 && arg[0] === '-') {
          const ch = arg[1]
          if (ch >= '1' && ch <= '9') setZoom(parseInt(ch))
          else if (ch === '0') setZoom(10)
          else switch (ch) {
            case 'o': CONFIG.currentGraph = 'off'; break
            case 'H': CONFIG.currentGraph = 'hstep'; break
            case 'h': CONFIG.currentGraph = 'hlinear'; break
            case 'V': CONFIG.currentGraph = 'vstep'; break
            case 'v': CONFIG.currentGraph = 'vlinear'; break
            case 'l': CONFIG.graphHighlight = !CONFIG.graphHighlight; break
            case 'c': CONFIG.centerHighlight = !CONFIG.centerHighlight; break
            case 'x': CONFIG.hexMode = !CONFIG.hexMode; break
            case 'y': CONFIG.inverseY = !CONFIG.inverseY; break
            case 's': CONFIG.coordMode = 'screen'; break
            case 'w': CONFIG.coordMode = 'window'; break
            case 'a': CONFIG.inputMode = 'arrow'; break
            case 'm': CONFIG.inputMode = 'mouse'; break
            case 'b': CONFIG.boundMode = !CONFIG.boundMode; break
            case 'r': CONFIG.showRuler = !CONFIG.showRuler; break
            case 'i': CONFIG.statisticsInfo = !CONFIG.statisticsInfo; break
          }
        }
    }
  }
}

parseCommandLine()

app.whenReady().then(async () => {
  const permitted = await checkScreenCapturePermission()
  if (!permitted) return

  createWindow()
  buildAppMenu()
  startMouseTracking()
  if (SUPPORTS_ACTIVE_WINDOW) {
    startActiveWindowTracking()
  }

  // Initialize display tracking
  refreshDisplays()

  // Dynamic hotplug: react to display changes
  screen.on('display-added', () => {
    refreshDisplays()
    sendScreenInfo()
    if (IS_X11) {
      // X11 XShm buffer is fixed-size — needs restart to resize
      sendToRenderer('capture-restart')
    }
    // Multi-instance: next updateCaptureRegion() will pick up new display
    updateCaptureRegion()
  })

  screen.on('display-removed', () => {
    refreshDisplays()
    sendScreenInfo()
    if (IS_X11) {
      sendToRenderer('capture-restart')
    }
    updateCaptureRegion()
  })

  screen.on('display-metrics-changed', () => {
    refreshDisplays()
    sendScreenInfo()
    if (IS_X11) {
      sendToRenderer('capture-restart')
    }
    updateCaptureRegion()
  })

  // Send initial capture config to the renderer (preload manages the addon)
  {
    const primaryDisplay = screen.getPrimaryDisplay()
    const { x: ox, y: oy } = primaryDisplay.bounds
    const { width: screenW, height: screenH } = primaryDisplay.size
    cursorPos.x = ox + Math.floor(screenW / 2)
    cursorPos.y = oy + Math.floor(screenH / 2)
    MAIN_WINDOW.webContents.once('did-finish-load', () => {
      if (CONFIG.captureDriver) {
        sendToRenderer('capture-driver-change', CONFIG.captureDriver)
      }
      sendToRenderer('capture-rate', Math.round(100 / CONFIG.refreshInterval))
      updateCaptureRegion()
    })
  }

  // Apply CLI window position/size
  if (CONFIG._windowPos) {
    MAIN_WINDOW.setPosition(CONFIG._windowPos.x, CONFIG._windowPos.y)
    delete CONFIG._windowPos
  }
  if (CONFIG._windowSize) {
    MAIN_WINDOW.setSize(CONFIG._windowSize.w, CONFIG._windowSize.h)
    delete CONFIG._windowSize
  }
  MAIN_WINDOW.setAlwaysOnTop(CONFIG.alwaysOnTop)

  MAIN_WINDOW.webContents.on('did-finish-load', () => {
    sendScreenInfo()
    if (CONFIG._loadSlot) {
      // Request specific config slot from renderer's localStorage
      sendToRenderer('request-config-slot', CONFIG._loadSlot)
      delete CONFIG._loadSlot
    } else {
      sendToRenderer('request-config')
    }
  })

  app.on('activate', () => {
    if (BrowserWindow.getAllWindows().length === 0) {
      createWindow()
    }
  })
})

app.on('window-all-closed', () => {
  if (process.platform !== 'darwin') {
    app.quit()
  }
})

app.on('before-quit', () => {
  stopActiveWindowTracking()
  try { uIOhook.stop() } catch {}
  // Force exit after cleanup in case something still blocks
  setTimeout(() => process.exit(0), 500)
})
