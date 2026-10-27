// prepack.js: checks the version names a release tag, records the release source tarball's SHA-256, and copies the licence files in.
'use strict';

const fs = require('fs');
const path = require('path');
const crypto = require('crypto');
const { spawnSync } = require('child_process');
const { product, SOURCE_URL, SOURCE_CHECKSUM_FILE } = require('./install');

function fail(message) {
  console.error(message);
  process.exit(1);
}

async function main() {
  const repo = path.join(__dirname, '..', '..');
  const tagged = spawnSync('git', ['show', `refs/tags/${product.version}:meson.build`], { cwd: repo, encoding: 'utf8' });
  if (tagged.status !== 0) {
    fail(`package.json is ${product.version}, but there is no ${product.version} release tag; npm versions track published releases`);
  }
  const taggedVersion = tagged.stdout.match(/^\s*version\s*:\s*'([^']+)'/m)[1];
  if (taggedVersion !== product.version) fail(`tag ${product.version} builds version ${taggedVersion}`);

  const response = await fetch(SOURCE_URL);
  if (!response.ok) fail(`GET ${SOURCE_URL}: HTTP ${response.status}`);
  const sha256 = crypto.createHash('sha256').update(Buffer.from(await response.arrayBuffer())).digest('hex');
  fs.writeFileSync(SOURCE_CHECKSUM_FILE, `${JSON.stringify({ version: product.version, sha256 }, null, 2)}\n`);
  console.error(`source tarball ${product.version}: sha256 ${sha256}`);

  for (const file of product.licenseFiles) {
    fs.copyFileSync(path.join(repo, file), path.join(__dirname, '..', file));
  }
}

main().catch((error) => fail(error.stack));
