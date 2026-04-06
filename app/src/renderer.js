import { DEFAULT_CONFIG } from './config-defaults.js'

const CANVAS = document.getElementById('magnifier')
const CTX = CANVAS.getContext('2d')
const VIDEO = document.getElementById('screen-video')
const STATUS_TEXT = document.getElementById('status-text')

const CONFIG = structuredClone(DEFAULT_CONFIG)

// Capture mode: 'video' (desktopCapturer/getDisplayMedia) or 'native' (PipeWire addon)
let CAPTURE_MODE = 'video'

// Runtime state (not persisted)
const RUNTIME = {
  cursorX: 0,
  cursorY: 0,
  stream: null,
  activeWindowPos: { x: 0, y: 0 },
  screenWidth: 0,
  screenHeight: 0,
  displays: [],       // Full display list from main process
  tracking: false,
  workerBusy: false,
  workerFrameId: 0,
  pendingFrame: null,
  refreshTimerId: null,
}

function isMultiInstance() {
  return !!(window.snoop && window.snoop.needsMultiInstance)
}

const RULER_SIZE = 12

function rulerSize() { return CONFIG.showRuler ? RULER_SIZE : 0 }

// Hidden canvas for capturing full-resolution screen frames
const OFFSCREEN = document.createElement('canvas')
const OFF_CTX = OFFSCREEN.getContext('2d', { willReadFrequently: true })

// Buffer canvas for worker pipeline (avoids flashing unprocessed frames)
const BUFFER = document.createElement('canvas')
const BUFFER_CTX = BUFFER.getContext('2d')

// Web Worker for pixel processing
const PIXEL_WORKER = new Worker('pixel-worker.js')

PIXEL_WORKER.onmessage = function(e) {
  const { type, id, buffer, stats, width, height } = e.data
  if (type !== 'result') return

  RUNTIME.workerBusy = false

  if (!RUNTIME.pendingFrame || RUNTIME.pendingFrame.id !== id) return

  const frame = RUNTIME.pendingFrame
  RUNTIME.pendingFrame = null

  const resultData = new Uint8ClampedArray(buffer)
  const imageData = new ImageData(resultData, width, height)
  CTX.putImageData(imageData, 0, 0)

  CTX.save()
  CTX.beginPath()
  CTX.rect(frame.vpX, frame.vpY, frame.vpW, frame.vpH)
  CTX.clip()

  drawOverlays(frame, stats)

  CTX.restore()

  drawRulers(frame.srcX, frame.srcY, frame.srcW, frame.srcH)
  updateStatusBar()
}

async function initCapture() {
  const sources = await window.snoop.getSources()

  // Native capture mode (Linux) — addon loaded in preload, zero-copy frames
  if (sources.length > 0 && sources[0].id === '__native__') {
    CAPTURE_MODE = 'native'
    const ret = window.snoop.startNativeCapture()
    if (ret !== 0) {
      console.warn('Native capture failed to start')
    }
    return
  }

  // Video capture (macOS, Windows)
  CAPTURE_MODE = 'video'
  if (sources.length > 0) {
    try {
      RUNTIME.stream = await navigator.mediaDevices.getUserMedia({
        audio: false,
        video: {
          mandatory: {
            chromeMediaSource: 'desktop',
            chromeMediaSourceId: sources[0].id,
          },
          cursor: 'never',
        },
      })
      VIDEO.srcObject = RUNTIME.stream
      return
    } catch (err) {
      console.debug('desktopCapturer getUserMedia unavailable, trying getDisplayMedia')
    }
  }

  // Fallback to getDisplayMedia
  try {
    RUNTIME.stream = await navigator.mediaDevices.getDisplayMedia({
      audio: false,
      video: { cursor: 'never' },
    })
    VIDEO.srcObject = RUNTIME.stream
  } catch (err) {
    console.warn('getDisplayMedia not available:', err.message)
  }
}

function resizeCanvas() {
  CANVAS.width = CANVAS.clientWidth
  CANVAS.height = CANVAS.clientHeight
}

function needsDisplayProcessing() {
  const d = CONFIG.displayOptions
  return !d.red || !d.green || !d.blue ||
    d.brightness !== 0 || d.contrast !== 0 || d.gamma !== 1.0
}

function needsWorkerProcessing() {
  return CONFIG.currentFilter === 'gradient' || needsDisplayProcessing() ||
    CONFIG.currentGraph === 'statistics'
}

function renderFrame() {
  if (CAPTURE_MODE === 'native') {
    if (isMultiInstance()) {
      // Multi-instance: composite frames from multiple displays
      const frames = window.snoop.getNativeFrames()
      if (!frames || frames.length === 0) return

      const region = window.snoop.getCaptureRegion()
      // OFFSCREEN covers the viewport region size
      const regionW = RUNTIME.screenWidth || 1920
      const regionH = RUNTIME.screenHeight || 1080

      // Use the first frame's dimensions as OFFSCREEN size hint
      let offW = 0, offH = 0
      for (const { frame, bounds } of frames) {
        if (bounds) {
          // Compute where this frame maps in viewport space
          const dx = bounds.x - Math.max(0, region.x)
          const dy = bounds.y - Math.max(0, region.y)
          offW = Math.max(offW, dx + frame.width)
          offH = Math.max(offH, dy + frame.height)
        } else {
          offW = Math.max(offW, frame.width)
          offH = Math.max(offH, frame.height)
        }
      }
      OFFSCREEN.width = offW
      OFFSCREEN.height = offH
      OFF_CTX.fillStyle = '#000'
      OFF_CTX.fillRect(0, 0, offW, offH)

      try {
        for (const { frame, bounds } of frames) {
          const { buffer, width, height } = frame
          const pixels = new Uint8ClampedArray(buffer)
          const imageData = new ImageData(pixels, width, height)
          if (bounds) {
            const dx = bounds.x - Math.max(0, region.x)
            const dy = bounds.y - Math.max(0, region.y)
            OFF_CTX.putImageData(imageData, dx, dy)
          } else {
            OFF_CTX.putImageData(imageData, 0, 0)
          }
        }
      } catch (err) {
        console.error('multi-instance renderFrame error:', err)
      }
    } else {
      // Single-instance: one frame covers the whole viewport
      const frame = window.snoop.getNativeFrame()
      if (!frame) return

      try {
        const { buffer, width, height } = frame
        OFFSCREEN.width = width
        OFFSCREEN.height = height
        const pixels = new Uint8ClampedArray(buffer)
        const imageData = new ImageData(pixels, width, height)
        OFF_CTX.putImageData(imageData, 0, 0)
      } catch (err) {
        console.error('native renderFrame error:', err)
      }
    }
  } else {
    if (VIDEO.readyState < VIDEO.HAVE_CURRENT_DATA) return

    OFFSCREEN.width = VIDEO.videoWidth
    OFFSCREEN.height = VIDEO.videoHeight
    OFF_CTX.drawImage(VIDEO, 0, 0)
  }

  const useWorker = needsWorkerProcessing()

  // Skip the entire frame if worker is still busy
  if (useWorker && RUNTIME.workerBusy) return

  const rs = rulerSize()
  const vpX = rs
  const vpY = rs
  const vpW = CANVAS.width - rs
  const vpH = CANVAS.height - rs

  const srcW = vpW / CONFIG.zoomLevel
  const srcH = vpH / CONFIG.zoomLevel

  // Apply focus offset only for stream driver (cursor rendered in capture)
  const hasCursorInCapture = CONFIG.captureDriver === 'stream'
  let curX = RUNTIME.cursorX + (hasCursorInCapture ? (CONFIG.focusOffsetX || 0) : 0)
  let curY = RUNTIME.cursorY + (hasCursorInCapture ? (CONFIG.focusOffsetY || 0) : 0)
  if (CONFIG.boundMode) {
    if (RUNTIME.displays.length > 0) {
      // Find the display containing the cursor
      const disp = RUNTIME.displays.find(d =>
        curX >= d.bounds.x && curX < d.bounds.x + d.bounds.width &&
        curY >= d.bounds.y && curY < d.bounds.y + d.bounds.height
      ) || RUNTIME.displays[0]
      const b = disp.bounds
      curX = Math.max(b.x, Math.min(b.x + b.width - 1, curX))
      curY = Math.max(b.y, Math.min(b.y + b.height - 1, curY))
    } else if (RUNTIME.screenWidth > 0 && RUNTIME.screenHeight > 0) {
      curX = Math.max(0, Math.min(RUNTIME.screenWidth - 1, curX))
      curY = Math.max(0, Math.min(RUNTIME.screenHeight - 1, curY))
    }
  }

  // Calculate source region in screen space (can be negative at edges)
  const screenSrcX = curX - srcW / 2
  const screenSrcY = curY - srcH / 2

  // When worker processing is needed, draw to buffer canvas instead of
  // the visible canvas to avoid flashing unprocessed frames
  const drawCtx = useWorker ? BUFFER_CTX : CTX
  if (useWorker) {
    BUFFER.width = CANVAS.width
    BUFFER.height = CANVAS.height
  }

  drawCtx.fillStyle = '#000'
  drawCtx.fillRect(0, 0, CANVAS.width, CANVAS.height)

  // Clamp source to screen boundaries
  const screenW = CAPTURE_MODE === 'native' ? RUNTIME.screenWidth : OFFSCREEN.width
  const screenH = CAPTURE_MODE === 'native' ? RUNTIME.screenHeight : OFFSCREEN.height
  const clampedScreenX = Math.max(0, screenSrcX)
  const clampedScreenY = Math.max(0, screenSrcY)
  const clampedScreenR = Math.min(screenW, screenSrcX + srcW)
  const clampedScreenB = Math.min(screenH, screenSrcY + srcH)
  const clampedSrcW = clampedScreenR - clampedScreenX
  const clampedSrcH = clampedScreenB - clampedScreenY

  if (clampedSrcW > 0 && clampedSrcH > 0) {
    // Map screen coordinates to OFFSCREEN coordinates
    let offSrcX, offSrcY
    if (CAPTURE_MODE === 'native') {
      const region = window.snoop.getCaptureRegion()
      // Actual captured region starts at max(0, region.x) in screen space
      const actualRegionX = Math.max(0, region.x)
      const actualRegionY = Math.max(0, region.y)
      offSrcX = clampedScreenX - actualRegionX
      offSrcY = clampedScreenY - actualRegionY
    } else {
      offSrcX = clampedScreenX
      offSrcY = clampedScreenY
    }

    const dstX = vpX + (clampedScreenX - screenSrcX) * CONFIG.zoomLevel
    const dstY = vpY + (clampedScreenY - screenSrcY) * CONFIG.zoomLevel
    const dstW = clampedSrcW * CONFIG.zoomLevel
    const dstH = clampedSrcH * CONFIG.zoomLevel

    drawCtx.imageSmoothingEnabled = false
    drawCtx.drawImage(OFFSCREEN, offSrcX, offSrcY, clampedSrcW, clampedSrcH, dstX, dstY, dstW, dstH)
  }

  const srcX = screenSrcX
  const srcY = screenSrcY
  const frameContext = { vpX, vpY, vpW, vpH, srcX, srcY, srcW, srcH }

  if (useWorker) {
    const frameId = ++RUNTIME.workerFrameId
    const imageData = BUFFER_CTX.getImageData(0, 0, CANVAS.width, CANVAS.height)
    const buffer = imageData.data.buffer

    RUNTIME.pendingFrame = { id: frameId, ...frameContext }

    RUNTIME.workerBusy = true
    PIXEL_WORKER.postMessage({
      type: 'process',
      id: frameId,
      buffer,
      width: CANVAS.width,
      height: CANVAS.height,
      filter: CONFIG.currentFilter,
      needsDisplay: needsDisplayProcessing(),
      dispOpts: { ...CONFIG.displayOptions },
      needsStats: CONFIG.currentGraph === 'statistics',
    }, [buffer])
  } else {
    CTX.save()
    CTX.beginPath()
    CTX.rect(vpX, vpY, vpW, vpH)
    CTX.clip()

    drawOverlays(frameContext, null)

    CTX.restore()

    drawRulers(srcX, srcY, srcW, srcH)
    updateStatusBar()
  }
}

function drawOverlays(frame, stats) {
  const { vpX, vpY, vpW, vpH, srcX, srcY, srcW, srcH } = frame

  if (CONFIG.showGrid && CONFIG.zoomLevel >= 2) {
    drawGrid(srcX, srcY)
  }

  if (CONFIG.centerHighlight && CONFIG.zoomLevel > 1) {
    const px = vpX + Math.floor(vpW / 2 - ((RUNTIME.cursorX - Math.floor(RUNTIME.cursorX)) * CONFIG.zoomLevel))
    const py = vpY + Math.floor(vpH / 2 - ((RUNTIME.cursorY - Math.floor(RUNTIME.cursorY)) * CONFIG.zoomLevel))
    CTX.strokeStyle = 'red'
    CTX.lineWidth = 1
    CTX.strokeRect(px + 0.5, py + 0.5, CONFIG.zoomLevel - 1, CONFIG.zoomLevel - 1)
  }

  if (CONFIG.currentGraph === 'statistics') {
    drawStatisticsOverlay(CONFIG.statisticsInfo, stats)
  } else if (CONFIG.currentGraph !== 'off') {
    drawGraph(srcX, srcY, srcW, srcH)
  }
}

function drawRulers(srcX, srcY, srcW, srcH) {
  if (!CONFIG.showRuler) return

  const vpX = RULER_SIZE
  const vpY = RULER_SIZE
  const vpW = CANVAS.width - RULER_SIZE
  const vpH = CANVAS.height - RULER_SIZE

  CTX.fillStyle = '#c0c0c0'
  CTX.fillRect(0, 0, CANVAS.width, RULER_SIZE)
  CTX.fillRect(0, 0, RULER_SIZE, CANVAS.height)

  CTX.fillStyle = '#a0a0a0'
  CTX.fillRect(0, 0, RULER_SIZE, RULER_SIZE)

  CTX.fillStyle = '#000'
  CTX.strokeStyle = '#000'
  CTX.lineWidth = 1

  const majorInterval = CONFIG.zoomLevel >= 4 ? 5 : 10

  const firstPixelX = Math.floor(srcX)
  const offsetPxX = (firstPixelX - srcX) * CONFIG.zoomLevel

  for (let p = firstPixelX; ; p++) {
    const x = vpX + offsetPxX + (p - firstPixelX) * CONFIG.zoomLevel
    if (x > CANVAS.width) break
    if (x < vpX - CONFIG.zoomLevel) continue

    const tickX = Math.round(x) + 0.5
    const isMajor = p % majorInterval === 0

    CTX.beginPath()
    CTX.moveTo(tickX, RULER_SIZE - (isMajor ? 8 : 4))
    CTX.lineTo(tickX, RULER_SIZE)
    CTX.stroke()
  }

  const firstPixelY = Math.floor(srcY)
  const offsetPxY = (firstPixelY - srcY) * CONFIG.zoomLevel

  for (let p = firstPixelY; ; p++) {
    const y = vpY + offsetPxY + (p - firstPixelY) * CONFIG.zoomLevel
    if (y > CANVAS.height) break
    if (y < vpY - CONFIG.zoomLevel) continue

    const tickY = Math.round(y) + 0.5
    const isMajor = p % majorInterval === 0

    CTX.beginPath()
    CTX.moveTo(RULER_SIZE - (isMajor ? 8 : 4), tickY)
    CTX.lineTo(RULER_SIZE, tickY)
    CTX.stroke()
  }

  CTX.strokeStyle = '#666'
  CTX.beginPath()
  CTX.moveTo(RULER_SIZE, 0)
  CTX.lineTo(RULER_SIZE, CANVAS.height)
  CTX.moveTo(0, RULER_SIZE)
  CTX.lineTo(CANVAS.width, RULER_SIZE)
  CTX.stroke()
}

function render() {
  renderFrame()
}

function drawGrid(srcX, srcY) {
  const rs = rulerSize()
  const vpX = rs
  const vpY = rs
  const vpW = CANVAS.width - rs
  const vpH = CANVAS.height - rs

  CTX.strokeStyle = 'rgba(255, 255, 255, 0.2)'
  CTX.lineWidth = 1

  const offsetX = ((-srcX % 1) + 1) % 1
  const offsetY = ((-srcY % 1) + 1) % 1

  for (let x = offsetX * CONFIG.zoomLevel; x < vpW; x += CONFIG.zoomLevel) {
    const px = Math.round(vpX + x) + 0.5
    CTX.beginPath()
    CTX.moveTo(px, vpY)
    CTX.lineTo(px, CANVAS.height)
    CTX.stroke()
  }

  for (let y = offsetY * CONFIG.zoomLevel; y < vpH; y += CONFIG.zoomLevel) {
    const py = Math.round(vpY + y) + 0.5
    CTX.beginPath()
    CTX.moveTo(vpX, py)
    CTX.lineTo(CANVAS.width, py)
    CTX.stroke()
  }
}

function getLuminance(r, g, b) {
  return 0.299 * r + 0.587 * g + 0.114 * b
}

function drawGraph(srcX, srcY, srcW, srcH) {
  const isHorizontal = CONFIG.currentGraph === 'hstep' || CONFIG.currentGraph === 'hlinear'
  const isStep = CONFIG.currentGraph === 'hstep' || CONFIG.currentGraph === 'vstep'

  const rs = rulerSize()
  const vpX = rs
  const vpY = rs
  const vpW = CANVAS.width - rs
  const vpH = CANVAS.height - rs

  const graphSize = isHorizontal ? Math.floor(vpH / 3) : Math.floor(vpW / 3)
  const graphOriginX = isHorizontal ? vpX : (CANVAS.width - graphSize)

  CTX.save()
  CTX.fillStyle = 'rgba(0, 0, 0, 0.85)'
  if (isHorizontal) {
    CTX.fillRect(vpX, vpY, vpW, graphSize)
  } else {
    CTX.fillRect(graphOriginX, vpY, graphSize, vpH)
  }

  const centerCanvasX = vpX + Math.floor(vpW / 2)
  const centerCanvasY = vpY + Math.floor(vpH / 2)

  const channels = { r: [], g: [], b: [] }

  if (isHorizontal) {
    const numPixels = Math.ceil(srcW)
    const sy = Math.floor(srcY + srcH / 2)
    const startX = Math.floor(srcX)

    if (sy >= 0 && sy < OFFSCREEN.height && OFFSCREEN.width > 0) {
      const readX = Math.max(0, startX)
      const readEnd = Math.min(OFFSCREEN.width, startX + numPixels)
      const readW = readEnd - readX
      if (readW > 0) {
        const rowData = OFF_CTX.getImageData(readX, sy, readW, 1).data
        for (let p = 0; p < numPixels; p++) {
          const sx = startX + p
          if (sx >= 0 && sx < OFFSCREEN.width) {
            const idx = (sx - readX) * 4
            channels.r.push(rowData[idx])
            channels.g.push(rowData[idx + 1])
            channels.b.push(rowData[idx + 2])
          } else {
            channels.r.push(0); channels.g.push(0); channels.b.push(0)
          }
        }
      } else {
        for (let p = 0; p < numPixels; p++) {
          channels.r.push(0); channels.g.push(0); channels.b.push(0)
        }
      }
    } else {
      for (let p = 0; p < numPixels; p++) {
        channels.r.push(0); channels.g.push(0); channels.b.push(0)
      }
    }
  } else {
    const numPixels = Math.ceil(srcH)
    const sx = Math.floor(srcX + srcW / 2)
    const startY = Math.floor(srcY)

    if (sx >= 0 && sx < OFFSCREEN.width && OFFSCREEN.height > 0) {
      const readY = Math.max(0, startY)
      const readEnd = Math.min(OFFSCREEN.height, startY + numPixels)
      const readH = readEnd - readY
      if (readH > 0) {
        const colData = OFF_CTX.getImageData(sx, readY, 1, readH).data
        for (let p = 0; p < numPixels; p++) {
          const sy = startY + p
          if (sy >= 0 && sy < OFFSCREEN.height) {
            const idx = (sy - readY) * 4
            channels.r.push(colData[idx])
            channels.g.push(colData[idx + 1])
            channels.b.push(colData[idx + 2])
          } else {
            channels.r.push(0); channels.g.push(0); channels.b.push(0)
          }
        }
      } else {
        for (let p = 0; p < numPixels; p++) {
          channels.r.push(0); channels.g.push(0); channels.b.push(0)
        }
      }
    } else {
      for (let p = 0; p < numPixels; p++) {
        channels.r.push(0); channels.g.push(0); channels.b.push(0)
      }
    }
  }

  const colors = [
    { key: 'r', color: 'rgba(255, 0, 0, 0.8)' },
    { key: 'g', color: 'rgba(0, 255, 0, 0.8)' },
    { key: 'b', color: 'rgba(0, 0, 255, 0.8)' },
  ]

  const offsetPx = isHorizontal
    ? ((Math.floor(srcX) - srcX) * CONFIG.zoomLevel)
    : ((Math.floor(srcY) - srcY) * CONFIG.zoomLevel)

  for (const { key, color } of colors) {
    const vals = channels[key]
    CTX.strokeStyle = color
    CTX.lineWidth = 1
    CTX.beginPath()

    for (let p = 0; p < vals.length; p++) {
      const norm = vals[p] / 255

      if (isHorizontal) {
        const pixStart = vpX + offsetPx + p * CONFIG.zoomLevel
        const pixEnd = pixStart + CONFIG.zoomLevel
        const gy = vpY + graphSize - norm * graphSize

        if (isStep) {
          if (p === 0) CTX.moveTo(pixStart, gy)
          else CTX.lineTo(pixStart, gy)
          CTX.lineTo(pixEnd, gy)
        } else {
          const pixCenter = pixStart + CONFIG.zoomLevel / 2
          if (p === 0) CTX.moveTo(pixCenter, gy)
          else CTX.lineTo(pixCenter, gy)
        }
      } else {
        const pixStart = vpY + offsetPx + p * CONFIG.zoomLevel
        const pixEnd = pixStart + CONFIG.zoomLevel
        const gx = graphOriginX + norm * graphSize

        if (isStep) {
          if (p === 0) CTX.moveTo(gx, pixStart)
          else CTX.lineTo(gx, pixStart)
          CTX.lineTo(gx, pixEnd)
        } else {
          const pixCenter = pixStart + CONFIG.zoomLevel / 2
          if (p === 0) CTX.moveTo(gx, pixCenter)
          else CTX.lineTo(gx, pixCenter)
        }
      }
    }

    CTX.stroke()
  }

  if (CONFIG.graphHighlight) {
    CTX.fillStyle = 'rgba(255, 255, 0, 0.15)'
    if (isHorizontal) {
      CTX.fillRect(vpX, centerCanvasY, vpW, CONFIG.zoomLevel)
    } else {
      CTX.fillRect(centerCanvasX, vpY, CONFIG.zoomLevel, vpH)
    }
  }

  CTX.restore()
}

function drawStatisticsOverlay(showText, stats) {
  if (!stats) return

  const rs = rulerSize()
  const vpX = rs
  const vpY = rs
  const vpW = CANVAS.width - rs
  const vpH = CANVAS.height - rs
  const graphH = Math.floor(vpH / 3)
  const textH = 32

  CTX.save()

  CTX.fillStyle = 'rgba(0, 0, 0, 0.85)'
  CTX.fillRect(vpX, vpY, vpW, graphH)

  // Histogram Y-axis scaling: auto-detected max or fixed reference value
  let maxCount = 0
  if (CONFIG.displayOptions.autoNormHistogram) {
    for (let v = 0; v < 256; v++) {
      if (stats.histR[v] > maxCount) maxCount = stats.histR[v]
      if (stats.histG[v] > maxCount) maxCount = stats.histG[v]
      if (stats.histB[v] > maxCount) maxCount = stats.histB[v]
    }
  } else {
    maxCount = CONFIG.displayOptions.refHistogram || 1
  }

  if (maxCount > 0) {
    const binWidth = vpW / 256

    const histChannels = [
      { hist: stats.histR, color: 'rgba(255, 0, 0, 0.7)' },
      { hist: stats.histG, color: 'rgba(0, 255, 0, 0.7)' },
      { hist: stats.histB, color: 'rgba(0, 0, 255, 0.7)' },
    ]

    for (const { hist, color } of histChannels) {
      CTX.strokeStyle = color
      CTX.lineWidth = 1
      CTX.beginPath()
      for (let v = 0; v < 256; v++) {
        const x = vpX + v * binWidth
        const barH = (hist[v] / maxCount) * graphH
        const y = vpY + graphH - barH
        if (v === 0) CTX.moveTo(x, y)
        else CTX.lineTo(x, y)
      }
      CTX.stroke()
    }
  }

  if (showText) {
    const pad = (n) => String(n).padStart(3, '0')
    const textY = vpY + vpH - textH
    CTX.fillStyle = 'rgba(0, 0, 0, 0.85)'
    CTX.fillRect(vpX, textY, vpW, textH)
    CTX.font = '11px monospace'
    CTX.fillStyle = '#fff'
    CTX.fillText(
      `max=(${pad(stats.max.r)},${pad(stats.max.g)},${pad(stats.max.b)})` +
      `min=(${pad(stats.min.r)},${pad(stats.min.g)},${pad(stats.min.b)})`,
      vpX + 4, textY + 13
    )
    CTX.fillText(
      `avg=(${pad(stats.avg.r)},${pad(stats.avg.g)},${pad(stats.avg.b)})` +
      `med=(${pad(stats.med.r)},${pad(stats.med.g)},${pad(stats.med.b)})`,
      vpX + 4, textY + 27
    )
  }

  CTX.restore()
}

function formatCoord(n) {
  return String(n).padStart(6, ' ')
}

function formatRgb(pixel) {
  if (CONFIG.hexMode) {
    const r = pixel[0].toString(16).padStart(2, '0')
    const g = pixel[1].toString(16).padStart(2, '0')
    const b = pixel[2].toString(16).padStart(2, '0')
    return `(${r},${g},${b})`
  }
  const r = String(pixel[0]).padStart(3, '0')
  const g = String(pixel[1]).padStart(3, '0')
  const b = String(pixel[2]).padStart(3, '0')
  return `(${r},${g},${b})`
}

function updateStatusBar() {
  let displayX = RUNTIME.cursorX
  let displayY = RUNTIME.cursorY

  if (CONFIG.coordMode === 'window') {
    displayX = RUNTIME.cursorX - RUNTIME.activeWindowPos.x
    displayY = RUNTIME.cursorY - RUNTIME.activeWindowPos.y
  }

  if (CONFIG.inverseY) {
    displayY = (CONFIG.coordMode === 'window'
      ? (RUNTIME.activeWindowPos.height || RUNTIME.screenHeight) - 1 - (RUNTIME.cursorY - RUNTIME.activeWindowPos.y)
      : RUNTIME.screenHeight - 1 - RUNTIME.cursorY)
  }

  const coords = `(${formatCoord(displayX)},${formatCoord(displayY)})`

  let rgb = '(000,000,000)'
  if (OFFSCREEN.width > 0 && OFFSCREEN.height > 0) {
    let px = Math.floor(RUNTIME.cursorX)
    let py = Math.floor(RUNTIME.cursorY)
    if (CAPTURE_MODE === 'native') {
      const region = window.snoop.getCaptureRegion()
      px -= region.x
      py -= region.y
    }
    if (px >= 0 && px < OFFSCREEN.width && py >= 0 && py < OFFSCREEN.height) {
      const pixel = OFF_CTX.getImageData(px, py, 1, 1).data
      rgb = formatRgb(pixel)
    }
  }

  const coordLabel = CONFIG.coordMode === 'window' ? 'W' : 'S'
  STATUS_TEXT.textContent = `${coords} · ${rgb} ${coordLabel}`
}

// Config change listener — single channel for all settings
window.snoop.onConfigChange((newConfig) => {
  const prevRefreshEnabled = CONFIG.refreshEnabled
  const prevRefreshInterval = CONFIG.refreshInterval
  Object.assign(CONFIG, newConfig)
  if (newConfig.displayOptions) {
    CONFIG.displayOptions = { ...newConfig.displayOptions }
  }
  if (CONFIG.refreshEnabled !== prevRefreshEnabled ||
      CONFIG.refreshInterval !== prevRefreshInterval) {
    startRefreshTimer()
  }
})

// Runtime event listeners
window.snoop.onCursorPosition((point) => {
  if (CONFIG.inputMode === 'mouse') {
    RUNTIME.cursorX = point.x
    RUNTIME.cursorY = point.y
  }
})

// Keyboard shortcuts (DOM events — active when window is focused)
let spaceHeld = false
const ARROW_DIRS = { ArrowUp: 'up', ArrowDown: 'down', ArrowLeft: 'left', ArrowRight: 'right' }

document.addEventListener('keydown', (e) => {
  if (e.code === 'Space') { spaceHeld = true; e.preventDefault(); return }

  const dir = ARROW_DIRS[e.code]
  if (dir) {
    e.preventDefault()
    if (e.altKey && CONFIG.captureDriver === 'stream') {
      // Alt+Arrow: adjust focus offset (stream driver renders cursor in capture)
      const dx = dir === 'left' ? -1 : dir === 'right' ? 1 : 0
      const dy = dir === 'up' ? -1 : dir === 'down' ? 1 : 0
      window.snoop.adjustFocusOffset(dx, dy)
    } else if (spaceHeld) {
      // Space+Arrow: resize window
      const [w, h] = [window.innerWidth, window.innerHeight]
      const step = (e.shiftKey ? CONFIG.shiftDelta : 1) * CONFIG.zoomLevel
      if (dir === 'up') window.snoop.resizeWindow(w, Math.max(200, h - step))
      else if (dir === 'down') window.snoop.resizeWindow(w, h + step)
      else if (dir === 'left') window.snoop.resizeWindow(Math.max(200, w - step), h)
      else if (dir === 'right') window.snoop.resizeWindow(w + step, h)
    } else if (e.ctrlKey && CONFIG.inputMode === 'arrow') {
      // Ctrl+Arrow: peg to screen boundary
      // Update renderer cursor position
      const rs = rulerSize()
      const vpW = CANVAS.width - rs
      const vpH = CANVAS.height - rs
      const srcW = vpW / CONFIG.zoomLevel
      const srcH = vpH / CONFIG.zoomLevel
      const disp = RUNTIME.displays.find(d =>
        RUNTIME.cursorX >= d.bounds.x && RUNTIME.cursorX < d.bounds.x + d.bounds.width &&
        RUNTIME.cursorY >= d.bounds.y && RUNTIME.cursorY < d.bounds.y + d.bounds.height
      )
      const dx = disp ? disp.bounds.x : 0
      const dy = disp ? disp.bounds.y : 0
      const dw = disp ? disp.bounds.width : RUNTIME.screenWidth
      const dh = disp ? disp.bounds.height : RUNTIME.screenHeight
      if (dir === 'up') RUNTIME.cursorY = dy + Math.floor(srcH / 2)
      else if (dir === 'down') RUNTIME.cursorY = dy + dh - Math.floor(srcH / 2)
      else if (dir === 'left') RUNTIME.cursorX = dx + Math.floor(srcW / 2)
      else if (dir === 'right') RUNTIME.cursorX = dx + dw - Math.floor(srcW / 2)
      window.snoop.pegCursor(dir)
      renderFrame()
    } else if (CONFIG.inputMode === 'arrow') {
      // Arrow / Shift+Arrow: move cursor
      const d = e.shiftKey ? CONFIG.shiftDelta : 1
      if (dir === 'up') { RUNTIME.cursorY -= d; window.snoop.moveCursor(0, -d) }
      else if (dir === 'down') { RUNTIME.cursorY += d; window.snoop.moveCursor(0, d) }
      else if (dir === 'left') { RUNTIME.cursorX -= d; window.snoop.moveCursor(-d, 0) }
      else if (dir === 'right') { RUNTIME.cursorX += d; window.snoop.moveCursor(d, 0) }
      renderFrame()
    }
    return
  }

  // PageUp / PageDown
  if (e.code === 'PageUp' || e.code === 'PageDown') {
    e.preventDefault()
    const up = e.code === 'PageUp'
    if (e.shiftKey && e.ctrlKey) {
      window.snoop.adjustDisplayOption('gamma', up ? 0.02 : -0.02, 0.02, 2.0)
    } else if (e.shiftKey) {
      window.snoop.adjustDisplayOption('brightness', up ? 2 : -2, -100, 100)
    } else if (e.ctrlKey) {
      window.snoop.adjustDisplayOption('contrast', up ? 2 : -2, -100, 100)
    } else {
      window.snoop.histogramScale(up ? 'up' : 'down')
    }
    return
  }

  if (e.code === 'Home') { e.preventDefault(); window.snoop.histogramScale('reset'); return }
  if (e.code === 'Tab') { e.preventDefault(); window.snoop.cycleFilter(); return }
  if (e.code === 'Equal' || e.code === 'NumpadAdd') { e.preventDefault(); window.snoop.zoomIn(); return }
  if (e.code === 'Minus' || e.code === 'NumpadSubtract') { e.preventDefault(); window.snoop.zoomOut(); return }
  if (e.code === 'Backspace') { e.preventDefault(); window.snoop.resetDefaults(); return }
  if (e.code === 'Delete') { e.preventDefault(); window.snoop.clearAllConfigs(); return }

  // F1-F9: Ctrl+F = load slot, Shift+F = save slot
  const fMatch = e.code.match(/^F(\d)$/)
  if (fMatch) {
    const slot = parseInt(fMatch[1])
    if (slot >= 1 && slot <= 9) {
      if (e.ctrlKey && !e.shiftKey) { e.preventDefault(); window.snoop.loadConfigSlot(slot); return }
      if (e.shiftKey && !e.ctrlKey) { e.preventDefault(); window.snoop.saveConfigSlot(slot); return }
    }
  }
})

document.addEventListener('keyup', (e) => {
  if (e.code === 'Space') spaceHeld = false
})

window.snoop.onActiveWindowPos((pos) => {
  RUNTIME.activeWindowPos = pos
})

window.snoop.onScreenInfo((info) => {
  RUNTIME.screenWidth = info.width
  RUNTIME.screenHeight = info.height
  if (info.displays) {
    RUNTIME.displays = info.displays
  }
})

window.snoop.onCaptureReinit(async () => {
  // Stop existing capture
  if (RUNTIME.stream) {
    RUNTIME.stream.getTracks().forEach(t => t.stop())
    RUNTIME.stream = null
    VIDEO.srcObject = null
  }
  window.snoop.stopNativeCapture()
  // Re-initialize with current driver setting
  await initCapture()
})

window.snoop.onUpdate(() => {
  renderFrame()
})

window.snoop.onCopyRequest(() => {
  const dataURL = CANVAS.toDataURL('image/png')
  window.snoop.copyImage(dataURL)
})

// Config persistence via localStorage
const CONFIG_KEY = 'snoop-config'

window.snoop.onSaveConfig((cfg) => {
  try {
    localStorage.setItem(CONFIG_KEY, JSON.stringify(cfg))
  } catch (e) {
    console.error('Failed to save config:', e)
  }
})

window.snoop.onRequestConfig(() => {
  try {
    const raw = localStorage.getItem(CONFIG_KEY)
    window.snoop.sendSavedConfig(raw ? JSON.parse(raw) : null)
  } catch (e) {
    console.error('Failed to load config:', e)
    window.snoop.sendSavedConfig(null)
  }
})

// Track mode: mouse drag in the magnifier to move zoom center
CANVAS.addEventListener('mousedown', (e) => {
  if (e.button !== 0) return
  RUNTIME.tracking = true
  updateTrackPosition(e)
})

CANVAS.addEventListener('mousemove', (e) => {
  if (!RUNTIME.tracking) return
  updateTrackPosition(e)
})

CANVAS.addEventListener('mouseup', () => {
  RUNTIME.tracking = false
})

CANVAS.addEventListener('mouseleave', () => {
  RUNTIME.tracking = false
})

function updateTrackPosition(e) {
  const rs = rulerSize()
  const vpW = CANVAS.width - rs
  const vpH = CANVAS.height - rs
  const srcW = vpW / CONFIG.zoomLevel
  const srcH = vpH / CONFIG.zoomLevel
  const hasCursorInTrack = CONFIG.captureDriver === 'stream'
  const centerX = RUNTIME.cursorX + (hasCursorInTrack ? (CONFIG.focusOffsetX || 0) : 0)
  const centerY = RUNTIME.cursorY + (hasCursorInTrack ? (CONFIG.focusOffsetY || 0) : 0)
  const srcX = centerX - srcW / 2
  const srcY = centerY - srcH / 2
  RUNTIME.cursorX = Math.floor(srcX + (e.offsetX - rs) / CONFIG.zoomLevel)
  RUNTIME.cursorY = Math.floor(srcY + (e.offsetY - rs) / CONFIG.zoomLevel)
  renderFrame()
}

// Histogram scale adjustment via PageUp/Down
window.snoop.onHistogramScale((action) => {
  if (CONFIG.currentGraph !== 'statistics') return
  if (action === 'reset') {
    CONFIG.displayOptions.refHistogram = 4000
  } else if (action === 'up') {
    CONFIG.displayOptions.refHistogram = Math.max(10, CONFIG.displayOptions.refHistogram - Math.ceil(CONFIG.displayOptions.refHistogram * 0.1))
  } else if (action === 'down') {
    CONFIG.displayOptions.refHistogram += Math.ceil(CONFIG.displayOptions.refHistogram * 0.1)
  }
})

// Config slot persistence
const SLOT_KEY_PREFIX = 'snoop-config-slot-'

window.snoop.onSaveConfigSlot((data) => {
  try {
    localStorage.setItem(SLOT_KEY_PREFIX + data.slot, JSON.stringify(data.config))
  } catch (e) {
    console.error('Failed to save config slot:', e)
  }
})

window.snoop.onLoadConfigSlot((slot) => {
  try {
    const raw = localStorage.getItem(SLOT_KEY_PREFIX + slot)
    window.snoop.sendSlotConfig(raw ? JSON.parse(raw) : null)
  } catch (e) {
    console.error('Failed to load config slot:', e)
    window.snoop.sendSlotConfig(null)
  }
})

window.snoop.onRequestConfigSlot((slot) => {
  try {
    const raw = localStorage.getItem(SLOT_KEY_PREFIX + slot)
    window.snoop.sendSlotConfig(raw ? JSON.parse(raw) : null)
  } catch (e) {
    console.error('Failed to load config slot:', e)
    window.snoop.sendSlotConfig(null)
  }
})

window.snoop.onClearAllConfigs(() => {
  try {
    localStorage.removeItem(CONFIG_KEY)
    for (let i = 1; i <= 10; i++) {
      localStorage.removeItem(SLOT_KEY_PREFIX + i)
    }
  } catch (e) {
    console.error('Failed to clear configs:', e)
  }
})

// Refresh timer management
function startRefreshTimer() {
  stopRefreshTimer()
  if (CONFIG.refreshEnabled) {
    RUNTIME.refreshTimerId = setInterval(render, CONFIG.refreshInterval * 10)
  }
}

function stopRefreshTimer() {
  if (RUNTIME.refreshTimerId !== null) {
    clearInterval(RUNTIME.refreshTimerId)
    RUNTIME.refreshTimerId = null
  }
}

// Initialize
window.addEventListener('resize', resizeCanvas)
resizeCanvas()
initCapture()
startRefreshTimer()
