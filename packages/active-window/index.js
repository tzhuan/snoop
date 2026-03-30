// Shim: re-export active-win until the native addon is built
import activeWin from 'active-win';

export async function getActiveWindow() {
  const win = await activeWin();
  if (!win) return null;
  return {
    x: win.bounds.x,
    y: win.bounds.y,
    width: win.bounds.width,
    height: win.bounds.height,
    title: win.title,
  };
}

export function getCursorPosition() {
  // Stub — Phase 2 will implement native cursor position
  return null;
}
