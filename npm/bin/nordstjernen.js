#!/usr/bin/env node
// nordstjernen.js: the `nordstjernen` command, which installs the browser if needed and runs it with the given arguments.
'use strict';

const { spawn } = require('child_process');
const { install, InstallError, log } = require('../lib/install');

async function main() {
  const { exe, env } = await install();
  const child = spawn(exe, process.argv.slice(2), { stdio: 'inherit', env });
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
