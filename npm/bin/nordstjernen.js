#!/usr/bin/env node
// nordstjernen.js: the `nordstjernen` command, which installs the browser if needed and runs it with the given arguments.
'use strict';

const { spawn, spawnSync } = require('child_process');
const { install, installPlan, storeMessage, STORE_URL, InstallError, log } = require('../lib/install');

function openStore() {
  log(storeMessage());
  spawnSync('cmd', ['/c', 'start', '', STORE_URL], { stdio: 'ignore' });
  process.exit(1);
}

async function main() {
  if (installPlan().kind === 'store') openStore();
  const exe = await install();
  const child = spawn(exe, process.argv.slice(2), { stdio: 'inherit' });
  for (const signal of ['SIGINT', 'SIGTERM', 'SIGHUP']) {
    process.on(signal, () => child.kill(signal));
  }
  child.on('exit', (code, signal) => {
    if (signal) process.kill(process.pid, signal);
    else process.exit(code ?? 0);
  });
}

main().catch((error) => {
  log(error instanceof InstallError ? error.message : error.stack);
  process.exit(1);
});
