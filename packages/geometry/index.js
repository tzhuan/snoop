import process from 'node:process';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';
import path from 'node:path';

const require = createRequire(import.meta.url);
const __dirname = path.dirname(fileURLToPath(import.meta.url));
const platform = `${process.platform}-${process.arch}`;

let native = null;
for (const p of [
  path.join(__dirname, 'prebuilds', platform, 'snoop_geometry.node'),
  path.join(__dirname, 'build', 'Release', 'snoop_geometry.node'),
]) {
  try { native = require(p); break; } catch {}
}

export function getActiveWindow() {
  return native ? native.getActiveWindow() : null;
}

export function getCursorPosition() {
  return native ? native.getCursorPosition() : null;
}

export function setCursorPosition(x, y) {
  return native ? native.setCursorPosition(x, y) : false;
}
