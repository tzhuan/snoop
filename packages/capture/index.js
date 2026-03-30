import { createRequire } from 'module';
const require = createRequire(import.meta.url);

// Backend names in order of preference.
// On Linux: pipewire first (Wayland + GNOME X11), then x11 (XShm fallback).
// On Windows/macOS: single snoop_capture binary.
// Set SNOOP_CAPTURE=x11|pipewire to force a specific backend.
const ALL_BACKENDS = [
  'snoop_capture_pipewire',
  'snoop_capture_x11',
  'snoop_capture',
];
const FORCE_BACKEND = process.env.SNOOP_CAPTURE;
const BACKEND_NAMES = FORCE_BACKEND
  ? [`snoop_capture_${FORCE_BACKEND}`]
  : ALL_BACKENDS;

function loadBackend(name) {
  try {
    return require(`./build/Release/${name}.node`);
  } catch {
    return null;
  }
}

const noopCapture = {
  onFrame(_callback) {},
  start() { return -1; },
  stop() {},
  setRegion(_x, _y, _w, _h) {},
  setRate(_fps) {},
  suspend() {},
  resume() {},
  snap() {},
};

export function createCapture() {
  let active = null;
  let onFrameCallback = null;

  return {
    onFrame(callback) {
      onFrameCallback = callback;
      if (active) active.onFrame(callback);
    },
    start() {
      // Try each backend lazily — only load the next if the previous fails
      for (const name of BACKEND_NAMES) {
        const backend = loadBackend(name);
        if (!backend) continue;

        const cap = backend.createCapture();
        if (onFrameCallback) cap.onFrame(onFrameCallback);
        const ret = cap.start();
        if (ret === 0) {
          active = cap;
          return 0;
        }
        // Start failed — clean up and try next
        cap.stop();
      }
      return -1;
    },
    stop() { if (active) active.stop(); },
    setRegion(x, y, w, h) { if (active) active.setRegion(x, y, w, h); },
    setRate(fps) { if (active) active.setRate(fps); },
    suspend() { if (active) active.suspend(); },
    resume() { if (active) active.resume(); },
    snap() { if (active) active.snap(); },
  };
}
