// prepack.js: refuses to pack a version that has no release tag, and copies the licence files into the package.
'use strict';

const fs = require('fs');
const path = require('path');
const { spawnSync } = require('child_process');
const { version } = require('../package.json');

const repo = path.join(__dirname, '..', '..');
const tagged = spawnSync('git', ['show', `refs/tags/${version}:meson.build`], { cwd: repo, encoding: 'utf8' });
if (tagged.status !== 0) {
  console.error(`package.json is ${version}, but there is no ${version} release tag; npm versions track published releases`);
  process.exit(1);
}
const taggedVersion = tagged.stdout.match(/^\s*version\s*:\s*'([^']+)'/m)[1];
if (taggedVersion !== version) {
  console.error(`tag ${version} builds Nordstjernen ${taggedVersion}`);
  process.exit(1);
}
for (const file of ['License.md', 'COPYING']) {
  fs.copyFileSync(path.join(repo, file), path.join(__dirname, '..', file));
}
