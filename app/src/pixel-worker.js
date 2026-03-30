// Web Worker for CPU-intensive pixel operations
// Receives ImageData buffers via transferable objects for zero-copy transfer

function applyGradientFilter(data, w, h, autoNorm, refGradient) {
  const src = data
  const pixelCount = w * h
  // Store raw gradient values as floats for normalization
  const rawR = new Float32Array(pixelCount)
  const rawG = new Float32Array(pixelCount)
  const rawB = new Float32Array(pixelCount)

  let maxGrad = 0

  // First pass: compute raw gradient magnitudes
  for (let y = 0; y < h; y++) {
    for (let x = 0; x < w; x++) {
      const idx = y * w + x
      const i = idx * 4
      const left = x > 0 ? (y * w + x - 1) * 4 : i
      const right = x < w - 1 ? (y * w + x + 1) * 4 : i
      const top = y > 0 ? ((y - 1) * w + x) * 4 : i
      const bottom = y < h - 1 ? ((y + 1) * w + x) * 4 : i

      for (let c = 0; c < 3; c++) {
        const dx = src[right + c] - src[left + c]
        const dy = src[bottom + c] - src[top + c]
        const grad = Math.sqrt(dx * dx + dy * dy)
        if (c === 0) rawR[idx] = grad
        else if (c === 1) rawG[idx] = grad
        else rawB[idx] = grad
        if (grad > maxGrad) maxGrad = grad
      }
    }
  }

  // Determine normalization max: auto-detected or reference value
  const normMax = autoNorm ? (maxGrad || 1) : (refGradient || 1)

  // Second pass: normalize to 0-255
  const out = new Uint8ClampedArray(src.length)
  for (let idx = 0; idx < pixelCount; idx++) {
    const i = idx * 4
    out[i] = Math.min(255, Math.round(rawR[idx] / normMax * 255))
    out[i + 1] = Math.min(255, Math.round(rawG[idx] / normMax * 255))
    out[i + 2] = Math.min(255, Math.round(rawB[idx] / normMax * 255))
    out[i + 3] = 255
  }

  return out
}

function applyDisplayOptions(data, opts) {
  const { red, green, blue, brightness, contrast, gamma } = opts

  const contrastFactor = (259 * (contrast + 255)) / (255 * (259 - contrast))

  // Build gamma LUT
  let lut = null
  if (gamma !== 1.0) {
    lut = new Uint8Array(256)
    const invGamma = 1.0 / gamma
    for (let i = 0; i < 256; i++) {
      lut[i] = Math.min(255, Math.max(0, Math.round(255 * Math.pow(i / 255, invGamma))))
    }
  }

  for (let i = 0; i < data.length; i += 4) {
    let r = data[i]
    let g = data[i + 1]
    let b = data[i + 2]

    if (brightness !== 0) {
      r += brightness
      g += brightness
      b += brightness
    }

    if (contrast !== 0) {
      r = contrastFactor * (r - 128) + 128
      g = contrastFactor * (g - 128) + 128
      b = contrastFactor * (b - 128) + 128
    }

    r = Math.min(255, Math.max(0, Math.round(r)))
    g = Math.min(255, Math.max(0, Math.round(g)))
    b = Math.min(255, Math.max(0, Math.round(b)))

    if (lut) {
      r = lut[r]
      g = lut[g]
      b = lut[b]
    }

    data[i] = red ? r : 0
    data[i + 1] = green ? g : 0
    data[i + 2] = blue ? b : 0
  }

  return data
}

function computeStatistics(data, w, h) {
  const pixelCount = w * h

  let minR = 255, minG = 255, minB = 255
  let maxR = 0, maxG = 0, maxB = 0
  let sumR = 0, sumG = 0, sumB = 0

  const histR = new Uint32Array(256)
  const histG = new Uint32Array(256)
  const histB = new Uint32Array(256)

  for (let i = 0; i < data.length; i += 4) {
    const r = data[i], g = data[i + 1], b = data[i + 2]
    if (r < minR) minR = r
    if (g < minG) minG = g
    if (b < minB) minB = b
    if (r > maxR) maxR = r
    if (g > maxG) maxG = g
    if (b > maxB) maxB = b
    sumR += r
    sumG += g
    sumB += b
    histR[r]++
    histG[g]++
    histB[b]++
  }

  function median(hist) {
    const half = Math.floor(pixelCount / 2)
    let cum = 0
    for (let v = 0; v < 256; v++) {
      cum += hist[v]
      if (cum > half) return v
    }
    return 255
  }

  return {
    min: { r: minR, g: minG, b: minB },
    max: { r: maxR, g: maxG, b: maxB },
    avg: {
      r: Math.round(sumR / pixelCount),
      g: Math.round(sumG / pixelCount),
      b: Math.round(sumB / pixelCount),
    },
    med: {
      r: median(histR),
      g: median(histG),
      b: median(histB),
    },
    histR: Array.from(histR),
    histG: Array.from(histG),
    histB: Array.from(histB),
    count: pixelCount,
  }
}

self.onmessage = function(e) {
  const { type, id } = e.data

  if (type === 'process') {
    const { buffer, width, height, filter, needsDisplay, dispOpts, needsStats } = e.data
    let data = new Uint8ClampedArray(buffer)

    // Apply gradient filter with normalization
    if (filter === 'gradient') {
      data = applyGradientFilter(data, width, height,
        dispOpts.autoNormGradient, dispOpts.refGradient)
    }

    // Apply display options (brightness, contrast, gamma, RGB channels)
    if (needsDisplay) {
      data = applyDisplayOptions(data, dispOpts)
    }

    // Compute statistics if needed
    let stats = null
    if (needsStats) {
      stats = computeStatistics(data, width, height)
    }

    // Transfer buffer back
    const resultBuffer = data.buffer
    self.postMessage({ type: 'result', id, buffer: resultBuffer, stats, width, height }, [resultBuffer])
  }
}
