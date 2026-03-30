export const DEFAULT_CONFIG = {
  zoomLevel: 1,
  showGrid: true,
  showRuler: true,
  hexMode: false,
  centerHighlight: false,
  inputMode: 'mouse', // 'mouse' | 'arrow'
  coordMode: 'screen', // 'screen' | 'window'
  inverseY: false,
  boundMode: false,
  currentFilter: 'none', // 'none' | 'gradient'
  currentGraph: 'off', // 'off' | 'hstep' | 'hlinear' | 'vstep' | 'vlinear' | 'statistics'
  graphHighlight: false,
  statisticsInfo: false,
  alwaysOnTop: false,
  autoSaveConfig: false,
  refreshEnabled: true,
  refreshInterval: 1, // in 1/100 sec units
  shiftDelta: 8, // pixels to move per Shift+Arrow press
  displayOptions: {
    red: true, green: true, blue: true,
    autoNormHistogram: false, autoNormGradient: false,
    refHistogram: 4000, refGradient: 255,
    brightness: 0, contrast: 0, gamma: 1.0,
  },
}
