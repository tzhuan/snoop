import process from 'node:process';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';
import path from 'node:path';

const require = createRequire(import.meta.url);
const __dirname = path.dirname(fileURLToPath(import.meta.url));

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

// Try prebuilt binary first (prebuilds/<platform>-<arch>/), then build/Release/
function loadBackend(name) {
  const platform = `${process.platform}-${process.arch}`;
  const paths = [
    path.join(__dirname, 'prebuilds', platform, `${name}.node`),
    path.join(__dirname, 'build', 'Release', `${name}.node`),
  ];
  for (const p of paths) {
    try {
      return require(p);
    } catch {
      // Not available at this path
    }
  }
  return null;
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
  setDisplay(_id) { return -1; },
  listDisplays() { return []; },
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
    setDisplay(displayId) {
      if (active) return active.setDisplay(displayId);
      return -1;
    },
    listDisplays() {
      if (active) return active.listDisplays();
      return [];
    },
    /** Stop → setDisplay → re-register callback → start */
    switchDisplay(displayId) {
      if (!active) return -1;
      active.stop();
      active.setDisplay(displayId);
      if (onFrameCallback) active.onFrame(onFrameCallback);
      return active.start();
    },
    /** Stop → re-register callback → start (e.g. for X11 XShm hotplug) */
    restart() {
      if (!active) return -1;
      active.stop();
      if (onFrameCallback) active.onFrame(onFrameCallback);
      return active.start();
    },
  };
}
