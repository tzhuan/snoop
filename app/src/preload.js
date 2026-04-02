import { ipcRenderer } from 'electron'
import { createCapture } from '@snoop/capture'

// Platform detection
const IS_LINUX = process.platform === 'linux'
const IS_DARWIN = process.platform === 'darwin'
const IS_WIN32 = process.platform === 'win32'
const IS_WAYLAND = IS_LINUX && !!process.env.WAYLAND_DISPLAY

// Multi-instance is needed for stream-based backends where each capture
// targets one monitor. Polling backends handle multi-monitor natively.
const NEEDS_MULTI_INSTANCE = IS_WAYLAND || IS_DARWIN
// Note: Windows DXGI multi-instance added later (Phase 2)

// --- Single-instance path (X11 XShm, Windows BitBlt/DXGI) ---
// --- Multi-instance path (Wayland PipeWire, macOS SCK) ---

let nativeCapture = null       // single-instance only
let latestNativeFrame = null   // single-instance only

// Multi-instance state
const captureInstances = new Map()  // displayId → { capture, latestFrame, bounds }
let captureRate = 30

if (!NEEDS_MULTI_INSTANCE) {
  // Single-instance: create one capture upfront
  try {
    nativeCapture = createCapture()
    nativeCapture.onFrame((buffer, width, height) => {
      latestNativeFrame = { buffer, width, height }
    })
  } catch (err) {
    console.warn('Native capture not available:', err.message)
  }
}

function startDisplayCapture(displayId, bounds) {
  if (captureInstances.has(displayId)) return
  try {
    const capture = createCapture()
    const entry = { capture, latestFrame: null, bounds }
    capture.setDisplay(displayId)
    capture.onFrame((buffer, width, height) => {
      entry.latestFrame = { buffer, width, height }
    })
    capture.setRate(captureRate)
    capture.start()
    captureInstances.set(displayId, entry)
  } catch (err) {
    console.warn(`Failed to start capture for display ${displayId}:`, err.message)
  }
}

function stopDisplayCapture(displayId) {
  const entry = captureInstances.get(displayId)
  if (!entry) return
  entry.capture.stop()
  captureInstances.delete(displayId)
}

// Track capture region offset for renderer coordinate mapping
let captureRegionX = 0
let captureRegionY = 0

ipcRenderer.on('capture-region', (_e, { x, y, w, h }) => {
  captureRegionX = x
  captureRegionY = y
  if (NEEDS_MULTI_INSTANCE) {
    // Multi-instance: each instance gets local coords (region relative to its display)
    // The main process sends per-instance regions via activate-displays
    // Here we just store the global viewport for coordinate mapping
  } else {
    if (nativeCapture) nativeCapture.setRegion(x, y, w, h)
  }
})

ipcRenderer.on('capture-rate', (_e, fps) => {
  captureRate = fps
  if (NEEDS_MULTI_INSTANCE) {
    for (const entry of captureInstances.values()) {
      entry.capture.setRate(fps)
    }
  } else {
    if (nativeCapture) nativeCapture.setRate(fps)
  }
})

ipcRenderer.on('capture-suspend', () => {
  if (NEEDS_MULTI_INSTANCE) {
    for (const entry of captureInstances.values()) entry.capture.suspend()
  } else {
    if (nativeCapture) nativeCapture.suspend()
  }
})

ipcRenderer.on('capture-resume', () => {
  if (NEEDS_MULTI_INSTANCE) {
    for (const entry of captureInstances.values()) entry.capture.resume()
  } else {
    if (nativeCapture) nativeCapture.resume()
  }
})

// Multi-instance: activate/deactivate displays based on viewport overlap
ipcRenderer.on('activate-displays', (_e, displayList) => {
  if (!NEEDS_MULTI_INSTANCE) return

  // displayList = [{ displayId, bounds: { x, y, width, height }, region: { x, y, w, h } }]
  const newIds = new Set(displayList.map(d => d.displayId))

  // Stop instances no longer needed
  for (const id of captureInstances.keys()) {
    if (!newIds.has(id)) stopDisplayCapture(id)
  }

  // Start new instances and update regions
  for (const { displayId, bounds, region } of displayList) {
    if (!captureInstances.has(displayId)) {
      startDisplayCapture(displayId, bounds)
    } else {
      captureInstances.get(displayId).bounds = bounds
    }
    const entry = captureInstances.get(displayId)
    if (entry && region) {
      entry.capture.setRegion(region.x, region.y, region.w, region.h)
    }
  }
})

// For X11 XShm hotplug: restart the single capture instance
ipcRenderer.on('capture-restart', () => {
  if (NEEDS_MULTI_INSTANCE) {
    // Restart all instances
    for (const [id, entry] of captureInstances.entries()) {
      entry.capture.stop()
      entry.capture.onFrame((buffer, width, height) => {
        entry.latestFrame = { buffer, width, height }
      })
      entry.capture.start()
    }
  } else if (nativeCapture) {
    nativeCapture.stop()
    nativeCapture.onFrame((buffer, width, height) => {
      latestNativeFrame = { buffer, width, height }
    })
    nativeCapture.start()
  }
})

// With contextIsolation disabled, set globals directly on window
window.snoop = {
  getSources: () => ipcRenderer.invoke('get-sources'),

  // Native capture — single instance
  getNativeFrame: () => {
    const frame = latestNativeFrame
    latestNativeFrame = null
    return frame
  },
  getCaptureRegion: () => ({ x: captureRegionX, y: captureRegionY }),
  startNativeCapture: () => {
    if (NEEDS_MULTI_INSTANCE) return 0  // managed via activate-displays
    return nativeCapture ? nativeCapture.start() : -1
  },
  stopNativeCapture: () => {
    if (NEEDS_MULTI_INSTANCE) {
      for (const id of [...captureInstances.keys()]) stopDisplayCapture(id)
    } else if (nativeCapture) {
      nativeCapture.stop()
    }
  },

  // Multi-instance: return all active frames with their display bounds
  getNativeFrames: () => {
    if (!NEEDS_MULTI_INSTANCE) {
      // Single-instance fallback
      const frame = latestNativeFrame
      latestNativeFrame = null
      return frame ? [{ frame, bounds: null }] : []
    }
    const frames = []
    for (const entry of captureInstances.values()) {
      if (entry.latestFrame) {
        frames.push({ frame: entry.latestFrame, bounds: entry.bounds })
        entry.latestFrame = null
      }
    }
    return frames
  },

  needsMultiInstance: NEEDS_MULTI_INSTANCE,

  // Config
  onConfigChange: (callback) => ipcRenderer.on('config-change', (_e, config) => callback(config)),
  sendSavedConfig: (config) => ipcRenderer.send('config-loaded', config),
  onSaveConfig: (callback) => ipcRenderer.on('save-config', (_e, config) => callback(config)),
  onRequestConfig: (callback) => ipcRenderer.on('request-config', () => callback()),

  // Config slots
  onSaveConfigSlot: (callback) => ipcRenderer.on('save-config-slot', (_e, data) => callback(data)),
  onLoadConfigSlot: (callback) => ipcRenderer.on('load-config-slot', (_e, slot) => callback(slot)),
  onRequestConfigSlot: (callback) => ipcRenderer.on('request-config-slot', (_e, slot) => callback(slot)),
  sendSlotConfig: (config) => ipcRenderer.send('config-slot-loaded', config),
  onClearAllConfigs: (callback) => ipcRenderer.on('clear-all-configs', () => callback()),

  // Runtime events
  onCursorPosition: (callback) => ipcRenderer.on('cursor-position', (_e, point) => callback(point)),
  onActiveWindowPos: (callback) => ipcRenderer.on('active-window-pos', (_e, pos) => callback(pos)),
  onScreenInfo: (callback) => ipcRenderer.on('screen-info', (_e, info) => callback(info)),
  onArrowMove: (callback) => ipcRenderer.on('arrow-move', (_e, data) => callback(data)),
  onUpdate: (callback) => ipcRenderer.on('update', () => callback()),
  onCopyRequest: (callback) => ipcRenderer.on('copy-request', () => callback()),
  copyImage: (dataURL) => ipcRenderer.send('copy-image', dataURL),
  onHistogramScale: (callback) => ipcRenderer.on('histogram-scale', (_e, action) => callback(action)),
}
