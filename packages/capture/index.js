import { createRequire } from 'module';
const require = createRequire(import.meta.url);

let native = null;
try {
  native = require('./build/Release/snoop_capture.node');
} catch {
  // Native addon not built — fall through to no-op stub
}

export function createCapture() {
  if (native) {
    return native.createCapture();
  }
  // No-op fallback
  return {
    onFrame(_callback) {},
    start() {},
    stop() {},
    setRegion(_x, _y, _w, _h) {},
    setRate(_fps) {},
    suspend() {},
    resume() {},
    snap() {},
  };
}
