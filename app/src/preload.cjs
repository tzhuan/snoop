const { contextBridge, ipcRenderer } = require('electron')

// Register native frame listener eagerly — frames may arrive before renderer JS loads.
// The callback is set later by the renderer via onNativeFrame().
let nativeFrameCallback = null
ipcRenderer.on('native-frame', (_e, data) => {
  if (nativeFrameCallback) nativeFrameCallback(data)
})

contextBridge.exposeInMainWorld('snoop', {
  getSources: () => ipcRenderer.invoke('get-sources'),

  // Config: single channel for all settings
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

  // Runtime events (not config)
  onCursorPosition: (callback) => ipcRenderer.on('cursor-position', (_e, point) => callback(point)),
  onActiveWindowPos: (callback) => ipcRenderer.on('active-window-pos', (_e, pos) => callback(pos)),
  onScreenInfo: (callback) => ipcRenderer.on('screen-info', (_e, info) => callback(info)),
  onArrowMove: (callback) => ipcRenderer.on('arrow-move', (_e, data) => callback(data)),
  onUpdate: (callback) => ipcRenderer.on('update', () => callback()),
  onCopyRequest: (callback) => ipcRenderer.on('copy-request', () => callback()),
  copyImage: (dataURL) => ipcRenderer.send('copy-image', dataURL),
  onHistogramScale: (callback) => ipcRenderer.on('histogram-scale', (_e, action) => callback(action)),
  onNativeFrame: (callback) => { nativeFrameCallback = callback },
})
