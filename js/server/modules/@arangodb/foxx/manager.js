'use strict';

// //////////////////////////////////////////////////////////////////////////////
// / @brief Foxx service manager
// /
// / @file
// /
// / DISCLAIMER
// /
// / Copyright 2013 triagens GmbH, Cologne, Germany
// /
// / Licensed under the Apache License, Version 2.0 (the "License")
// / you may not use this file except in compliance with the License.
// / You may obtain a copy of the License at
// /
// /     http://www.apache.org/licenses/LICENSE-2.0
// /
// / Unless required by applicable law or agreed to in writing, software
// / distributed under the License is distributed on an "AS IS" BASIS,
// / WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// / See the License for the specific language governing permissions and
// / limitations under the License.
// /
// / Copyright holder is triAGENS GmbH, Cologne, Germany
// /
// / @author Dr. Frank Celler
// / @author Michael Hackstein
// / @author Copyright 2013, triAGENS GmbH, Cologne, Germany
// //////////////////////////////////////////////////////////////////////////////

const fs = require('fs');
const path = require('path');
const dd = require('dedent');
const utils = require('@arangodb/foxx/manager-utils');
const store = require('@arangodb/foxx/store');
const FoxxService = require('@arangodb/foxx/service');
const ensureServiceExecuted = require('@arangodb/foxx/routing').routeService;
const arangodb = require('@arangodb');
const ArangoError = arangodb.ArangoError;
const errors = arangodb.errors;
const aql = arangodb.aql;
const db = arangodb.db;
const ArangoClusterControl = require('@arangodb/cluster');
const request = require('@arangodb/request');
const actions = require('@arangodb/actions');
const isZipBuffer = require('@arangodb/util').isZipBuffer;
const codeFrame = require('@arangodb/util').codeFrame;
const internal  = require('internal');

const SYSTEM_SERVICE_MOUNTS = [
  '/_admin/aardvark', // Admin interface.
  '/_api/foxx' // Foxx management API.
];

// global (database-agnostic) map from { mount => service }
let INTERNAL_SERVICE_MAP = new Map();

// database-specific map from { db => { mount => service } }
const GLOBAL_SERVICE_MAP = new Map();

// Cluster helpers

function getAllCoordinatorIds () {
  if (!ArangoClusterControl.isCluster()) {
    return [];
  }
  return global.ArangoClusterInfo.getCoordinators();
}

function getMyCoordinatorId () {
  if (!ArangoClusterControl.isCluster()) {
    return null;
  }
  return global.ArangoServerState.id();
}

function isFoxxmaster () {
  return global.ArangoServerState.isFoxxmaster();
}

function validateInstallUrl (url) {
  if (!internal.foxxAllowInstallFromRemote()) {
    // check if a user-defined install baseurl exists
    let baseUrl = require('process').env.FOXX_BASE_URL;
    let invalid = false;
    if (baseUrl) {
      if (!url.startsWith(baseUrl)) {
        // install url does not start with FOXX_BASE_URL
        invalid = true;
      }
    } else {
      const checkRegex = /^https?:\/\/([^:\.]+:[^@\.]*@)?(www\.)?github\.com\//i;
      invalid = !checkRegex.test(url);
    }

    if (invalid) {
      throw new ArangoError({
        errorNum: errors.ERROR_FORBIDDEN.code,
        errorMessage: dd`
          ${errors.ERROR_FORBIDDEN.message}
          Installing apps from remote URLs is disabled
        `
      });
    }
  }
}

// Startup and self-heal

function selfHealAll (skipReloadRouting) {
  const db = require('internal').db;
  const dbName = db._name();
  let modified = false;
  try {
    db._useDatabase('_system');
    const databases = db._databases();
    for (const name of databases) {
      try {
        db._useDatabase(name);
        modified = selfHeal() || modified;
      } catch (e) {
        console.debugStack(e);
      }
      if (global.KEYSPACE_EXISTS('FoxxFirstSelfHeal')) {
        // drop the keyspace that indicates we still need to run an initial
        // self-heal in this database
        try {
          global.KEYSPACE_DROP('FoxxFirstSelfHeal');
        } catch (err) {
          // potential races can be ignored here, as long as the key space
          // is gone in the end
        }
      }
    }
  } finally {
    db._useDatabase(dbName);
    if (modified && !skipReloadRouting) {
      reloadRouting();
    }
  }
}

function triggerSelfHeal () {
  const modified = selfHeal();
  if (modified) {
    reloadRouting();
  }
}

function selfHeal () {
  const dirname = FoxxService.rootBundlePath();
  if (!fs.exists(dirname)) {
    fs.makeDirectoryRecursive(dirname);
  }

  const bundleCollection = utils.getBundleStorage();
  const serviceDefinitions = db._query(aql`/*selfHeal*/ FOR doc IN _apps
    FILTER LEFT(doc.mount, 2) != "/_"
    LET bundleExists = DOCUMENT(_appbundles, doc.checksum) != null
    RETURN [doc.mount, doc.checksum, doc._rev, bundleExists]`).toArray();

  let modified = false;
  const knownBundlePaths = new Array(serviceDefinitions.length);
  const knownServicePaths = new Array(serviceDefinitions.length);
  if (!GLOBAL_SERVICE_MAP.has(db._name())) {
    initLocalServiceMap();
  }
  const localServiceMap = GLOBAL_SERVICE_MAP.get(db._name());
  for (const [mount, checksum, rev, bundleExists] of serviceDefinitions) {
    const bundlePath = FoxxService.bundlePath(mount);
    const basePath = FoxxService.basePath(mount);
    knownBundlePaths.push(bundlePath);
    knownServicePaths.push(basePath);

    if (!localServiceMap.has(mount) || localServiceMap.get(mount)._rev !== rev) {
      modified = true;
    }

    const hasBundle = fs.exists(bundlePath);
    const hasFolder = fs.exists(basePath);
    if (!hasBundle && hasFolder) {
      createServiceBundle(mount);
    } else if (hasBundle && !hasFolder) {
      extractServiceBundle(bundlePath, basePath);
      modified = true;
    }
    const isInstalled = hasBundle || hasFolder;
    const localChecksum = isInstalled ? safeChecksum(mount) : null;

    if (!checksum) {
      // pre-3.2 service, can't self-heal this
      continue;
    }

    if (checksum === localChecksum) {
      if (!bundleExists) {
        try {
          bundleCollection._binaryInsert({_key: checksum}, bundlePath);
        } catch (e) {
          console.warnStack(e, `Failed to store missing service bundle for service at "${mount}"`);
        }
      }
    } else if (bundleExists) {
      try {
        if (fs.exists(bundlePath)) {
          fs.remove(bundlePath);
        }
        bundleCollection._binaryDocument(checksum, bundlePath);
        extractServiceBundle(bundlePath, basePath);
        modified = true;
      } catch (e) {
        console.errorStack(e, `Failed to load service bundle for service at "${mount}"`);
      }
    }
  }

  modified = cleanupOrphanedServices(
    knownServicePaths,
    knownBundlePaths
  ) || modified;

  return modified;
}

function cleanupOrphanedServices (knownServicePaths, knownBundlePaths) {
  let modified = false;

  function traverseServices (basePath) {
    if (!fs.isDirectory(basePath)) {
      return;
    }
    for (const relPath of fs.list(basePath)) {
      const absPath = path.resolve(basePath, relPath);
      if (relPath.toUpperCase() !== 'APP') {
        traverseServices(absPath);
      } else if (!knownServicePaths.includes(absPath)) {
        modified = true;
        try {
          fs.removeDirectoryRecursive(absPath, true);
          console.debug(`Deleted orphaned service folder ${absPath}`);
        } catch (e) {
          console.warnStack(e, `Failed to delete orphaned service folder ${absPath}`);
        }
      }
    }
  }

  const servicesRoot = FoxxService.rootPath();
  for (const name of fs.list(servicesRoot)) {
    if (name === '_appbundles') {
      continue;
    }
    traverseServices(path.resolve(servicesRoot, name));
  }

  const bundlesRoot = FoxxService.rootBundlePath();
  for (const relPath of fs.list(bundlesRoot)) {
    const absPath = path.resolve(bundlesRoot, relPath);
    if (!knownBundlePaths.includes(absPath)) {
      try {
        fs.remove(absPath);
        console.debug(`Deleted orphaned service bundle ${absPath}`);
      } catch (e) {
        console.warnStack(e, `Failed to delete orphaned service bundle ${absPath}`);
      }
    }
  }

  return modified;
}

function startup () {
  if (global.ArangoServerState.role() === 'SINGLE') {
    commitLocalState(true);
    // execute self-heal as part of startup process. this can take a while,
    // but as all queries can run locally, it should always make progress
    selfHealAll();
  } else {
    let offset = 1; // start selfheal in x seconds
    const period = 5 * 60; // repeat sealfheal all x seconds
    if (global.FOXX_STARTUP_WAIT_FOR_SELF_HEAL) {
      // Enforce a selfheal now.
      selfHealAll();
      // we just did a selfheal, can delay the first automatic one
      offset = period;
    } else {
      // for all databases that exist at startup, create a per-database 
      // keyspace that indicates we still need to run the initial self-heal in it
      let dbName = db._name();
      try {
        db._databases().forEach(function(database) {
          db._useDatabase(dbName);
          global.KEYSPACE_CREATE('FoxxFirstSelfHeal', 1, true);
        });
      } finally {
        db._useDatabase(dbName);
      }
    }
    
    // in a cluster, move the initial self-heal job to a background thread,
    // so that we do not block on startup
    try {
      require('@arangodb/tasks').register({
        id: 'self-heal',
        isSystem: true,
        offset,
        period,
        command: function () {
          const FoxxManager = require('@arangodb/foxx/manager');
          FoxxManager.healAll();
        }
      });
    } catch (ee) {
      if (ee.errorNum !== errors.ERROR_TASK_DUPLICATE_ID.code) {
        // a "duplicate task id" error is actually allowed here, because
        // the bootstrap function may be called repeatedly by the BootstrapFeature
        throw ee;
      }
    }
  }
}

function commitLocalState (replace) {
  let modified = false;
  const rootPath = FoxxService.rootPath();
  const collection = utils.getStorage();
  const bundleCollection = utils.getBundleStorage();
  for (const relPath of fs.listTree(rootPath)) {
    if (!relPath) {
      continue;
    }
    const basename = path.basename(relPath);
    if (basename.toUpperCase() !== 'APP') {
      continue;
    }
    const mount = '/' + path.dirname(relPath).split(path.sep).join('/');
    const basePath = FoxxService.basePath(mount);
    if (!fs.list(basePath).length) {
      continue;
    }
    const bundlePath = FoxxService.bundlePath(mount);
    if (!fs.exists(bundlePath)) {
      createServiceBundle(mount, bundlePath);
    }
    const serviceDefinition = db._query(aql`
      FOR service IN ${collection}
      FILTER service.mount == ${mount}
      RETURN service
    `).next();
    if (!serviceDefinition) {
      try {
        const service = FoxxService.create({mount});
        service.updateChecksum();
        if (!bundleCollection.exists(service.checksum)) {
          bundleCollection._binaryInsert({_key: service.checksum}, bundlePath);
        }
        collection.save(service.toJSON());
        modified = true;
      } catch (e) {
        console.errorStack(e);
      }
    } else {
      const checksum = safeChecksum(mount);
      if (!serviceDefinition.checksum || (replace && serviceDefinition.checksum !== checksum)) {
        if (!bundleCollection.exists(checksum)) {
          bundleCollection._binaryInsert({_key: checksum}, bundlePath);
        }
        collection.update(serviceDefinition._key, {checksum});
        modified = true;
      } else if (serviceDefinition.checksum === checksum) {
        if (!bundleCollection.exists(checksum)) {
          bundleCollection._binaryInsert({_key: checksum}, bundlePath);
          modified = true;
        }
      }
    }
  }
  if (modified) {
    propagateSelfHeal();
    reloadRouting();
  }
}

// Change propagation

function reloadRouting () {
  global.SYS_EXECUTE_GLOBAL_CONTEXT_FUNCTION('reloadRouting');
  actions.reloadRouting();
}

function propagateSelfHeal () {
  global.SYS_PROPAGATE_SELF_HEAL();
}

// INTERNAL_SERVICE_MAP manipulation

// initialize /_admin/aardvark and /_api/foxx
function initInternalServiceMap () {
  if (INTERNAL_SERVICE_MAP.size !== 0) {
    return;
  }
  const internalServiceMap = new Map();
  // initialize /_admin/aardvark and /_api/foxx
  for (const mount of SYSTEM_SERVICE_MOUNTS) {
    try {
      const serviceDefinition = {mount};
      const service = FoxxService.create(serviceDefinition);
      internalServiceMap.set(mount, service);
    } catch (e) {
      console.errorStack(e);
    }
  }
  // apply all changes at once
  INTERNAL_SERVICE_MAP = internalServiceMap;
}

// GLOBAL_SERVICE_MAP manipulation

function initLocalServiceMap () {
  // initialize /_admin/aardvark and /_api/foxx
  // this will populate INTERNAL_SERVICE_MAP if needed
  initInternalServiceMap();

  const localServiceMap = new Map();
  // copy internal services into map
  for (let [mount, service] of INTERNAL_SERVICE_MAP) {
    localServiceMap.set(mount, service);
  }

  for (const serviceDefinition of utils.getStorage().all()) {
    try {
      const service = loadInstalledService(serviceDefinition);
      localServiceMap.set(serviceDefinition.mount, service);
    } catch (e) {
      localServiceMap.set(serviceDefinition.mount, {error: e});
    }
  }
  GLOBAL_SERVICE_MAP.set(db._name(), localServiceMap);
}

function ensureFoxxInitialized (internalOnly) {
  // initialize /_admin/aardvark and /_api/foxx
  // this will populate INTERNAL_SERVICE_MAP if needed
  initInternalServiceMap();

  if (internalOnly) {
    // all we need to know must be in the internal service map
    return;
  }

  if (!GLOBAL_SERVICE_MAP.has(db._name())) {
    if (global.KEYSPACE_EXISTS('FoxxFirstSelfHeal')) {
      // still waiting for initial selfHeal to execute
      throw new ArangoError({
        errorNum: errors.ERROR_HTTP_SERVICE_UNAVAILABLE.code,
        errorMessage: "waiting for initialization of Foxx services in this database"
      });
    }

    initLocalServiceMap();
  }
}

function ensureServiceLoaded (mount) {
  const service = getServiceInstance(mount);
  return ensureServiceExecuted(service, false);
}

function getServiceInstance (mount) {
  const internalOnly = SYSTEM_SERVICE_MOUNTS.indexOf(mount) !== -1;
  ensureFoxxInitialized(internalOnly);

  let localServiceMap;
  if (internalOnly) {
    localServiceMap = INTERNAL_SERVICE_MAP;
  } else {
    localServiceMap = GLOBAL_SERVICE_MAP.get(db._name());
  }
  let service;
  if (localServiceMap.has(mount)) {
    service = localServiceMap.get(mount);
  } else if (!internalOnly) {
    service = reloadInstalledService(mount);
  }
  if (!service) {
    throw new ArangoError({
      errorNum: errors.ERROR_SERVICE_NOT_FOUND.code,
      errorMessage: dd`
        ${errors.ERROR_SERVICE_NOT_FOUND.message}
        Mount path: "${mount}".
      `
    });
  }
  if (service.error) {
    throw service.error;
  }
  return service;
}

function reloadInstalledService (mount, runSetup) {
  const serviceDefinition = utils.getServiceDefinition(mount);
  if (!serviceDefinition) {
    throw new ArangoError({
      errorNum: errors.ERROR_SERVICE_NOT_FOUND.code,
      errorMessage: dd`
        ${errors.ERROR_SERVICE_NOT_FOUND.message}
        Mount path: "${mount}".
      `
    });
  }
  const service = loadInstalledService(serviceDefinition);
  if (runSetup) {
    service.executeScript('setup');
  }
  if (!GLOBAL_SERVICE_MAP.has(db._name())) {
    initLocalServiceMap();
  }
  GLOBAL_SERVICE_MAP.get(db._name()).set(service.mount, service);
  return service;
}

function loadInstalledService (serviceDefinition) {
  const mount = serviceDefinition.mount;
  if (!mount.startsWith('/_')) {
    if (
      !fs.exists(FoxxService.bundlePath(mount)) ||
      !fs.exists(FoxxService.basePath(mount))
    ) {
      throw new ArangoError({
        errorNum: errors.ERROR_SERVICE_FILES_MISSING.code,
        errorMessage: dd`
          ${errors.ERROR_SERVICE_FILES_MISSING.message}
          Mount: ${mount}
        `
      });
    }
    const checksum = serviceDefinition.checksum;
    if (checksum && checksum !== safeChecksum(mount)) {
      throw new ArangoError({
        errorNum: errors.ERROR_SERVICE_FILES_OUTDATED.code,
        errorMessage: dd`
          ${errors.ERROR_SERVICE_FILES_OUTDATED.message}
          Mount: ${mount}
        `
      });
    }
  }
  return FoxxService.create(serviceDefinition);
}

// Misc?

function patchManifestFile (servicePath, patchData) {
  const filename = path.join(servicePath, 'manifest.json');
  let manifest;
  try {
    const rawManifest = fs.readFileSync(filename, 'utf-8');
    manifest = JSON.parse(rawManifest);
  } catch (e) {
    throw Object.assign(
      new ArangoError({
        errorNum: errors.ERROR_MALFORMED_MANIFEST_FILE.code,
        errorMessage: dd`
          ${errors.ERROR_MALFORMED_MANIFEST_FILE.message}
          File: ${filename}
        `
      }), {cause: e}
    );
  }
  Object.assign(manifest, patchData);
  fs.writeFileSync(filename, JSON.stringify(manifest, null, 2));
}

function _prepareService (serviceInfo, legacy = false) {
  const tempServicePath = utils.joinLastPath(fs.getTempFile('services', false));
  const tempBundlePath = utils.joinLastPath(fs.getTempFile('bundles', false));
  try {
    if (isZipBuffer(serviceInfo)) {
      // Buffer (zip)
      const tempFile = fs.getTempFile('bundles', false);
      fs.writeFileSync(tempFile, serviceInfo);
      extractServiceBundle(tempFile, tempServicePath);
      fs.move(tempFile, tempBundlePath);
    } else if (serviceInfo instanceof Buffer) {
      // Buffer (js)
      _buildServiceBundleFromScript(tempServicePath, tempBundlePath, serviceInfo);
    } else if (/^https?:/i.test(serviceInfo)) {
      // Remote path
      // check if we are allowed to install from this remote URL or not
      validateInstallUrl(serviceInfo);
      const tempFile = downloadServiceBundleFromRemote(serviceInfo);
      try {
        _buildServiceFromFile(tempServicePath, tempBundlePath, tempFile);
      } finally {
        fs.remove(tempFile);
      }
    } else if (fs.exists(serviceInfo)) {
      // Local path
      if (fs.isDirectory(serviceInfo)) {
        utils.zipDirectory(serviceInfo, tempBundlePath);
        extractServiceBundle(tempBundlePath, tempServicePath);
      } else {
        _buildServiceFromFile(tempServicePath, tempBundlePath, serviceInfo);
      }
    } else {
      const info = !internal.isFoxxApiDisabled() && store.installationInfo(serviceInfo);  //disable foxx store
      if (!info) {
        throw new ArangoError({
          errorNum: errors.ERROR_SERVICE_SOURCE_NOT_FOUND.code,
          errorMessage: dd`
            ${errors.ERROR_SERVICE_SOURCE_NOT_FOUND.message}
            Location: ${serviceInfo}
          `
        });
      }
      const storeBundle = downloadServiceBundleFromRemote(info.url);
      try {
        extractServiceBundle(storeBundle, tempServicePath);
      } finally {
        try {
          fs.remove(storeBundle);
        } catch (e) {
          console.warnStack(e, `Cannot remove temporary file "${storeBundle}"`);
        }
      }
      patchManifestFile(tempServicePath, info.manifest);
      utils.zipDirectory(tempServicePath, tempBundlePath);
    }
    if (legacy) {
      patchManifestFile(tempServicePath, {engines: {arangodb: '^2.8.0'}});
      if (fs.exists(tempBundlePath)) {
        fs.remove(tempBundlePath);
      }
      utils.zipDirectory(tempServicePath, tempBundlePath);
    }
    return {
      tempServicePath,
      tempBundlePath
    };
  } catch (e) {
    try {
      fs.removeDirectoryRecursive(tempServicePath, true);
    } catch (e2) {}
    throw e;
  }
}

function _buildServiceFromFile (tempServicePath, tempBundlePath, filePath) {
  try {
    extractServiceBundle(filePath, tempServicePath);
  } catch (e) {
    _buildServiceBundleFromScript(tempServicePath, tempBundlePath, fs.readFileSync(filePath));
    return;
  }
  fs.copyFile(filePath, tempBundlePath);
}

function _buildServiceBundleFromScript (tempServicePath, tempBundlePath, jsBuffer) {
  const manifest = JSON.stringify({main: 'index.js'}, null, 4);
  fs.makeDirectoryRecursive(tempServicePath);
  fs.writeFileSync(path.join(tempServicePath, 'index.js'), jsBuffer);
  fs.writeFileSync(path.join(tempServicePath, 'manifest.json'), manifest);
  utils.zipDirectory(tempServicePath, tempBundlePath);
}

function _deleteServiceFromPath (mount, force) {
}

// @brief Save foxx service to the database, i.e. _apps and _appbundles.
//
// Uses the service and bundle files from tempPaths instead of
// FoxxService.basePath(mount) and FoxxService.bundlePath(mount). This is to
// avoid a race condition with selfHeal() which could delete the files before
// the service is saved to the database.
function _install (tempService, tempBundlePath, options = {}) {
  try {
    if (options.setup !== false) {
      try {
        tempService.executeScript('setup');
      } catch (e) {
        if (!options.force) {
          e.codeFrame = codeFrame(e, tempService.basePath);
          throw e;
        } else {
          console.warnStack(e);
        }
      }
    }
    // instead of service.updateChecksum(), update the checksum
    // manually from the temporary path.
    tempService.checksum = FoxxService._checksumPath(tempBundlePath);
    try {
      ensureServiceExecuted(tempService, true);
    } catch (e) {
      if (!options.force) {
        e.codeFrame = codeFrame(e, tempService.basePath);
        throw e;
      } else {
        console.warnStack(e);
      }
    }
    const bundleCollection = utils.getBundleStorage();
    if (!bundleCollection.exists(tempService.checksum)) {
      bundleCollection._binaryInsert({_key: tempService.checksum}, tempBundlePath);
    }
    const serviceDefinition = tempService.toJSON();
    const collection = utils.getStorage();
    db._query(aql`
      UPSERT {mount: ${tempService.mount}}
      INSERT ${serviceDefinition}
      REPLACE ${serviceDefinition}
      IN ${collection}
    `);
  } finally {
    fs.remove(tempBundlePath);
    fs.removeDirectoryRecursive(tempService.basePath);
  }
}

function _uninstall (mount, options = {}) {
  let service;
  try {
    service = getServiceInstance(mount);
    if (service.error) {
      const error = service.error;
      service = null;
      throw error;
    }
  } catch (e) {
    if (!options.force) {
      throw e;
    }
  }
  if (service && options.teardown !== false) {
    try {
      service.executeScript('teardown');
    } catch (e) {
      if (!options.force) {
        throw e;
      }
      console.warnStack(e);
    }
  }
  const collection = utils.getStorage();
  db._query(aql`
    FOR service IN ${collection}
    FILTER service.mount == ${mount}
    REMOVE service IN ${collection}
  `);
  if (!GLOBAL_SERVICE_MAP.has(db._name())) {
    initLocalServiceMap();
  }
  GLOBAL_SERVICE_MAP.get(db._name()).delete(mount);
  const servicePath = FoxxService.basePath(mount);
  if (fs.exists(servicePath)) {
    try {
      fs.removeDirectoryRecursive(servicePath, true);
    } catch (e) {
      if (!options.force) {
        throw e;
      }
      console.warnStack(e);
    }
  }
  const bundlePath = FoxxService.bundlePath(mount);
  if (fs.exists(bundlePath)) {
    try {
      fs.remove(bundlePath);
    } catch (e) {
      if (!options.force) {
        throw e;
      }
      console.warnStack(e);
    }
  }
  return service;
}

// Service bundle manipulation

function createServiceBundle (mount, bundlePath = FoxxService.bundlePath(mount)) {
  const servicePath = FoxxService.basePath(mount);
  if (fs.exists(bundlePath)) {
    fs.remove(bundlePath);
  }
  fs.makeDirectoryRecursive(path.dirname(bundlePath));
  utils.zipDirectory(servicePath, bundlePath);
}

function downloadServiceBundleFromRemote (url) {
  try {
    const res = request.get(url, {encoding: null});
    if (res.json && res.json.errorNum) {
      throw new ArangoError(res.json);
    }
    res.throw();
    const tempFile = fs.getTempFile('bundles', false);
    fs.writeFileSync(tempFile, res.body);
    return tempFile;
  } catch (e) {
    throw Object.assign(
      new ArangoError({
        errorNum: errors.ERROR_SERVICE_SOURCE_ERROR.code,
        errorMessage: dd`
          ${errors.ERROR_SERVICE_SOURCE_ERROR.message}
          URL: ${url}
        `
      }),
      {cause: e}
    );
  }
}

function extractServiceBundle (archive, targetPath) {
  const tempFolder = utils.joinLastPath(fs.getTempFile('services', false));
  fs.makeDirectory(tempFolder);
  fs.unzipFile(archive, tempFolder, false, true);

  let manifestPath;
  // find the manifest with the shortest path
  const filenames = fs.listTree(tempFolder).sort((a, b) => a.length - b.length);
  for (const filename of filenames) {
    if (filename === 'manifest.json' || filename.endsWith('/manifest.json') || filename.endsWith('\\manifest.json')) {
      manifestPath = filename;
      break;
    }
  }

  if (!manifestPath) {
    throw new ArangoError({
      errorNum: errors.ERROR_SERVICE_MANIFEST_NOT_FOUND.code,
      errorMessage: dd`
        ${errors.ERROR_SERVICE_MANIFEST_NOT_FOUND.message}
        Source: ${tempFolder}
      `
    });
  }

  let basePath = path.dirname(path.resolve(tempFolder, manifestPath));
  if (fs.exists(targetPath)) {
    fs.removeDirectoryRecursive(targetPath, true);
  }
  fs.move(basePath, targetPath);

  if (manifestPath.endsWith('/manifest.json') || manifestPath.endsWith('\\manifest.json')) {
    // service basePath is a subfolder of tempFolder
    // so tempFolder still exists and needs to be removed
    fs.removeDirectoryRecursive(tempFolder, true);
  }
}

// Exported functions for manipulating services

function install (serviceInfo, mount, options = {}) {
  utils.validateMount(mount);
  ensureFoxxInitialized();
  if (utils.getServiceDefinition(mount)) {
    throw new ArangoError({
      errorNum: errors.ERROR_SERVICE_MOUNTPOINT_CONFLICT.code,
      errorMessage: dd`
        ${errors.ERROR_SERVICE_MOUNTPOINT_CONFLICT.message}
        Mount path: "${mount}".
      `
    });
  }
  const tempPaths = _prepareService(serviceInfo, Boolean(options.legacy));
  const tempService = FoxxService.create({
    mount,
    basePath: tempPaths.tempServicePath,
    noisy: true,
    options: {
      development: Boolean(options.development),
      configuration: options.configuration,
      dependencies: options.dependencies
    }
  });
  _install(tempService, tempPaths.tempBundlePath, {
    setup: options.setup === undefined || Boolean(options.setup),
    force: Boolean(options.force)
  });
  selfHeal();
  propagateSelfHeal();
  reloadRouting();

  return getServiceInstance(mount);
}

function uninstall (mount, options = {}) {
  ensureFoxxInitialized();
  const oldService = _uninstall(mount, options);
  selfHeal();
  propagateSelfHeal();
  reloadRouting();
  return oldService;
}

function replace (serviceInfo, mount, options = {}) {
  ensureFoxxInitialized();
  if (!options.force) {
    const serviceDefinition = utils.getServiceDefinition(mount);
    if (!serviceDefinition) {
      throw new ArangoError({
        errorNum: errors.ERROR_SERVICE_NOT_FOUND.code,
        errorMessage: dd`
          ${errors.ERROR_SERVICE_NOT_FOUND.message}
          Mount path: "${mount}".
        `
      });
    }
  } else {
    utils.validateMount(mount);
  }
  const tempPaths = _prepareService(serviceInfo, Boolean(options.legacy));
  const tempService = FoxxService.create({
    mount,
    basePath: tempPaths.tempServicePath,
    noisy: true,
    options: {
      development: Boolean(options.development),
      configuration: options.configuration,
      dependencies: options.dependencies
    }
  });
  _uninstall(mount, {
    teardown: options.teardown === undefined || Boolean(options.teardown),
    force: true
  });
  _install(tempService, tempPaths.tempBundlePath, {
    setup: options.setup === undefined || Boolean(options.setup),
    force: true
  });
  selfHeal();
  propagateSelfHeal();
  reloadRouting();

  return getServiceInstance(mount);
}

function upgrade (serviceInfo, mount, options = {}) {
  ensureFoxxInitialized();
  const serviceDefinition = utils.getServiceDefinition(mount);
  let old = {};
  if (serviceDefinition) {
    old = serviceDefinition.options;
  } else {
    if (!options.force) {
      throw new ArangoError({
        errorNum: errors.ERROR_SERVICE_NOT_FOUND.code,
        errorMessage: dd`
          ${errors.ERROR_SERVICE_NOT_FOUND.message}
          Mount path: "${mount}".
        `
      });
    }
    utils.validateMount(mount);
  }

  const tempPaths = _prepareService(serviceInfo, Boolean(options.legacy));
  const tempService = FoxxService.create({
    mount,
    basePath: tempPaths.tempServicePath,
    noisy: true,
    options: {
      development: Boolean(
        options.development === undefined
        ? old.development
        : options.development
      ),
      configuration: Object.assign({}, old.configuration, options.configuration),
      dependencies: Object.assign({}, old.dependencies, options.dependencies)
    }
  });
  _uninstall(mount, {
    teardown: Boolean(options.teardown),
    force: true
  });
  _install(tempService, tempPaths.tempBundlePath, {
    setup: options.setup === undefined || Boolean(options.setup),
    force: true
  });
  selfHeal();
  propagateSelfHeal();
  reloadRouting();

  return getServiceInstance(mount);
}

function runScript (scriptName, mount, options) {
  let service = getServiceInstance(mount);
  if (service.error) {
    throw service.error;
  }
  if (service.isDevelopment) {
    const runSetup = scriptName !== 'setup';
    service = reloadInstalledService(mount, runSetup);
  }
  ensureServiceLoaded(mount);
  try {
    const result = service.executeScript(scriptName, options);
    return result === undefined ? null : result;
  } catch (e) {
    e.codeFrame = codeFrame(e, service.basePath);
    throw e;
  }
}

function runTests (mount, options = {}) {
  let service = getServiceInstance(mount);
  if (service.error) {
    throw service.error;
  }
  if (service.isDevelopment) {
    service = reloadInstalledService(mount, true);
  }
  ensureServiceLoaded(mount);
  return require('@arangodb/foxx/mocha').run(service, options);
}

function enableDevelopmentMode (mount) {
  const service = getServiceInstance(mount);
  if (service.error) {
    throw service.error;
  }
  service.development(true);
  utils.updateService(mount, service.toJSON());
  propagateSelfHeal();
  reloadRouting();
  return service;
}

function disableDevelopmentMode (mount) {
  const service = getServiceInstance(mount);
  if (service.error) {
    throw service.error;
  }
  service.development(false);
  createServiceBundle(mount);
  service.updateChecksum();
  const bundleCollection = utils.getBundleStorage();
  if (!bundleCollection.exists(service.checksum)) {
    bundleCollection._binaryInsert({_key: service.checksum}, service.bundlePath);
  }
  utils.updateService(mount, service.toJSON());
  // Make sure setup changes from devmode are respected
  service.executeScript('setup');
  propagateSelfHeal();
  reloadRouting();
  return service;
}

function setConfiguration (mount, options = {}) {
  const service = getServiceInstance(mount);
  if (service.error) {
    throw service.error;
  }
  const warnings = service.applyConfiguration(options.configuration, options.replace);
  utils.updateService(mount, service.toJSON());
  propagateSelfHeal();
  reloadRouting();
  return warnings;
}

function setDependencies (mount, options = {}) {
  const service = getServiceInstance(mount);
  if (service.error) {
    throw service.error;
  }
  const warnings = service.applyDependencies(options.dependencies, options.replace);
  utils.updateService(mount, service.toJSON());
  propagateSelfHeal();
  reloadRouting();
  return warnings;
}

// Misc exported functions

function requireService (mount) {
  mount = '/' + mount.replace(/^\/+|\/+$/g, '');
  const service = getServiceInstance(mount);
  if (service.error) {
    // TODO Bad requires should probably always blow up
    return {};
  }
  return ensureServiceExecuted(service, true).exports;
}

function getMountPoints (internalOnly) {
  ensureFoxxInitialized(internalOnly);
  if (internalOnly) {
    return Array.from(INTERNAL_SERVICE_MAP.keys());
  }
  return Array.from(GLOBAL_SERVICE_MAP.get(db._name()).keys());
}

function installedServices () {
  ensureFoxxInitialized(false);
  return Array.from(GLOBAL_SERVICE_MAP.get(db._name()).values()).filter(service => !service.error);
}

function safeChecksum (mount) {
  try {
    return FoxxService.checksum(mount);
  } catch (e) {
    return null;
  }
}

// Exports

exports.install = install;
exports.uninstall = uninstall;
exports.replace = replace;
exports.upgrade = upgrade;
exports.runTests = runTests;
exports.runScript = runScript;
exports.development = enableDevelopmentMode;
exports.production = disableDevelopmentMode;
exports.setConfiguration = setConfiguration;
exports.setDependencies = setDependencies;
exports.requireService = requireService;
exports.lookupService = getServiceInstance;
exports.installedServices = installedServices;

// -------------------------------------------------
// Exported internals
// -------------------------------------------------

exports.isFoxxmaster = isFoxxmaster;
exports._reloadRouting = reloadRouting;
exports.reloadInstalledService = reloadInstalledService;
exports.ensureRouted = ensureServiceLoaded;
exports.initializeFoxx = initLocalServiceMap;
exports.ensureFoxxInitialized = ensureFoxxInitialized;
exports._startup = startup;
exports.heal = triggerSelfHeal;
exports.healAll = selfHealAll;
exports.commitLocalState = commitLocalState;
exports._createServiceBundle = createServiceBundle;
exports._resetCache = () => GLOBAL_SERVICE_MAP.clear();
exports._mountPoints = getMountPoints;
exports.validateInstallUrl = validateInstallUrl;

// -------------------------------------------------
// Exports from Foxx utils module
// -------------------------------------------------

exports.getServiceDefinition = utils.getServiceDefinition;
exports.list = utils.list;
exports.listDevelopment = utils.listDevelopment;
exports.listDevelopmentJson = utils.listDevelopmentJson;

// -------------------------------------------------
// Exports from Foxx store module
// -------------------------------------------------

exports.available = store.available;
exports.availableJson = store.availableJson;
exports.getFishbowlStorage = store.getFishbowlStorage;
exports.search = store.search;
exports.searchJson = store.searchJson;
exports.update = store.update;
exports.info = store.info;
