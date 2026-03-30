import { createRequire } from 'module';
const require = createRequire(import.meta.url);

let native = null;
try {
  native = require('./build/Release/snoop_active_window.node');
} catch {
  // Native addon not built — fall through to active-win shim
}

export function getActiveWindow() {
  if (native) {
    return native.getActiveWindow();
  }
  return null;
}

export function getCursorPosition() {
  if (native) {
    return native.getCursorPosition();
  }
  return null;
}
