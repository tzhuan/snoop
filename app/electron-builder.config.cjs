// Resolve electron version from wherever npm installed it (root or app node_modules)
let electronVersion;
try {
  electronVersion = require('electron/package.json').version;
} catch {
  // Fallback: read from root node_modules when running in workspace
  electronVersion = require('../node_modules/electron/package.json').version;
}

module.exports = {
  appId: 'com.tzhuan.snoop',
  productName: 'Snoop',
  electronVersion,

  directories: {
    output: 'out',
  },

  asar: true,

  // We build native addons with cmake-js and rebuild for Electron manually.
  npmRebuild: false,

  // Consistent naming: always include platform and arch
  artifactName: '${name}-${version}-${os}-${arch}.${ext}',

  files: [
    'src/**/*',
    'assets/**/*',
    'package.json',
    'node_modules/**/*',
  ],

  // Native .node addons must be outside asar (can't be dlopen'd from inside)
  asarUnpack: [
    'node_modules/@snoop/*/prebuilds/**',
    'node_modules/@snoop/*/build/**',
  ],

  mac: {
    category: 'public.app-category.utilities',
    icon: 'assets/icon.icns',
    identity: null,
    target: ['dmg', 'zip'],
  },

  dmg: {
    title: 'Snoop',
  },

  win: {
    icon: 'assets/icon.ico',
    target: ['zip', 'nsis'],
  },

  nsis: {
    oneClick: true,
    perMachine: false,
  },

  linux: {
    icon: 'assets',
    category: 'Utility',
    target: ['AppImage', 'deb', 'rpm', 'zip'],
  },

  deb: {
    depends: ['libx11-6', 'libxext6'],
  },

  rpm: {
    depends: ['libX11', 'libXext'],
  },

  publish: {
    provider: 'github',
    owner: 'tzhuan',
    repo: 'snoop',
  },
};
