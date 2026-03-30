// Stub: no-op capture until the native addon is built (Phase 3)

export function createCapture() {
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
