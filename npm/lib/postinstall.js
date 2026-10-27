// postinstall.js: builds the browser when the npm package is installed, so the first launch is immediate.
'use strict';

const { install, product, InstallError, log } = require('./install');

const skip = `${product.envPrefix}_SKIP_INSTALL`;

if (process.env[skip] === '1') {
  log(`${skip}=1: the browser is built on first launch instead`);
} else {
  install().catch((error) => {
    log(error instanceof InstallError ? error.message : error.stack);
    process.exit(1);
  });
}
