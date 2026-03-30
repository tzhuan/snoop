import { ipcRenderer } from 'electron'
import { createCapture } from '@snoop/capture'

// Native capture — loaded directly in the renderer process (zero-copy frames)
let nativeCapture = null
let latestNativeFrame = null

try {
  nativeCapture = createCapture()
  nativeCapture.onFrame((buffer, width, height) => {
    latestNativeFrame = { buffer, width, height }
  })
} catch (err) {
  console.warn('Native capture not available:', err.message)
}

// Track capture region offset for renderer coordinate mapping
let captureRegionX = 0
let captureRegionY = 0

ipcRenderer.on('capture-region', (_e, { x, y, w, h }) => {
  captureRegionX = x
  captureRegionY = y
  if (nativeCapture) nativeCapture.setRegion(x, y, w, h)
})

ipcRenderer.on('capture-rate', (_e, fps) => {
  if (nativeCapture) nativeCapture.setRate(fps)
})

ipcRenderer.on('capture-suspend', () => {
  if (nativeCapture) nativeCapture.suspend()
})

ipcRenderer.on('capture-resume', () => {
  if (nativeCapture) nativeCapture.resume()
})

// With contextIsolation disabled, set globals directly on window
window.snoop = {
  getSources: () => ipcRenderer.invoke('get-sources'),

  // Native capture
  getNativeFrame: () => {
    const frame = latestNativeFrame
    latestNativeFrame = null
    return frame
  },
  getCaptureRegion: () => ({ x: captureRegionX, y: captureRegionY }),
  startNativeCapture: () => nativeCapture ? nativeCapture.start() : -1,
  stopNativeCapture: () => { if (nativeCapture) nativeCapture.stop() },

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
