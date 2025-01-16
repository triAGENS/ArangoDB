/* jshint strict: false, maxlen: 300 */
/* global arango */

const db = require('@arangodb').db;
const internal = require('internal');
const _ = require('lodash');
const systemColors = internal.COLORS;
const output = internal.output;

let colors = {};

// max elements to print from array/objects
const maxMembersToPrint = 20;

let uniqueValue = 0;

const isCoordinator = function () {
  let isCoordinator = false;
  if (internal.isArangod()) {
    isCoordinator = require('@arangodb/cluster').isCoordinator();
  } else {
    try {
      if (arango) {
        let result = arango.GET('/_admin/server/role');
        if (result.role === 'COORDINATOR') {
          isCoordinator = true;
        }
      }
    } catch (err) {
      // ignore error
    }
  }
  return isCoordinator;
};

const anonymize = function (doc) {
  if (Array.isArray(doc)) {
    return doc.map(anonymize);
  }
  if (typeof doc === 'string') {
    // make unique values because of unique indexes
    return Array(doc.length + 1).join('X') + uniqueValue++;
  }
  if (doc === null || typeof doc === 'number' || typeof doc === 'boolean') {
    return doc;
  }
  if (typeof doc === 'object') {
    let result = {};
    Object.keys(doc).forEach(function (key) {
      if (key.startsWith('_') || key.startsWith('@')) {
        // This excludes system attributes in examples
        // and collections in bindVars
        result[key] = doc[key];
      } else {
        result[key] = anonymize(doc[key]);
      }
    });
    return result;
  }
  return doc;
};

let stringBuilder = {
  output: '',
  prefix: '',

  appendLine: function (line) {
    if (line) {
      this.output += this.prefix + line;
    }
    this.output += '\n';
  },

  getOutput: function () {
    return this.output;
  },

  clearOutput: function () {
    this.output = '';
  },

  wrap: function (str, width) {
    let re = '.{1,' + width + '}(\\s|$)|\\S+?(\\s|$)';
    return str.match(new RegExp(re, 'g')).join('\n' + this.prefix).replace(/\n+/g, '\n ' + this.prefix);
  }

};

/* set colors for output */
function setColors(useSystemColors) {
  'use strict';

  ['COLOR_RESET',
    'COLOR_CYAN', 'COLOR_BLUE', 'COLOR_GREEN', 'COLOR_MAGENTA', 'COLOR_YELLOW', 'COLOR_RED', 'COLOR_WHITE',
    'COLOR_BOLD_CYAN', 'COLOR_BOLD_BLUE', 'COLOR_BOLD_GREEN', 'COLOR_BOLD_MAGENTA', 'COLOR_BOLD_YELLOW',
    'COLOR_BOLD_RED', 'COLOR_BOLD_WHITE'].forEach(function (c) {
      colors[c] = useSystemColors ? systemColors[c] : '';
    });
}

/* colorizer and output helper functions */

function netLength(value) {
  'use strict';
  return String(value).replace(/\x1b\[.+?m/g, '').length;
}


function bracketize(node, v) {
  'use strict';
  if (node && node.subNodes && node.subNodes.length > 1) {
    return '(' + v + ')';
  }
  return v;
}

function attributeUncolored(v) {
  'use strict';
  return '`' + v + '`';
}

function indexFieldToName(v) {
  'use strict';
  if (typeof v === 'object') {
    return v.name;
  }
  return v;
}

function keyword(v) {
  'use strict';
  return colors.COLOR_CYAN + v + colors.COLOR_RESET;
}

function annotation(v) {
  'use strict';
  return colors.COLOR_BLUE + v + colors.COLOR_RESET;
}

function value(v) {
  'use strict';
  if (typeof v === 'string' && v.length > 1024) {
    return colors.COLOR_GREEN + v.substr(0, 1024) + '...' + colors.COLOR_RESET;
  }
  return colors.COLOR_GREEN + v + colors.COLOR_RESET;
}

function variable(v) {
  'use strict';
  if (v[0] === '#') {
    return colors.COLOR_MAGENTA + v + colors.COLOR_RESET;
  }
  return colors.COLOR_YELLOW + v + colors.COLOR_RESET;
}

function func(v) {
  'use strict';
  return colors.COLOR_GREEN + v + colors.COLOR_RESET;
}

function collection(v) {
  'use strict';
  return colors.COLOR_RED + v + colors.COLOR_RESET;
}

function view(v) {
  'use strict';
  return colors.COLOR_RED + v + colors.COLOR_RESET;
}

function attribute(v) {
  'use strict';
  return '`' + colors.COLOR_YELLOW + v + colors.COLOR_RESET + '`';
}

function attributePath(v) {
  'use strict';
  return v.split('.').map(attribute).join('.');
}

function header(v) {
  'use strict';
  return colors.COLOR_MAGENTA + v + colors.COLOR_RESET;
}

function section(v) {
  'use strict';
  return colors.COLOR_BOLD_BLUE + v + colors.COLOR_RESET;
}

// return n times ' '
function pad(n) {
  'use strict';
  if (n < 0) {
    // value seems invalid...
    n = 0;
  }
  return new Array(n).join(' ');
}

/* print query string */
function printQuery(query, cacheable, planCacheKey) {
  'use strict';
  // restrict max length of printed query to avoid endless printing for
  // very long query strings
  const maxLength = 4096;
  let headline = 'Query String (' + query.length + ' chars';
  if (query.length > maxLength) {
    headline += ' - truncated...';
    query = query.substr(0, maxLength / 2) + ' ... ' + query.substr(query.length - maxLength / 2);
  }
  if (planCacheKey !== undefined) {
    headline += ', plan cache key: "' + planCacheKey + '"';
  }
  headline += ', results cachable: ' + (cacheable ? 'true' : 'false');
  headline += '):';
  stringBuilder.appendLine(section(headline));
  stringBuilder.appendLine(' ' + value(stringBuilder.wrap(query, 100)));
  stringBuilder.appendLine();
}

/* print write query modification flags */
function printModificationFlags(flags) {
  'use strict';
  if (flags === undefined) {
    return;
  }
  stringBuilder.appendLine(section('Write query options:'));
  const keys = Object.keys(flags);
  let maxLen = 'Option'.length;
  keys.forEach(function (k) {
    maxLen = Math.max(maxLen, k.length);
  });
  stringBuilder.appendLine(' ' + header('Option') + pad(1 + maxLen - 'Option'.length) + '   ' + header('Value'));
  keys.forEach(function (k) {
    stringBuilder.appendLine(' ' + keyword(k) + pad(1 + maxLen - k.length) + '   ' + value(JSON.stringify(flags[k])));
  });
  stringBuilder.appendLine();
}

/* print optimizer rules */
function printRules(rules, stats) {
  'use strict';

  stringBuilder.appendLine(section('Optimization rules applied:'));
  if (rules.length === 0) {
    stringBuilder.appendLine(' ' + value('none'));
  } else {
    let maxIdLen = 'Id'.length;
    let maxRuleLen = 'Rule Name'.length;
    rules.forEach((rule, i) => {
      maxIdLen = Math.max(maxIdLen, String(i + 1).length);
      maxRuleLen = Math.max(maxRuleLen, rule.length);
    });
    const cols = 3;
    const off = Math.ceil(rules.length / cols);
    let parts = [];
    for (let j = 0; j < Math.min(rules.length, cols); ++j) {
      const idx = j * off;
      if (idx < rules.length) {
        parts.push(pad(1 + maxIdLen - 'Id'.length) + header('Id') + '   ' + header('Rule Name') + pad(1 + maxRuleLen - 'Rule Name'.length));
      }
    }
    stringBuilder.appendLine(' ' + parts.join('         '));

    for (let i = 0; i < off; ++i) {
      parts = [];
      for (let j = 0; j < cols; ++j) {
        const idx = i + j * off;
        if (idx < rules.length) {
          parts.push(pad(1 + maxIdLen - String(idx + 1).length) + variable(String(idx + 1)) + '   ' + keyword(rules[idx]) + pad(1 + maxRuleLen - rules[idx].length)); 
        }
      }
    
      stringBuilder.appendLine(' ' + parts.join('         '));
    }
  }
  stringBuilder.appendLine();

  if (!stats || !stats.rules) {
    return;
  }

  let maxNameLength = 0;
  let times = Object.keys(stats.rules).map(function(key) {
    maxNameLength = Math.max(maxNameLength, key.length);
    return { name: key, time: stats.rules[key] };
  });
  // filter out everything that was reasonably fast
  times = times.filter((item) => item.time >= 0.0002);

  times.sort(function(l, r) {
    // highest cost first
    return r.time - l.time;
  });
  // top few only
  times = times.slice(0, 5);

  if (times.length > 0) {
    stringBuilder.appendLine(section('Optimization rules with highest execution times:'));
    stringBuilder.appendLine(' ' + header('RuleName') + '   ' + pad(maxNameLength - 'RuleName'.length) + header('Duration [s]'));
    times.forEach(function(rule) {
      stringBuilder.appendLine(' ' + keyword(rule.name) + '   ' + pad(12 + maxNameLength - rule.name.length - rule.time.toFixed(5).length) + value(rule.time.toFixed(5)));
    });

    stringBuilder.appendLine();
  }

  let statsLine = value(stats.rulesExecuted) + annotation(' rule(s) executed');
  statsLine += ', ' + value(stats.plansCreated) + annotation(' plan(s) created');
  if (stats.hasOwnProperty('peakMemoryUsage')) {
    statsLine += ', ' + annotation('peak mem [b]') + ': ' + value(stats.peakMemoryUsage);
  }
  if (stats.hasOwnProperty('executionTime')) {
    statsLine += ', ' + annotation('exec time [s]') + ': ' + value(stats.executionTime.toFixed(5));
  }
  stringBuilder.appendLine(statsLine);
  stringBuilder.appendLine();
}

/* print warnings */
function printWarnings(warnings) {
  'use strict';
  if (!Array.isArray(warnings) || warnings.length === 0) {
    return;
  }

  stringBuilder.appendLine(section('Warnings:'));
  let maxIdLen = 'Code'.length;
  stringBuilder.appendLine(' ' + pad(1 + maxIdLen - 'Code'.length) + header('Code') + '   ' + header('Message'));
  for (let i = 0; i < warnings.length; ++i) {
    stringBuilder.appendLine(' ' + pad(1 + maxIdLen - String(warnings[i].code).length) + variable(warnings[i].code) + '   ' + keyword(warnings[i].message));
  }
  stringBuilder.appendLine();
}

/* print stats */
function printStats(stats, isCoord) {
  'use strict';
  if (!stats) {
    return;
  }

  const spc = '      ';

  stringBuilder.appendLine(section('Query Statistics:'));
  let maxWELen = 'Writes Exec'.length;
  let maxWILen = 'Writes Ign'.length;
  let maxDLLen = 'Doc. Lookups'.length;
  let maxSFLen = 'Scan Full'.length;
  let maxSILen = 'Scan Index'.length;
  let maxCHMLen = 'Cache Hits/Misses'.length;
  let maxFLen = 'Filtered'.length;
  let maxRLen = 'Requests'.length;
  let maxMemLen = 'Peak Mem [b]'.length;
  let maxETLen = 'Exec Time [s]'.length;
  stats.executionTime = stats.executionTime.toFixed(5);
  stringBuilder.appendLine(' ' + header('Writes Exec') + spc + header('Writes Ign') + spc + header('Doc. Lookups') + spc + header('Scan Full') + spc +
    header('Scan Index') + spc + header('Cache Hits/Misses') + spc + header('Filtered') + spc + (isCoord ? header('Requests') + spc : '') +
    header('Peak Mem [b]') + spc + header('Exec Time [s]'));

  stringBuilder.appendLine(' ' + pad(1 + maxWELen - String(stats.writesExecuted).length) + value(stats.writesExecuted) + spc +
    pad(1 + maxWILen - String(stats.writesIgnored).length) + value(stats.writesIgnored) + spc +
    pad(1 + maxDLLen - String((stats.documentLookups || 0)).length) + value(stats.documentLookups || 0) + spc +
    pad(1 + maxSFLen - String(stats.scannedFull).length) + value(stats.scannedFull) + spc +
    pad(1 + maxSILen - String(stats.scannedIndex).length) + value(stats.scannedIndex) + spc +
    pad(1 + maxCHMLen - (String(stats.cacheHits || 0) + ' / ' + String(stats.cacheMisses || 0)).length) + value(stats.cacheHits || 0) + ' / ' + value(stats.cacheMisses || 0) + spc +
    pad(1 + maxFLen - String(stats.filtered || 0).length) + value(stats.filtered || 0) + spc +
    (isCoord ? pad(1 + maxRLen - String(stats.httpRequests).length) + value(stats.httpRequests) + spc : '') +
    pad(1 + maxMemLen - String(stats.peakMemoryUsage).length) + value(stats.peakMemoryUsage) + spc +
    pad(1 + maxETLen - String(stats.executionTime).length) + value(stats.executionTime));
  stringBuilder.appendLine();
}

function printProfile(profile) {
  'use strict';
  if (!profile) {
    return;
  }

  stringBuilder.appendLine(section('Query Profile:'));
  let keys = Object.keys(profile);
  let maxHeadLen = 0;
  let maxDurLen = 'Duration [s]'.length;
  keys.forEach(key => {
    maxHeadLen = Math.max(maxHeadLen, key.length);
    maxDurLen = Math.max(maxDurLen, profile[key].toFixed(5).length);
  });

  const cols = 3;
  const off = Math.ceil(keys.length / cols);
  let parts = [];
  for (let j = 0; j < cols; ++j) {
    parts.push(header('Query Stage') + pad(1 + maxHeadLen - 'Query Stage'.length) + '    ' + pad(1 + maxDurLen - 'Duration [s]'.length) + header('Duration [s]'));
  }
  stringBuilder.appendLine(' ' + parts.join('         '));

  for (let i = 0; i < off; ++i) {
    parts = [];
    for (let j = 0; j < cols; ++j) {
      let key = keys[i + j * off];
      if (key !== undefined) {
        parts.push(keyword(key) + pad(1 + maxHeadLen - String(key).length) + '    ' + pad(1 + maxDurLen - profile[key].toFixed(5).length) + value(profile[key].toFixed(5)));
      }
    }
    
    stringBuilder.appendLine(' ' + parts.join('         '));
  }
  stringBuilder.appendLine();
}

/* print indexes used */
function printIndexes(indexes) {
  'use strict';

  stringBuilder.appendLine(section('Indexes used:'));

  if (indexes.length === 0) {
    stringBuilder.appendLine(' ' + value('none'));
    return;
  }

  let maxIdLen = String('By').length;
  let maxCollectionLen = String('Collection').length;
  let maxUniqueLen = String('Unique').length;
  let maxSparseLen = String('Sparse').length;
  let maxCacheLen = String('Cache').length;
  let maxNameLen = String('Name').length;
  let maxTypeLen = String('Type').length;
  let maxSelectivityLen = String('Selectivity').length;
  let maxFieldsLen = String('Fields').length;
  let maxStoredValuesLen = String('Stored values').length;
  indexes.forEach(function (index) {
    maxIdLen = Math.max(maxIdLen, String(index.node).length);
    maxNameLen = Math.max(maxNameLen, index.name ? index.name.length : 0);
    maxTypeLen = Math.max(maxTypeLen, index.type.length);
    maxFieldsLen = Math.max(maxFieldsLen, index.fields.map(indexFieldToName).map(attributeUncolored).join(', ').length + '[  ]'.length);
    let storedValuesFields = index.storedValues || [];
    if (index.type === 'inverted') {
      storedValuesFields = Array.isArray(index.storedValues) && index.storedValues.flatMap(s => s.fields) || [];
    }
    maxStoredValuesLen = Math.max(maxStoredValuesLen, storedValuesFields.map(indexFieldToName).map(attributeUncolored).join(', ').length + '[  ]'.length);
    maxCollectionLen = Math.max(maxCollectionLen, index.collection.length);
  });
  let line = ' ' + pad(1 + maxIdLen - String('By').length) + header('By') + '   ' +
    header('Name') + pad(1 + maxNameLen - 'Name'.length) + '   ' +
    header('Type') + pad(1 + maxTypeLen - 'Type'.length) + '   ' +
    header('Collection') + pad(1 + maxCollectionLen - 'Collection'.length) + '   ' +
    header('Unique') + pad(1 + maxUniqueLen - 'Unique'.length) + '   ' +
    header('Sparse') + pad(1 + maxSparseLen - 'Sparse'.length) + '   ' +
    header('Cache') + pad(1 + maxCacheLen - 'Cache'.length) + '   ' +
    header('Selectivity') + '   ' +
    header('Fields') + pad(1 + maxFieldsLen - 'Fields'.length) + '   ' +
    header('Stored values') + pad(1 + maxStoredValuesLen - 'Stored values'.length) + '   ' +
    header('Ranges');

  stringBuilder.appendLine(line);

  for (let i = 0; i < indexes.length; ++i) {
    let uniqueness = (indexes[i].unique ? 'true' : 'false');
    let sparsity = (indexes[i].hasOwnProperty('sparse') ? (indexes[i].sparse ? 'true' : 'false') : 'n/a');
    let cache = (indexes[i].hasOwnProperty('cacheEnabled') && indexes[i].cacheEnabled ? 'true' : 'false');
    let fields = '[ ' + indexes[i].fields.map(indexFieldToName).map(attribute).join(', ') + ' ]';
    let fieldsLen = indexes[i].fields.map(indexFieldToName).map(attributeUncolored).join(', ').length + '[  ]'.length;
    let storedValuesFields = indexes[i].storedValues || [];
    if (indexes[i].type === 'inverted') {
      storedValuesFields = Array.isArray(indexes[i].storedValues) && indexes[i].storedValues.flatMap(s => s.fields) || [];
    }
    let storedValues = '[ ' + storedValuesFields.map(indexFieldToName).map(attribute).join(', ') + ' ]';
    let storedValuesLen = storedValuesFields.map(indexFieldToName).map(attributeUncolored).join(', ').length + '[  ]'.length;
    let ranges = '';
    if (indexes[i].hasOwnProperty('condition')) {
      ranges = indexes[i].condition;
    }

    let estimate;
    if (indexes[i].hasOwnProperty('selectivityEstimate')) {
      estimate = (indexes[i].selectivityEstimate * 100).toFixed(2) + ' %';
    } else if (indexes[i].unique) {
      // hard-code estimate to 100%
      estimate = '100.00 %';
    } else {
      estimate = 'n/a';
    }

    line = ' ' +
      pad(1 + maxIdLen - String(indexes[i].node).length) + variable(String(indexes[i].node)) + '   ' +
      collection(indexes[i].name) + pad(1 + maxNameLen - indexes[i].name.length) + '   ' +
      keyword(indexes[i].type) + pad(1 + maxTypeLen - indexes[i].type.length) + '   ' +
      collection(indexes[i].collection) + pad(1 + maxCollectionLen - indexes[i].collection.length) + '   ' +
      value(uniqueness) + pad(1 + maxUniqueLen - uniqueness.length) + '   ' +
      value(sparsity) + pad(1 + maxSparseLen - sparsity.length) + '   ' +
      value(cache) + pad(1 + maxCacheLen - cache.length) + '   ' +
      pad(1 + maxSelectivityLen - estimate.length) + value(estimate) + '   ' +
      fields + pad(1 + maxFieldsLen - fieldsLen) + '   ' +
      storedValues + pad(1 + maxStoredValuesLen - storedValuesLen) + '   ' +
      ranges;

    stringBuilder.appendLine(line);
  }
}

function printFunctions(functions) {
  'use strict';

  let funcArray = [];
  Object.keys(functions).forEach(function (f) {
    funcArray.push(functions[f]);
  });

  if (funcArray.length === 0) {
    return;
  }
  stringBuilder.appendLine();
  stringBuilder.appendLine(section('Functions used:'));

  let maxNameLen = 'Name'.length;
  let maxDeterministicLen = 'Deterministic'.length;
  let maxCacheableLen = 'Cacheable'.length;
  let maxV8Len = 'Uses V8'.length;
  funcArray.forEach(function (f) {
    maxNameLen = Math.max(maxNameLen, String(f.name).length);
  });
  let line = ' ' +
    header('Name') + pad(1 + maxNameLen - 'Name'.length) + '   ' +
    header('Deterministic') + pad(1 + maxDeterministicLen - 'Deterministic'.length) + '   ' +
    header('Cacheable') + pad(1 + maxCacheableLen - 'Cacheable'.length) + '   ' +
    header('Uses V8') + pad(1 + maxV8Len - 'Uses V8'.length);

  stringBuilder.appendLine(line);

  for (let i = 0; i < funcArray.length; ++i) {
    // prevent "undefined"
    let deterministic = String(funcArray[i].isDeterministic || false);
    let cacheable = String(funcArray[i].cacheable || false);
    let usesV8 = String(funcArray[i].usesV8 || false);
    line = ' ' +
      variable(funcArray[i].name) + pad(1 + maxNameLen - funcArray[i].name.length) + '   ' +
      value(deterministic) + pad(1 + maxDeterministicLen - deterministic.length) + '   ' +
      value(cacheable) + pad(1 + maxCacheableLen - cacheable.length) + '   ' +
      value(usesV8) + pad(1 + maxV8Len - usesV8.length);

    stringBuilder.appendLine(line);
  }
}

/* create a table with a given amount of columns and arbitrary many rows */
class PrintedTable {
  constructor(numColumns) {
    this.content = [];
    for (let i = 0; i < numColumns; ++i) {
      this.content.push({
        header: "",
        cells: [],
        size: 0
      });
    }
  }

  setHeader(index, value) {
    this.content[index].header = value;
    this.content[index].size = Math.max(this.content[index].size, value.length);
  }

  addCell(index, value, valueLength) {
    // Value might be empty
    value = value || "";
    valueLength = valueLength || netLength(value);
    this.content[index].cells.push({ formatted: value, size: valueLength });
    this.content[index].size = Math.max(this.content[index].size, valueLength);
  }

  alignNewEntry() {
    let rowsNeeded = Math.max(...this.content.map(c => c.cells.length));
    for (let c of this.content) {
      while (c.cells.length < rowsNeeded) {
        c.cells.push({ formatted: '', size: 0 });
      }
    }
  }

  print(builder) {
    let rowsNeeded = Math.max(...this.content.map(c => c.cells.length));
    // Print the header
    let line = ' ';
    let col = 0;
    for (let c of this.content) {
      line += (col === 0 ? '' : pad(3)) + header(c.header);
      if (col + 1 < this.content.length) {
        // Don't pad rightmost column
        line += pad(1 + c.size - c.header.length);
      }
      ++col;
    }
    builder.appendLine(line);

    // Print the cells
    for (let i = 0; i < rowsNeeded; ++i) {
      let line = ' ';
      let col = 0;
      for (let c of this.content) {
        line += (col === 0 ? '' : pad(3));
        if (c.cells.length > i) {
          line += c.cells[i].formatted;
          if (col + 1 < this.content.length) {
            line += pad(1 + c.size - c.cells[i].size);
          }
        } else {
          if (col + 1 < this.content.length) {
            // Don't pad rightmost column
            line += pad(1 + c.size);
          }
        }
        ++col;
      }
      builder.appendLine(line);
    }
  }
}

/* print traversal info */
function printTraversalDetails(traversals) {
  'use strict';
  if (traversals.length === 0) {
    return;
  }

  stringBuilder.appendLine();
  stringBuilder.appendLine(section('Traversals on graphs:'));

  let outTable = new PrintedTable(6);
  outTable.setHeader(0, 'Id');
  outTable.setHeader(1, 'Depth');
  outTable.setHeader(2, 'Vertex collections');
  outTable.setHeader(3, 'Edge collections');
  outTable.setHeader(4, 'Options');
  outTable.setHeader(5, 'Filter / Prune Conditions');

  let optify = function (options, colorize) {
    let opts = {
      bfs: options.bfs || undefined, /* only print if set to true to save room */
      neighbors: options.neighbors || undefined, /* only print if set to true to save room */
      uniqueVertices: options.uniqueVertices,
      uniqueEdges: options.uniqueEdges,
      order: options.order || 'dfs',
      weightAttribute: options.order === 'weighted' && options.weightAttribute || undefined,
    };

    let result = '';
    for (let att in opts) {
      if (opts[att] === undefined) {
        continue;
      }
      if (result.length > 0) {
        result += ', ';
      }
      if (colorize) {
        result += keyword(att) + ': ';
        if (typeof opts[att] === 'boolean') {
          result += value(opts[att] ? 'true' : 'false');
        } else {
          result += value(String(opts[att]));
        }
      } else {
        result += att + ': ';
        if (typeof opts[att] === 'boolean') {
          result += opts[att] ? 'true' : 'false';
        } else {
          result += String(opts[att]);
        }
      }
    }
    return result;
  };

  traversals.forEach(node => {
    outTable.alignNewEntry();
    outTable.addCell(0, variable(String(node.id)));
    outTable.addCell(1, node.minMaxDepth);
    outTable.addCell(2, node.vertexCollectionNameStr, node.vertexCollectionNameStrLen);
    outTable.addCell(3, node.edgeCollectionNameStr, node.edgeCollectionNameStrLen);
    if (node.hasOwnProperty('options')) {
      outTable.addCell(4, optify(node.options, true), optify(node.options, false).length);
    } else if (node.hasOwnProperty('traversalFlags')) {
      outTable.addCell(4, optify(node.traversalFlags, true), optify(node.options, false).length);
    }
    // else do not add a cell in 4
    if (node.hasOwnProperty('ConditionStr')) {
      outTable.addCell(5, keyword('FILTER ') + node.ConditionStr);
    }
    if (node.hasOwnProperty('PruneConditionStr')) {
      outTable.addCell(5, keyword('PRUNE ') + node.PruneConditionStr);
    }
  });
  outTable.print(stringBuilder);
}

/* print shortest_path info */
function printShortestPathDetails(shortestPaths) {
  'use strict';
  if (shortestPaths.length === 0) {
    return;
  }

  stringBuilder.appendLine();
  stringBuilder.appendLine(section('Shortest paths on graphs:'));

  let maxIdLen = 'Id'.length;
  let maxVertexCollectionNameStrLen = 'Vertex collections'.length;
  let maxEdgeCollectionNameStrLen = 'Edge collections'.length;

  shortestPaths.forEach(function (node) {
    maxIdLen = Math.max(maxIdLen, String(node.id).length);

    if (node.hasOwnProperty('vertexCollectionNameStr')) {
      if (node.vertexCollectionNameStrLen > maxVertexCollectionNameStrLen) {
        maxVertexCollectionNameStrLen = node.vertexCollectionNameStrLen;
      }
    }
    if (node.hasOwnProperty('edgeCollectionNameStr')) {
      if (node.edgeCollectionNameStrLen > maxEdgeCollectionNameStrLen) {
        maxEdgeCollectionNameStrLen = node.edgeCollectionNameStrLen;
      }
    }
  });

  let line = ' ' + pad(1 + maxIdLen - 'Id'.length) + header('Id') + '   ' +
    header('Vertex collections') + pad(1 + maxVertexCollectionNameStrLen - 'Vertex collections'.length) + '   ' +
    header('Edge collections') + pad(1 + maxEdgeCollectionNameStrLen - 'Edge collections'.length);

  stringBuilder.appendLine(line);

  for (let sp of shortestPaths) {
    line = ' ' + pad(1 + maxIdLen - String(sp.id).length) + variable(sp.id) + '   ';

    if (sp.hasOwnProperty('vertexCollectionNameStr')) {
      line += sp.vertexCollectionNameStr +
        pad(1 + maxVertexCollectionNameStrLen - sp.vertexCollectionNameStrLen) + '   ';
    } else {
      line += pad(1 + maxVertexCollectionNameStrLen) + '   ';
    }

    if (sp.hasOwnProperty('edgeCollectionNameStr')) {
      line += sp.edgeCollectionNameStr +
        pad(1 + maxEdgeCollectionNameStrLen - sp.edgeCollectionNameStrLen) + '   ';
    } else {
      line += pad(1 + maxEdgeCollectionNameStrLen) + '   ';
    }

    if (sp.hasOwnProperty('ConditionStr')) {
      line += sp.ConditionStr;
    }

    stringBuilder.appendLine(line);
  }
}

function printKShortestPathsDetails(shortestPaths) {
  'use strict';
  if (shortestPaths.length === 0) {
    return;
  }

  stringBuilder.appendLine();
  stringBuilder.appendLine(section('k shortest paths on graphs:'));

  let maxIdLen = 'Id'.length;
  let maxVertexCollectionNameStrLen = 'Vertex collections'.length;
  let maxEdgeCollectionNameStrLen = 'Edge collections'.length;

  shortestPaths.forEach(function (node) {
    maxIdLen = Math.max(maxIdLen, String(node.id).length);

    if (node.hasOwnProperty('vertexCollectionNameStr')) {
      if (node.vertexCollectionNameStrLen > maxVertexCollectionNameStrLen) {
        maxVertexCollectionNameStrLen = node.vertexCollectionNameStrLen;
      }
    }
    if (node.hasOwnProperty('edgeCollectionNameStr')) {
      if (node.edgeCollectionNameStrLen > maxEdgeCollectionNameStrLen) {
        maxEdgeCollectionNameStrLen = node.edgeCollectionNameStrLen;
      }
    }
  });

  let line = ' ' + pad(1 + maxIdLen - 'Id'.length) + header('Id') + '   ' +
    header('Vertex collections') + pad(1 + maxVertexCollectionNameStrLen - 'Vertex collections'.length) + '   ' +
    header('Edge collections') + pad(1 + maxEdgeCollectionNameStrLen - 'Edge collections'.length);

  stringBuilder.appendLine(line);

  for (let sp of shortestPaths) {
    line = ' ' + pad(1 + maxIdLen - String(sp.id).length) + sp.id + '   ';

    if (sp.hasOwnProperty('vertexCollectionNameStr')) {
      line += sp.vertexCollectionNameStr +
        pad(1 + maxVertexCollectionNameStrLen - sp.vertexCollectionNameStrLen) + '   ';
    } else {
      line += pad(1 + maxVertexCollectionNameStrLen) + '   ';
    }

    if (sp.hasOwnProperty('edgeCollectionNameStr')) {
      line += sp.edgeCollectionNameStr +
        pad(1 + maxEdgeCollectionNameStrLen - sp.edgeCollectionNameStrLen) + '   ';
    } else {
      line += pad(1 + maxEdgeCollectionNameStrLen) + '   ';
    }

    if (sp.hasOwnProperty('ConditionStr')) {
      line += sp.ConditionStr;
    }

    stringBuilder.appendLine(line);
  }
}


/* analyze and print execution plan */
function processQuery(query, explain, planIndex) {
  'use strict';
  var nodes = {},
    parents = {},
    rootNode = null,
    maxTypeLen = 0,
    maxSiteLen = 0,
    maxIdLen = String('Id').length,
    maxEstimateLen = String('Est.').length,
    maxCallsLen = String('Calls').length,
    maxParallelLen = String('Par').length,
    maxItemsLen = String('Items').length,
    maxFilteredLen = String('Filtered').length,
    maxRuntimeLen = String('Runtime [s]').length,
    stats = explain.stats;
  
  if (explain === undefined || (explain.plan === undefined && explain.plans === undefined)) {
    throw "incomplete query execution plan data - this should not happen unless when connected to a DB-Server. fetching query plans/profiles from a DB-Server is not supported!";
  }

  let plan;
  if (planIndex !== undefined) {
    plan = explain.plans[planIndex];
  } else {
    plan = explain.plan;
  }

  /// mode with actual runtime stats per node
  let profileMode = stats && stats.hasOwnProperty('nodes');

  let recursiveWalk = function (partNodes, level, site) {
    let n = _.clone(partNodes);
    n.reverse();
    n.forEach(function (node) {
      // set location of execution node in cluster
      node.site = site;

      nodes[node.id] = node;
      if (level === 0 && node.dependencies.length === 0) {
        rootNode = node.id;
      }
      if (node.type === 'SubqueryNode') {
        // enter subquery
        recursiveWalk(node.subquery.nodes, level + 1, site);
      } else if (node.type === 'RemoteNode') {
        site = (site === 'COOR' ? 'DBS' : 'COOR');
      }
      node.dependencies.forEach(function (d) {
        if (!parents.hasOwnProperty(d)) {
          parents[d] = [];
        }
        parents[d].push(node.id);
      });

      maxIdLen = Math.max(maxIdLen, String(node.id).length);
      maxTypeLen = Math.max(maxTypeLen, String(node.type).length);
      maxSiteLen = Math.max(maxSiteLen, String(node.site).length);
      if (!profileMode) { // not shown when we got actual runtime stats
        maxEstimateLen = Math.max(maxEstimateLen, String(node.estimatedNrItems).length);
        if (node.type === 'JoinNode') {
          node.indexInfos.forEach((info) => {
            maxEstimateLen = Math.max(maxEstimateLen, String(info.estimatedNrItems).length);
          });
        }
      }
    });
  };

  recursiveWalk(plan.nodes, 0, 'COOR');

  if (profileMode) { 
    // merge runtime info into plan.
    // note that this is only necessary for server versions <= 3.11.
    // the code can be removed when only supporting server versions
    // >= 3.12.
    stats.nodes.forEach(n => {
      // the try..catch here is necessary because we may have nodes inside
      // the stats object that are not contained in the original plan.
      // this can happen for parallel traversals, which inserts additional
      // nodes into the plan on the DB servers after the plan is shipped
      // to the servers.
      try {
        nodes[n.id].runtime = n.runtime;
      } catch (err) {}
    });
    stats.nodes.forEach(n => {
      if (nodes.hasOwnProperty(n.id) && !n.hasOwnProperty('fetching')) {
        // no separate fetching stats. that means the runtime is cumulative.
        // by subtracting the dependencies from parent runtime we get the runtime per node
        if (parents.hasOwnProperty(n.id)) {
          parents[n.id].forEach(pid => {
            nodes[pid].runtime -= n.runtime;
          });
        }
      }
    });

    stats.nodes.forEach(n => {
      if (nodes.hasOwnProperty(n.id)) {
        let runtime = 0;
        if (n.hasOwnProperty('fetching')) {
          // this branch is taken for servers >= 3.12.
          runtime = n.runtime;
          // we have separate runtime (including fetching time)
          // and fetching stats
          runtime -= n.fetching;
        } else if (nodes[n.id].hasOwnProperty('runtime')) {
          // this branch is taken for servers <= 3.11 and can be removed
          // eventually.
          runtime = nodes[n.id].runtime;
        }

        nodes[n.id].calls = String(n.calls || 0);
        nodes[n.id].parallel = (nodes[n.id].isAsyncPrefetchEnabled ? String(n.parallel || 0) : '-');
        nodes[n.id].items = String(n.items || 0);
        nodes[n.id].filtered = String(n.filtered || 0);
        nodes[n.id].runtime = runtime.toFixed(5);

        maxCallsLen = Math.max(maxCallsLen, String(nodes[n.id].calls).length);
        maxParallelLen = Math.max(maxParallelLen, String(nodes[n.id].parallel).length);
        maxItemsLen = Math.max(maxItemsLen, String(nodes[n.id].items).length);
        maxFilteredLen = Math.max(maxFilteredLen, String(nodes[n.id].filtered).length);
        maxRuntimeLen = Math.max(maxRuntimeLen, nodes[n.id].runtime.length);
      }
    });
  }

  var references = {},
    collectionVariables = {},
    usedVariables = {},
    indexes = [],
    traversalDetails = [],
    shortestPathDetails = [],
    kShortestPathsDetails = [],
    functions = [],
    modificationFlags,
    isConst = true,
    currentNode = null;

  const variableName = function (node) {
    try {
      if (/^[0-9_]/.test(node.name)) {
        return variable('#' + node.name);
      }
    } catch (x) {
      require('console').warn("got unexpected invalid variable struct in explainer");
      require("console").trace();
      throw x;
    }

    if (collectionVariables.hasOwnProperty(node.id)) {
      usedVariables[node.name] = collectionVariables[node.id];
    }
    return variable(node.name);
  };

  const lateVariableCallback = function () {
    return (node) => variableName(node);
  };

  const unfoldRawData = function (value) {
    let node = {};
    if (Array.isArray(value)) {
      node.type = 'array';
      node.subNodes = [];
      value.forEach((v) => {
        node.subNodes.push(unfoldRawData(v));
      });
    } else if (typeof value === 'object' && value !== null) {
      node.type = 'object';
      node.subNodes = [];
      Object.keys(value).forEach((k) => {
        node.subNodes.push({
          type: 'object element',
          name: k,
          subNodes: [ unfoldRawData(value[k]) ]
        });
      });
    } else {
      node.type = 'value';
      node.value = value;
    }
    return node;
  };

  const checkRawData = function (node) {
    if (node.hasOwnProperty('raw')) {
      node = unfoldRawData(node.raw);
    } else {
      if (Array.isArray(node.subNodes)) {
        let s = [];
        node.subNodes.forEach((v) => {
          s.push(checkRawData(v));
        });
        node.subNodes = s;
      }
    }
    return node;
  };

  const buildExpression = function (node) {
    // replace "raw" value with proper subNodes
    node = checkRawData(node);

    const binaryOperator = function (node, name) {
      if (name.match(/^[a-zA-Z]+$/)) {
        // make it a keyword
        name = keyword(name.toUpperCase());
      }
      const lhs = buildExpression(node.subNodes[0]);
      const rhs = buildExpression(node.subNodes[1]);
      if (node.subNodes.length === 3) {
        // array operator node... prepend "all" | "any" | "none" to node type
        name = keyword(node.subNodes[2].quantifier.toUpperCase()) + ' ' + name;
      }
      if (node.sorted) {
        return lhs + ' ' + name + ' ' + annotation('/* sorted */') + ' ' + rhs;
      }
      return lhs + ' ' + name + ' ' + rhs;
    };

    isConst = isConst && (['value', 'object', 'object element', 'array'].indexOf(node.type) !== -1);

    switch (node.type) {
      case 'reference':
        if (references.hasOwnProperty(node.name)) {
          const ref = references[node.name];
          let c = '*';
          if (ref.length > 5 && ref[5]) {
            c = '?';
          }
          delete references[node.name];
          if (Array.isArray(ref)) {
            let out = buildExpression(ref[1]) + '[' + (new Array(ref[0] + 1).join(c));
            if (ref[2].type !== 'no-op') {
              out += ' ' + keyword('FILTER') + ' ' + buildExpression(ref[2]);
            }
            if (ref[3].type !== 'no-op') {
              out += ' ' + keyword('LIMIT ') + ' ' + buildExpression(ref[3]);
            }
            if (ref[4].type !== 'no-op') {
              out += ' ' + keyword('RETURN ') + ' ' + buildExpression(ref[4]);
            }
            out += ']';
            return out;
          }
          return buildExpression(ref) + '[' + c + ']';
        }
        return variableName(node);
      case 'collection':
        return collection(node.name) + '   ' + annotation('/* all collection documents */');
      case 'value':
        return value(JSON.stringify(node.value));
      case 'object':
        if (node.hasOwnProperty('subNodes')) {
          if (node.subNodes.length > maxMembersToPrint) {
            // print only the first few values from the object
            return '{ ' + node.subNodes.slice(0, maxMembersToPrint).map(buildExpression).join(', ') + ', ... }';
          }
          return '{ ' + node.subNodes.map(buildExpression).join(', ') + ' }';
        }
        return '{ }';
      case 'object element':
        return value(JSON.stringify(node.name)) + ' : ' + buildExpression(node.subNodes[0]);
      case 'calculated object element':
        return '[ ' + buildExpression(node.subNodes[0]) + ' ] : ' + buildExpression(node.subNodes[1]);
      case 'array':
        if (node.hasOwnProperty('subNodes')) {
          if (node.subNodes.length > maxMembersToPrint) {
            // print only the first few values from the array
            return '[ ' + node.subNodes.slice(0, maxMembersToPrint).map(buildExpression).join(', ') + ', ... ]';
          }
          return '[ ' + node.subNodes.map(buildExpression).join(', ') + ' ]';
        }
        return '[ ]';
      case 'unary not':
        return '! ' + buildExpression(node.subNodes[0]);
      case 'unary plus':
        return '+ ' + buildExpression(node.subNodes[0]);
      case 'unary minus':
        return '- ' + buildExpression(node.subNodes[0]);
      case 'array limit':
        return buildExpression(node.subNodes[0]) + ', ' + buildExpression(node.subNodes[1]);
      case 'attribute access':
        return buildExpression(node.subNodes[0]) + '.' + attribute(node.name);
      case 'indexed access':
        return buildExpression(node.subNodes[0]) + '[' + buildExpression(node.subNodes[1]) + ']';
      case 'range':
        return buildExpression(node.subNodes[0]) + ' .. ' + buildExpression(node.subNodes[1]) + '   ' + annotation('/* range */');
      case 'no-op':
        return '';
      case 'quantifier':
        return node.quantifier.toUpperCase();
      case 'expand':
      case 'expansion':
        if (node.subNodes.length > 2) {
          // [FILTER ...]
          references[node.subNodes[0].subNodes[0].name] = [node.levels, node.subNodes[0].subNodes[1], node.subNodes[2], node.subNodes[3], node.subNodes[4], node.booleanize || false];
        } else {
          // [*]
          references[node.subNodes[0].subNodes[0].name] = node.subNodes[0].subNodes[1];
        }
        return buildExpression(node.subNodes[1]);
      case 'array filter':
        return buildExpression(node.subNodes[0]) + " " + buildExpression(node.subNodes[1]);
      case 'user function call':
        return func(node.name) + '(' + ((node.subNodes && node.subNodes[0].subNodes) || []).map(buildExpression).join(', ') + ')' + '   ' + annotation('/* user-defined function */');
      case 'function call':
        return func(node.name) + '(' + ((node.subNodes && node.subNodes[0].subNodes) || []).map(buildExpression).join(', ') + ')';
      case 'plus':
        return '(' + binaryOperator(node, '+') + ')';
      case 'minus':
        return '(' + binaryOperator(node, '-') + ')';
      case 'times':
        return '(' + binaryOperator(node, '*') + ')';
      case 'division':
        return '(' + binaryOperator(node, '/') + ')';
      case 'modulus':
        return '(' + binaryOperator(node, '%') + ')';
      case 'compare not in':
      case 'array compare not in':
        return '(' + binaryOperator(node, 'not in') + ')';
      case 'compare in':
      case 'array compare in':
        return '(' + binaryOperator(node, 'in') + ')';
      case 'compare ==':
      case 'array compare ==':
        return '(' + binaryOperator(node, '==') + ')';
      case 'compare !=':
      case 'array compare !=':
        return '(' + binaryOperator(node, '!=') + ')';
      case 'compare >':
      case 'array compare >':
        return '(' + binaryOperator(node, '>') + ')';
      case 'compare >=':
      case 'array compare >=':
        return '(' + binaryOperator(node, '>=') + ')';
      case 'compare <':
      case 'array compare <':
        return '(' + binaryOperator(node, '<') + ')';
      case 'compare <=':
      case 'array compare <=':
        return '(' + binaryOperator(node, '<=') + ')';
      case 'logical or':
        return '(' + binaryOperator(node, '||') + ')';
      case 'logical and':
        return '(' + binaryOperator(node, '&&') + ')';
      case 'ternary':
        if (node.subNodes.length === 2) {
          return '(' + buildExpression(node.subNodes[0]) + ' ?: ' + buildExpression(node.subNodes[1]) + ')';
        }
        return '(' + buildExpression(node.subNodes[0]) + ' ? ' + buildExpression(node.subNodes[1]) + ' : ' + buildExpression(node.subNodes[2]) + ')';
      case 'n-ary or':
        if (node.hasOwnProperty('subNodes')) {
          return bracketize(node, node.subNodes.map(function (sub) { return buildExpression(sub); }).join(' || '));
        }
        return '';
      case 'n-ary and':
        if (node.hasOwnProperty('subNodes')) {
          return bracketize(node, node.subNodes.map(function (sub) { return buildExpression(sub); }).join(' && '));
        }
        return '';
      case 'parameter':
      case 'datasource parameter':
        return value('@' + node.name);
      case undefined:
        return '';
      default:
        return 'unhandled node type in buildExpression (' + node.type + ')';
    }
  };

  const buildSimpleExpression = function (simpleExpressions) {
    let rc = '';

    for (let indexNo in simpleExpressions) {
      if (simpleExpressions.hasOwnProperty(indexNo)) {
        if (rc.length > 0) {
          rc += ' AND ';
        }
        for (let i = 0; i < simpleExpressions[indexNo].length; i++) {
          let item = simpleExpressions[indexNo][i];
          rc += attribute('Path') + '.';
          if (item.isEdgeAccess) {
            rc += attribute('edges');
          } else {
            rc += attribute('vertices');
          }
          rc += '[' + value(indexNo) + '] -> ';
          rc += buildExpression(item.varAccess);
          rc += ' ' + item.comparisonTypeStr + ' ';
          rc += buildExpression(item.compareTo);
        }
      }
    }
    return rc;
  };

  const buildBound = function (attr, operators, bound) {
    const boundValue = bound.isConstant ? value(JSON.stringify(bound.bound)) : buildExpression(bound.bound);
    return attribute(attr) + ' ' + operators[bound.include ? 1 : 0] + ' ' + boundValue;
  };

  const projections = function (value, attributeName, label) {
    if (value && value.hasOwnProperty(attributeName) && value[attributeName].length > 0) {
      let fields = value[attributeName].map(function(p) {
        if (Array.isArray(p)) {
          return '`' + p.join('`.`') + '`';
        } else if (typeof p === 'string') {
          return '`' + p + '`';
        } else if (p.hasOwnProperty('path')) {
          return '`' + p.path.join('`.`') + '`';
        }
      });
      return ' (' + label + ': ' + fields.join(', ') + ')';
    }
    return '';
  };

  const restriction = function (node) {
    if (node.restrictedTo) {
      return `, shard: ${node.restrictedTo}`;
    } else if (node.numberOfShards) {
      return `, ${node.numberOfShards} shard(s)`;
    }
    return '';
  };

  const iterateIndexes = function (idx, i, node, types, variable) {
    let what = (!node.ascending ? 'reverse ' : '') + idx.type + ' index scan';
    if (node.producesResult || !node.hasOwnProperty('producesResult')) {
      if (node.indexCoversProjections || node.isLateMaterialized === true) {
        what += ', index only';
      } else {
        what += ', index scan + document lookup';
      }
    } else {
      what += ', scan only';
    }
    if (node.readOwnWrites) {
      what += "; read own writes";
    }

    if (types.length === 0 || what !== types[types.length - 1]) {
      types.push(what);
    }
    idx.collection = node.collection;
    idx.node = node.id;
    if (node.hasOwnProperty('condition') && node.hasOwnProperty('allCoveredByOneIndex') &&
        node.allCoveredByOneIndex) {
       idx.condition = buildExpression(node.condition);
    } else if (node.hasOwnProperty('condition') && node.condition.type && node.condition.type === 'n-ary or') {
      idx.condition = buildExpression(node.condition.subNodes[i]);
    } else {
      if (variable !== false && variable !== undefined) {
        idx.condition = variable;
      }
    }
    if (idx.condition === '' || idx.condition === undefined) {
      idx.condition = '*'; // empty condition. this is likely an index used for sorting or scanning only
    }
    indexes.push(idx);
  };

  const label = function (node) {
    let rc, v, vNames, e, eNames, edgeCols, i, d, directions, isLast;
    let parts = [];
    let types = [];
    let filter = '';
    // index in this array gives the traversal direction
    const translate = ['ANY', 'INBOUND', 'OUTBOUND'];
    // get all indexes of a graph node, i.e. traversal, shortest path, etc.
    const getGraphNodeIndexes = (node) => {
      const allIndexes = [];
      for (i = 0; i < node.edgeCollections.length; ++i) {
        d = node.directions[i];
        // base indexes
        let ix = node.indexes.base[i];
        ix.collection = node.edgeCollections[i];
        ix.condition = keyword("base " + translate[d]);
        ix.level = -1;
        ix.direction = d;
        ix.node = node.id;
        allIndexes.push(ix);

        // level-specific indexes
        for (let l in node.indexes.levels) {
          ix = node.indexes.levels[l][i];
          ix.collection = node.edgeCollections[i];
          ix.condition = keyword("level " + parseInt(l, 10) + " " + translate[d]);
          ix.level = parseInt(l, 10);
          ix.direction = d;
          ix.node = node.id;
          allIndexes.push(ix);
        }
      }

      allIndexes.sort(function (l, r) {
        if (l.collection !== r.collection) {
          return l.collection < r.collection ? -1 : 1;
        }
        if (l.level !== r.level) {
          return l.level < r.level ? -1 : 1;
        }
        if (l.direction !== r.direction) {
          return l.direction < r.direction ? -1 : 1;
        }
        return 0;
      });

      return allIndexes;
    };

    const handleGraphDefinition = (node) => { 
      if (node.hasOwnProperty('graphDefinition')) {
        let v = [];
        let vNames = [];
        if (node.hasOwnProperty('options') && node.options.hasOwnProperty('vertexCollections')) {
          node.options.vertexCollections.forEach(function (vcn) {
            v.push(collection(vcn));
            vNames.push(vcn);
          });
        } else {
          node.graphDefinition.vertexCollectionNames.forEach(function (vcn) {
            v.push(collection(vcn));
            vNames.push(vcn);
          });
        }
        node.vertexCollectionNameStr = v.join(', ');
        node.vertexCollectionNameStrLen = vNames.join(', ').length;

        let e = [];
        let eNames = [];
        if (node.hasOwnProperty('options') && node.options.hasOwnProperty('edgeCollections')) {
          node.options.edgeCollections.forEach(function (ecn) {
            e.push(collection(ecn));
            eNames.push(ecn);
          });
        } else {
          node.graphDefinition.edgeCollectionNames.forEach(function (ecn) {
            e.push(collection(ecn));
            eNames.push(ecn);
          });
        }
        node.edgeCollectionNameStr = e.join(', ');
        node.edgeCollectionNameStrLen = eNames.join(', ').length;
      } else {
        let e = [];
        let eNames = [];
        if (node.hasOwnProperty('options') && node.options.hasOwnProperty('edgeCollections')) {
          node.options.edgeCollections.forEach(function (ecn) {
            e.push(collection(ecn));
            eNames.push(ecn);
          });
        } else {
          edgeCols = node.graph || [];
          edgeCols.forEach(function (ecn) {
            e.push(collection(ecn));
            eNames.push(ecn);
          });
        }
        node.edgeCollectionNameStr = e.join(', ');
        node.edgeCollectionNameStrLen = eNames.join(', ').length;
        node.graph = '<anonymous>';

        if (node.hasOwnProperty('options') && node.options.hasOwnProperty('vertexCollections')) {
          let v = [];
          node.options.vertexCollections.forEach(function (vcn) {
            v.push(collection(vcn));
          });
          node.vertexCollectionNameStr = v.join(', ');
          node.vertexCollectionNameStrLen = node.options.vertexCollections.join(', ').length;
        }
      }
    };

    switch (node.type) {
      case 'SingletonNode':
        let bindVars = [];
        if (node.bindParameterVariables) {
          for (const [name, bindVar] of Object.entries(node.bindParameterVariables)) {
            bindVars.push(variableName(bindVar) + ' = ' + variable("@" + name));
          }
        }
        return keyword('ROOT') + ' ' + bindVars.join(', ');
      case 'NoResultsNode':
        return keyword('EMPTY') + '   ' + annotation('/* empty result set */');
      case 'EnumerateCollectionNode':
        collectionVariables[node.outVariable.id] = node.collection;
        if (node.filter) {
          filter = '   ' + keyword('FILTER') + ' ' + buildExpression(node.filter) + '   ' + annotation('/* early pruning */');
        }
        if (node.projections) {
          // produce LET nodes for each projection output register
          let parts = [];
          node.projections.forEach((p) => {
            if (p.hasOwnProperty('variable')) {
              parts.push(variableName(p.variable) + ' = ' + variableName(node.outVariable) + '.' + p.path.map((p) => attribute(p)).join('.'));
            }
          });
          if (parts.length) {
            filter = '   ' + keyword('LET') + ' ' + parts.join(', ') + filter;
          }
        }
        const enumAnnotation = annotation('/* full collection scan' + (node.random ? ', random order' : '') + projections(node, 'projections', 'projections') + (node.satellite ? ', satellite' : '') + ((node.producesResult || !node.hasOwnProperty('producesResult')) ? '' : ', scan only') + (node.count ? ', with count optimization' : '') + `${restriction(node)} ${node.readOwnWrites ? '; read own writes' : ''} */`);
        return keyword('FOR') + ' ' + variableName(node.outVariable) + ' ' + keyword('IN') + ' ' + collection(node.collection) + '   ' + enumAnnotation + filter;
      case 'EnumerateListNode':
        if (node.filter) {
          filter = '   ' + keyword('FILTER') + ' ' + buildExpression(node.filter) + '   ' + annotation('/* early pruning */');
        }
        if (node.mode === "object") {
          return keyword('FOR') + ' [' + variableName(node.keyOutVariable) + ', ' + variableName(node.valueOutVariable) + '] ' + keyword('OF') + ' ' + variableName(node.inVariable) + '   ' + annotation('/* object iteration */') + filter;
        } else {
          return keyword('FOR') + ' ' + variableName(node.outVariable) + ' ' + keyword('IN') + ' ' + variableName(node.inVariable) + '   ' + annotation('/* list iteration */') + filter;
        }
        break;
      case 'EnumerateNearVectorNode': {
        let searchParameters = '';
        if (node.hasOwnProperty("searchParameters") && JSON.stringify(node.searchParameters) !== "{}") {
          searchParameters = keyword(' WITH SEARCH PARAMETERS ') + JSON.stringify(node.searchParameters);
        }

        return keyword('FOR') + ' ' + variableName(node.oldDocumentVariable) + keyword(' OF ') + collection(node.collection) + keyword(' IN TOP ') + node.limit + keyword(' NEAR ') + variableName(node.inVariable) + keyword(' DISTANCE INTO ') + variableName(node.distanceOutVariable) + searchParameters;
      }
      case 'EnumerateViewNode':
        var condition = '';
        if (node.condition && node.condition.hasOwnProperty('type')) {
          condition = keyword(' SEARCH ') + buildExpression(node.condition);
        }

        var sortCondition = '';
        if (node.primarySort && Array.isArray(node.primarySort)) {
          var primarySortBuckets = node.primarySort.length;
          if (typeof node.primarySortBuckets === 'number') {
            primarySortBuckets = Math.min(node.primarySortBuckets, primarySortBuckets);
          }

          sortCondition = keyword(' SORT ') + node.primarySort.slice(0, primarySortBuckets).map(function (element) {
            return variableName(node.outVariable) + '.' + attributePath(element.field) + ' ' + keyword(element.asc ? 'ASC' : 'DESC');
          }).join(', ');
        }

        var scorers = '';
        if (node.scorers && node.scorers.length > 0) {
          scorers = node.scorers.map(function (scorer) {
            return keyword(' LET ') + variableName(scorer) + ' = ' + buildExpression(scorer.node);
          }).join('');
        }

        var scorersSort = '';
        if (node.hasOwnProperty('scorersSort') && node.scorersSort.length > 0) {
          node.scorersSort.forEach(function(sort) {
              if (scorersSort.length > 0 ) {
                scorersSort += ', ';
              }
              scorersSort += variableName(node.scorers[sort.index]);
              if (sort.asc) {
                scorersSort += keyword(' ASC');
              } else {
                scorersSort += keyword(' DESC');
              }

          });
           scorersSort = keyword(' SORT ') + scorersSort;
           scorersSort += keyword(' LIMIT ') + value('0, ' + JSON.stringify(node.scorersSortLimit));

        }
        let viewAnnotation = '/* view query';
        if (node.hasOwnProperty('outNmDocId')) {
          viewAnnotation += ' with late materialization';
        } else if (node.hasOwnProperty('noMaterialization') && node.noMaterialization) {
          viewAnnotation += ' without materialization';
        }
        if (node.hasOwnProperty('options')) {
          if (node.options.hasOwnProperty('countApproximate')) {
            viewAnnotation += '. Count mode is ' + node.options.countApproximate;
          }
        }
        viewAnnotation += ' */';
        let viewVariables = '';
        if (node.hasOwnProperty('viewValuesVars') && node.viewValuesVars.length > 0) {
          viewVariables = node.viewValuesVars.map(function (viewValuesColumn) {
            if (viewValuesColumn.hasOwnProperty('field')) {
              return keyword(' LET ') + variableName(viewValuesColumn) + ' = ' + variableName(node.outVariable) + '.' + attributePath(viewValuesColumn.field);
            } else {
              return viewValuesColumn.viewStoredValuesVars.map(function (viewValuesVar) {
                return keyword(' LET ') + variableName(viewValuesVar) + ' = ' + variableName(node.outVariable) + '.' + attributePath(viewValuesVar.field);
              }).join('');
            }
          }).join('');
        }
        return keyword('FOR ') + variableName(node.outVariable) + keyword(' IN ') +
               view(node.view) + condition + sortCondition + scorers + viewVariables +
               scorersSort + '   ' + annotation(viewAnnotation);
      case 'JoinNode':
        node.indexInfos.forEach((info) => {
          collectionVariables[info.outVariable.id] = info.collection;
          let condition = '';
          if (info.condition && info.condition.hasOwnProperty('type')) {
            condition = buildExpression(info.condition);
          }
          info.index.condition = condition;
          iterateIndexes(info.index, 0, {id: node.id, collection: info.collection}, types, false); 
        });
        return keyword('JOIN');
      case 'IndexNode':
        var limit = '';
        collectionVariables[node.outVariable.id] = node.collection;
        if (node.filter) {
          filter = '   ' + keyword('FILTER') + ' ' + buildExpression(node.filter) + '   ' + annotation('/* early pruning */');
        }
        if (node.limit > 0) {
          limit = '   '  + keyword('LIMIT') + ' ' + value(JSON.stringify(node.limit)) + ' ' + annotation('/* early reducing results */');
        }
        if (node.projections) {
          // produce LET nodes for each projection output register
          let parts = [];
          node.projections.forEach((p) => {
            if (p.hasOwnProperty('variable')) {
              parts.push(variableName(p.variable) + ' = ' + variableName(node.outVariable) + '.' + p.path.map((p) => attribute(p)).join('.'));
            }
          });
          if (parts.length) {
            filter = '   ' + keyword('LET') + ' ' + parts.join(', ') + filter;
          }
        }
        let indexAnnotation = '';
        let indexVariables = '';
        if (node.hasOwnProperty('outNmDocId')) {
          indexAnnotation += '/* with late materialization */';
          if (node.hasOwnProperty('indexValuesVars') && node.indexValuesVars.length > 0) {
            indexVariables = node.indexValuesVars.map(function (indexValuesVar) {
              return keyword(' LET ') + variableName(indexValuesVar) + ' = ' + variableName(node.outVariable) + '.' + attribute(indexValuesVar.field);
            }).join('');
          }
        }
        if (node.count) {
          indexAnnotation += '/* with count optimization */';
        }
        node.indexes.forEach(function (idx, i) { iterateIndexes(idx, i, node, types, false); });
        return `${keyword('FOR')} ${variableName(node.outVariable)} ${keyword('IN')} ${collection(node.collection)}` + indexVariables +
          `   ${annotation(`/* ${types.join(', ')}${projections(node, 'filterProjections', 'filter projections')}${projections(node, 'projections', 'projections')}${node.satellite ? ', satellite' : ''}${restriction(node)} */`)} ` + filter + limit +
          '   ' + annotation(indexAnnotation);

      case 'TraversalNode': {
        if (node.hasOwnProperty("options")) {
          node.minMaxDepth = node.options.minDepth + '..' + node.options.maxDepth;
        } else if (node.hasOwnProperty("traversalFlags")) {
          node.minMaxDepth = node.traversalFlags.minDepth + '..' + node.traversalFlags.maxDepth;
        } else {
          node.minMaxDepth = '1..1';
        }
        node.minMaxDepthLen = node.minMaxDepth.length;

        rc = keyword('FOR ');
        if (node.hasOwnProperty('vertexOutVariable')) {
          if (node.options.hasOwnProperty('produceVertices') && !node.options.produceVertices) {
            parts.push(variableName(node.vertexOutVariable) + '  ' + annotation('/* vertex optimized away */'));
          } else {
            parts.push(variableName(node.vertexOutVariable) + '  ' + annotation('/* vertex' + projections(node.options, 'vertexProjections', 'projections') + ' */'));
          }
        } else {
          parts.push(annotation('/* vertex optimized away */'));
        }
        if (node.hasOwnProperty('edgeOutVariable')) {
          parts.push(variableName(node.edgeOutVariable) + '  ' + annotation('/* edge' + projections(node.options, 'edgeProjections', 'projections') + ' */'));
        }
        if (node.hasOwnProperty('pathOutVariable')) {
          let pathParts = [];
          if (node.options.producePathsVertices) {
            if (node.hasOwnProperty('vertexOutVariable')) {
              pathParts.push("vertices");
            } else {
              pathParts.push("vertices" + projections(node.options, 'vertexProjections', 'projections'));
            }
          }
          if (node.options.producePathsEdges) {
            if (node.hasOwnProperty('edgeOutVariable')) {
              pathParts.push("edges");
            } else {
              pathParts.push("edges" + projections(node.options, 'edgeProjections', 'projections'));
            }
          }
          if (node.options.producePathsWeights) {
            pathParts.push("weights");
          }
          if (pathParts.length === 3) {
            pathParts = '';
          } else {
            pathParts = ': ' + pathParts.join(', ');
          }

          parts.push(variableName(node.pathOutVariable) + '  ' + annotation('/* paths' + pathParts + ' */'));
        }

        rc += parts.join(', ') + ' ' + keyword('IN') + ' ' +
          value(node.minMaxDepth) + '  ' + annotation('/* min..maxPathDepth */') + ' ';

        directions = [];
        for (i = 0; i < node.edgeCollections.length; ++i) {
          isLast = (i + 1 === node.edgeCollections.length);
          if (!isLast && node.edgeCollections[i] === node.edgeCollections[i + 1]) {
            // direction ANY is represented by two traversals: an INBOUND and an OUTBOUND traversal
            // on the same collection
            directions.push({ collection: node.edgeCollections[i], direction: 0 /* ANY */ });
            ++i;
          } else {
            directions.push({ collection: node.edgeCollections[i], direction: node.directions[i] });
          }
        }

        rc += keyword(translate[node.defaultDirection]);
        if (node.hasOwnProperty('vertexId')) {
          rc += " '" + value(node.vertexId) + "' ";
        } else {
          rc += ' ' + variableName(node.inVariable) + ' ';
        }
        rc += annotation('/* startnode */') + '  ';

        if (Array.isArray(node.graph)) {
          if (directions.length > 0) {
            rc += collection(directions[0].collection);
            for (i = 1; i < directions.length; ++i) {
              rc += ', ' + keyword(translate[directions[i].direction]) + ' ' + collection(directions[i].collection);
            }
          } else {
            rc += annotation('/* no edge collections */');
          }
        } else {
          rc += keyword('GRAPH') + " '" + value(node.graph) + "'";
        }

        traversalDetails.push(node);
        if (node.hasOwnProperty('condition')) {
          node.ConditionStr = buildExpression(node.condition);
        }
        if (node.hasOwnProperty('expression')) {
          node.PruneConditionStr = buildExpression(node.expression);
        }

        handleGraphDefinition(node);

        indexes.push(...getGraphNodeIndexes(node));

        if (node.isLocalGraphNode) {
          if (node.isUsedAsSatellite) {
            rc += annotation(' /* local graph node, used as satellite */');
          } else {
            rc += annotation(' /* local graph node */');
          }
        }
        if (node.options && node.options.hasOwnProperty('parallelism') && node.options.parallelism > 1) {
          rc += annotation(' /* parallelism: ' + node.options.parallelism + ' */');
        }
        if (node.options && node.options.order) {
          let order = node.options.order;
          if (node.options.order === 'weighted') {
            order += ', weight attribute: ' + node.options.weightAttribute;
          }
          rc += annotation(' /* order: ' + order + ' */');
        }

        return rc;
      }
      case 'ShortestPathNode': {
        if (node.hasOwnProperty('vertexOutVariable')) {
          parts.push(variableName(node.vertexOutVariable) + '  ' + annotation('/* vertex */'));
        }
        if (node.hasOwnProperty('edgeOutVariable')) {
          parts.push(variableName(node.edgeOutVariable) + '  ' + annotation('/* edge */'));
        }
        let defaultDirection = node.defaultDirection;
        rc = `${keyword("FOR")} ${parts.join(", ")} ${keyword("IN")} ${keyword(translate[defaultDirection])} ${keyword("SHORTEST_PATH")} `;
        if (node.hasOwnProperty('startVertexId')) {
          rc += `'${value(node.startVertexId)}'`;
        } else {
          rc += variableName(node.startInVariable);
        }
        rc += ` ${annotation("/* startnode */")} ${keyword("TO")} `;

        if (node.hasOwnProperty('targetVertexId')) {
          rc += `'${value(node.targetVertexId)}'`;
        } else {
          rc += variableName(node.targetInVariable);
        }
        rc += ` ${annotation("/* targetnode */")} `;

        if (Array.isArray(node.graph)) {
          directions = [];
          for (i = 0; i < node.edgeCollections.length; ++i) {
            isLast = (i + 1 === node.edgeCollections.length);
            d = node.directions[i];
            if (!isLast && node.edgeCollections[i] === node.edgeCollections[i + 1]) {
              // direction ANY is represented by two traversals: an INBOUND and an OUTBOUND traversal
              // on the same collection
              d = 0; // ANY
              ++i;
            }
            directions.push({ collection: node.edgeCollections[i], direction: d });
          }
          rc += directions.map(function (g, index) {
            let tmp = '';
            if (g.direction !== defaultDirection) {
              tmp += keyword(translate[g.direction]);
              tmp += ' ';
            }
            return tmp + collection(g.collection);
          }).join(', ');
        } else {
          rc += keyword('GRAPH') + " '" + value(node.graph) + "'";
        }

        shortestPathDetails.push(node);

        handleGraphDefinition(node);

        indexes.push(...getGraphNodeIndexes(node));

        return rc;
      }
      case 'EnumeratePathsNode': {
        if (node.hasOwnProperty('pathOutVariable')) {
          parts.push(variableName(node.pathOutVariable) + '  ' + annotation('/* path */'));
        }
        let defaultDirection = node.defaultDirection;
        let shortestPathType = node.shortestPathType || 'K_SHORTEST_PATHS';
        let depth = '';
        if (shortestPathType === 'K_PATHS') {
          if (node.hasOwnProperty("options")) {
            depth = value(node.options.minDepth + '..' + node.options.maxDepth);
          } else {
            depth = value('1..1');
          }
          depth += '  ' + annotation('/* min..maxPathDepth */') + ' ';
        }
        rc = `${keyword("FOR")} ${parts.join(", ")} ${keyword("IN")} ${depth}${keyword(translate[defaultDirection])} ${keyword(shortestPathType)} `;
        if (node.hasOwnProperty('startVertexId')) {
          rc += `'${value(node.startVertexId)}'`;
        } else {
          rc += variableName(node.startInVariable);
        }
        rc += ` ${annotation("/* startnode */")} ${keyword("TO")} `;

        if (node.hasOwnProperty('targetVertexId')) {
          rc += `'${value(node.targetVertexId)}'`;
        } else {
          rc += variableName(node.targetInVariable);
        }
        rc += ` ${annotation("/* targetnode */")} `;

        if (Array.isArray(node.graph)) {
          directions = [];
          for (i = 0; i < node.edgeCollections.length; ++i) {
            isLast = (i + 1 === node.edgeCollections.length);
            d = node.directions[i];
            if (!isLast && node.edgeCollections[i] === node.edgeCollections[i + 1]) {
              // direction ANY is represented by two traversals: an INBOUND and an OUTBOUND traversal
              // on the same collection
              d = 0; // ANY
              ++i;
            }
            directions.push({ collection: node.edgeCollections[i], direction: d });
          }
          rc += directions.map(function (g, index) {
            let tmp = '';
            if (g.direction !== defaultDirection) {
              tmp += keyword(translate[g.direction]);
              tmp += ' ';
            }
            return tmp + collection(g.collection);
          }).join(', ');
        } else {
          rc += keyword('GRAPH') + " '" + value(node.graph) + "'";
        }

        kShortestPathsDetails.push(node);
        
        handleGraphDefinition(node);

        indexes.push(...getGraphNodeIndexes(node));

        return rc;
      }
      case 'CalculationNode':
        (node.functions || []).forEach(function (f) {
          functions[f.name] = f;
        });
        return keyword('LET') + ' ' + variableName(node.outVariable) + ' = ' + buildExpression(node.expression) + '   ' + annotation('/* ' + node.expressionType + ' expression */') + variablesUsed() + constNess();
      case 'FilterNode':
        return keyword('FILTER') + ' ' + variableName(node.inVariable);
      case 'AggregateNode': /* old-style COLLECT node */
        return keyword('COLLECT') + ' ' + node.aggregates.map(function (node) {
          return variableName(node.outVariable) + ' = ' + variableName(node.inVariable);
        }).join(', ') +
          (node.count ? ' ' + keyword('WITH COUNT') : '') +
          (node.outVariable ? ' ' + keyword('INTO') + ' ' + variableName(node.outVariable) : '') +
          (node.keepVariables ? ' ' + keyword('KEEP') + ' ' + node.keepVariables.map(function (variable) { return variableName(variable); }).join(', ') : '') +
          '   ' + annotation('/* ' + node.aggregationOptions.method + ' */');
      case 'CollectNode':
        var collect = keyword('COLLECT') + ' ' +
          node.groups.map(function (node) {
            return variableName(node.outVariable) + ' = ' + variableName(node.inVariable);
          }).join(', ');

        if (node.hasOwnProperty('aggregates') && node.aggregates.length > 0) {
          if (node.groups.length > 0) {
            collect += ' ';
          }
          collect += keyword('AGGREGATE') + ' ' +
            node.aggregates.map(function (node) {
              return variableName(node.outVariable) + ' = ' + func(node.type) + '(' + (node.inVariable ? variableName(node.inVariable) : '') + ')';
            }).join(', ');
        }
        collect +=
          (node.count ? ' ' + keyword('WITH COUNT') : '') +
          (node.outVariable ? ' ' + keyword('INTO') + ' ' + variableName(node.outVariable) : '') +
          ((node.expressionVariable && node.outVariable) ? " = " + variableName(node.expressionVariable) : "") +
          (node.keepVariables ? ' ' + keyword('KEEP') + ' ' + node.keepVariables.map(function (variable) { return variableName(variable.variable); }).join(', ') : '') +
          '   ' + annotation('/* ' + node.collectOptions.method + ' */');
        return collect;
      case 'IndexCollectNode':
        iterateIndexes(node.index, 0, node, types, false);
        let indexCollect = keyword('FOR ') + variableName(node.oldIndexVariable) + keyword(' IN ') + collection(node.collection) + keyword(' COLLECT ') + node.groups.map(function (grp) {
          return variableName(grp.outVariable) + ' = ' + variableName(node.oldIndexVariable) + '.' + grp.attribute.map((p) => attribute(p)).join('.');
        }).join(', ');
        if (node.aggregations.length > 0) {
          indexCollect += keyword(' AGGREGATE') + ' ' +
              node.aggregations.map(function (node) {
                return variableName(node.outVariable) + ' = ' + func(node.type) + '(' + buildExpression(node.expression) + ')';
              }).join(', ') + annotation(' /* full index scan */');
        } else {
          indexCollect += annotation(' /* distinct value index scan */');
        }
        return indexCollect;
      case 'SortNode':
        const groupedElements = node.numberOfTopGroupedElements > 0 ?
          '; ' + keyword('GROUPED BY') + ' '
          + node.elements.slice(0, node.numberOfTopGroupedElements).map((element) => variableName(element.inVariable)).join(', ') : '';
        return keyword('SORT') + ' '
          + node.elements.slice(node.numberOfTopGroupedElements, node.elements.length).map((node) => variableName(node.inVariable) + ' ' + keyword(node.ascending ? 'ASC' : 'DESC')).join(', ')
          + groupedElements
          + annotation(`   /* sorting strategy: ${node.strategy.split("-").join(" ")} */`);
      case 'LimitNode':
        return keyword('LIMIT') + ' ' + value(JSON.stringify(node.offset)) + ', ' + value(JSON.stringify(node.limit)) + (node.fullCount ? '  ' + annotation('/* fullCount */') : '');
      case 'ReturnNode':
        return keyword('RETURN') + ' ' + variableName(node.inVariable);
      case 'SubqueryNode':
        return keyword('LET') + ' ' + variableName(node.outVariable) + ' = ...   ' + annotation('/* ' + (node.isConst ? 'const ' : '') + 'subquery */');
      case 'SubqueryStartNode':
        return `${keyword('LET')} ${variableName(node.subqueryOutVariable)} = ( ${annotation(`/* subquery begin */`)}` ;
      case 'SubqueryEndNode':
        if (node.inVariable) {
          return `${keyword('RETURN')}  ${variableName(node.inVariable)} ) ${annotation(`/* subquery end */`)}`;
        }
        // special hack for spliced subqueries that do not return any data
        return `) ${annotation(`/* subquery end */`)}`;
      case 'InsertNode': {
        modificationFlags = node.modificationFlags;
        let restrictString = '';
        if (node.restrictedTo) {
          restrictString = annotation('/* ' + restriction(node) + ' */');
        }
        return keyword('INSERT') + ' ' + variableName(node.inVariable) + ' ' + keyword('IN') + ' ' + collection(node.collection) + ' ' + restrictString;
      }
      case 'UpdateNode': {
        modificationFlags = node.modificationFlags;
        let inputExplain = '';
        let indexRef = '';
        if (node.hasOwnProperty('inKeyVariable')) {
          indexRef = `${variableName(node.inKeyVariable)}`;
          inputExplain = `${variableName(node.inKeyVariable)} ${keyword('WITH')} ${variableName(node.inDocVariable)}`;
        } else {
          indexRef = inputExplain = `${variableName(node.inDocVariable)}`;
        }
        let restrictString = '';
        if (node.restrictedTo) {
          restrictString = annotation('/* ' + restriction(node) + ' */');
        }
        node.indexes.forEach(function (idx, i) { iterateIndexes(idx, i, node, types, indexRef); });
        return `${keyword('UPDATE')} ${inputExplain} ${keyword('IN')} ${collection(node.collection)} ${restrictString}`;
      }
      case 'ReplaceNode': {
        modificationFlags = node.modificationFlags;
        let inputExplain = '';
        let indexRef = '';
        if (node.hasOwnProperty('inKeyVariable')) {
          indexRef = `${variableName(node.inKeyVariable)}`;
          inputExplain = `${variableName(node.inKeyVariable)} ${keyword('WITH')} ${variableName(node.inDocVariable)}`;
        } else {
          indexRef = inputExplain = `${variableName(node.inDocVariable)}`;
        }
        let restrictString = '';
        if (node.restrictedTo) {
          restrictString = annotation('/* ' + restriction(node) + ' */');
        }
        node.indexes.forEach(function (idx, i) { iterateIndexes(idx, i, node, types, indexRef); });
        return `${keyword('REPLACE')} ${inputExplain} ${keyword('IN')} ${collection(node.collection)} ${restrictString}`;
      }
      case 'UpsertNode':
        modificationFlags = node.modificationFlags;
        let indexRef = `${variableName(node.inDocVariable)}`;
        node.indexes.forEach(function (idx, i) { iterateIndexes(idx, i, node, types, indexRef); });
        return keyword('UPSERT') + ' ' + variableName(node.inDocVariable) + ' ' + keyword('INSERT') + ' ' + variableName(node.insertVariable) + ' ' + keyword(node.isReplace ? 'REPLACE' : 'UPDATE') + ' ' + variableName(node.updateVariable) + ' ' + keyword('IN') + ' ' + collection(node.collection);
      case 'RemoveNode': {
        modificationFlags = node.modificationFlags;
        let restrictString = '';
        if (node.restrictedTo) {
          restrictString = annotation('/* ' + restriction(node) + ' */');
        }
        let indexRef = `${variableName(node.inVariable)}`;
        node.indexes.forEach(function (idx, i) { iterateIndexes(idx, i, node, types, indexRef); });
        return `${keyword('REMOVE')} ${variableName(node.inVariable)} ${keyword('IN')} ${collection(node.collection)} ${restrictString}`;
      }
      case 'SingleRemoteOperationNode': {
        switch (node.mode) {
          case "IndexNode": {
            collectionVariables[node.outVariable.id] = node.collection;
            let indexRef = `${variable(JSON.stringify(node.key))}`;
            node.indexes.forEach(function (idx, i) { iterateIndexes(idx, i, node, types, indexRef); });
            return `${keyword('FOR')} ${variableName(node.outVariable)} ${keyword('IN')} ${collection(node.collection)} ${keyword('FILTER')} ${variableName(node.outVariable)}.${attribute('_key')} == ${indexRef} ${annotation(`/* primary index scan */`)}`;
            // `
          }
          case 'InsertNode': {
            modificationFlags = node.modificationFlags;
            collectionVariables[node.inVariable.id] = node.collection;
            let indexRef = `${variableName(node.inVariable)}`;
            if (node.hasOwnProperty('indexes')) {
              node.indexes.forEach(function (idx, i) { iterateIndexes(idx, i, node, types, indexRef); });
            }
            return `${keyword('INSERT')} ${variableName(node.inVariable)} ${keyword('IN')} ${collection(node.collection)}`;
          }
          case 'UpdateNode': {
            modificationFlags = node.modificationFlags;
            let OLD = "";
            if (node.hasOwnProperty('inVariable')) {
              collectionVariables[node.inVariable.id] = node.collection;
              OLD = `${keyword('WITH')} ${variableName(node.inVariable)} `;
            }
            let indexRef;
            let keyCondition = "";
            let filterCondition;
            if (node.hasOwnProperty('key')) {
              keyCondition = `{ ${value('_key')} : ${variable(JSON.stringify(node.key))} } `;
              indexRef = `${variable(JSON.stringify(node.key))} `;
              filterCondition = `${variable('doc')}.${attribute('_key')} == ${variable(JSON.stringify(node.key))}`;
            } else if (node.hasOwnProperty('inVariable')) {
              keyCondition = `${variableName(node.inVariable)} `;
              indexRef = `${variableName(node.inVariable)}`;
            } else {
              keyCondition = "<UNSUPPORTED>";
              indexRef = "<UNSUPPORTED>";
            }
            if (node.hasOwnProperty('indexes')) {
              node.indexes.forEach(function (idx, i) { iterateIndexes(idx, i, node, types, indexRef); });
            }
            let forStatement = "";
            if (node.replaceIndexNode) {
              forStatement = `${keyword('FOR')} ${variable('doc')} ${keyword('IN')} ${collection(node.collection)} ${keyword('FILTER')} ${filterCondition} `;
              keyCondition = `${variable('doc')} `;
            }
            return `${forStatement}${keyword('UPDATE')} ${keyCondition}${OLD}${keyword('IN')} ${collection(node.collection)}`;
          }
          case 'ReplaceNode': {
            modificationFlags = node.modificationFlags;
            let OLD = "";
            if (node.hasOwnProperty('inVariable')) {
              collectionVariables[node.inVariable.id] = node.collection;
              OLD = `${keyword('WITH')} ${variableName(node.inVariable)} `;
            }
            let indexRef;
            let keyCondition = "";
            let filterCondition;
            if (node.hasOwnProperty('key')) {
              keyCondition = `{ ${value('_key')} : ${variable(JSON.stringify(node.key))} } `;
              indexRef = `${variable(JSON.stringify(node.key))}`;
              filterCondition = `${variable('doc')}.${attribute('_key')} == ${variable(JSON.stringify(node.key))}`;
            } else if (node.hasOwnProperty('inVariable')) {
              keyCondition = `${variableName(node.inVariable)} `;
              indexRef = `${variableName(node.inVariable)}`;
            } else {
              keyCondition = "<UNSUPPORTED>";
              indexRef = "<UNSUPPORTED>";
            }
            if (node.hasOwnProperty('indexes')) {
              node.indexes.forEach(function (idx, i) { iterateIndexes(idx, i, node, types, indexRef); });
            }
            let forStatement = "";
            if (node.replaceIndexNode) {
              forStatement = `${keyword('FOR')} ${variable('doc')} ${keyword('IN')} ${collection(node.collection)} ${keyword('FILTER')} ${filterCondition} `;
              keyCondition = `${variable('doc')} `;
            }
            return `${forStatement}${keyword('REPLACE')} ${keyCondition}${OLD}${keyword('IN')} ${collection(node.collection)}`;
          }
          case 'RemoveNode': {
            modificationFlags = node.modificationFlags;
            if (node.hasOwnProperty('inVariable')) {
              collectionVariables[node.inVariable.id] = node.collection;
            }
            let indexRef;
            let keyCondition;
            let filterCondition;
            if (node.hasOwnProperty('key')) {
              keyCondition = `{ ${value('_key')} : ${variable(JSON.stringify(node.key))} } `;
              indexRef = `${variable(JSON.stringify(node.key))}`;
              filterCondition = `${variable('doc')}.${attribute('_key')} == ${variable(JSON.stringify(node.key))}`;
            } else if (node.hasOwnProperty('inVariable')) {
              keyCondition = `${variableName(node.inVariable)} `;
              indexRef = `${variableName(node.inVariable)}`;
            } else {
              keyCondition = "<UNSUPPORTED>";
              indexRef = "<UNSUPPORTED>";
            }
            if (node.hasOwnProperty('indexes')) {
              node.indexes.forEach(function (idx, i) { iterateIndexes(idx, i, node, types, indexRef); });
            }
            let forStatement = "";
            if (node.replaceIndexNode) {
              forStatement = `${keyword('FOR')} ${variable('doc')} ${keyword('IN')} ${collection(node.collection)} ${keyword('FILTER')} ${filterCondition} `;
              keyCondition = `${variable('doc')} `;
            }
            return `${forStatement}${keyword('REMOVE')} ${keyCondition}${keyword('IN')} ${collection(node.collection)}`;
          }
        }
      }
        break;
      case 'MultipleRemoteModificationNode': {
        modificationFlags = node.modificationFlags;
        collectionVariables[node.inVariable.id] = node.collection;
        let indexRef = `${variableName(node.inVariable)}`;
        if (node.hasOwnProperty('indexes')) {
          node.indexes.forEach(function (idx, i) {
            iterateIndexes(idx, i, node, types, indexRef);
          });
        }
        return `${keyword('FOR')} d ${keyword('IN')} ${variableName(node.inVariable)} ${keyword('INSERT')} d ${keyword('IN')} ${collection(node.collection)}`;
      }
        break;

      case 'RemoteNode':
        return keyword('REMOTE');
      case 'DistributeNode':
        return keyword('DISTRIBUTE') + ' ' + variableName(node.variable);
      case 'ScatterNode':
        return keyword('SCATTER');
      case 'GatherNode':
        let gatherAnnotations = [];
        if (node.parallelism === 'parallel') {
          gatherAnnotations.push('parallel');
        }
        if (node.sortmode !== 'unset') {
          gatherAnnotations.push('sort mode: ' + node.sortmode);
        }
        if (node.elements.length === 0) {
          gatherAnnotations.push('unsorted');
        }
        return keyword('GATHER') + ' ' + node.elements.map(function (node) {
          if (node.path && node.path.length) {
            return variableName(node.inVariable) + node.path.map(function (n) { return '.' + attribute(n); }) + ' ' + keyword(node.ascending ? 'ASC' : 'DESC');
          }
          return variableName(node.inVariable) + ' ' + keyword(node.ascending ? 'ASC' : 'DESC');
        }).join(', ') + (gatherAnnotations.length ? '  ' + annotation('/* ' + gatherAnnotations.join(', ') + ' */') : '');

      case 'MaterializeNode':
        let annotations = '';
        let accessString = '';
        if (node.projections) {
          annotations += projections(node, 'projections', 'projections');
          // produce LET nodes for each projection output register
          let parts = [];
          node.projections.forEach((p) => {
            if (p.hasOwnProperty('variable')) {
              parts.push(variableName(p.variable) + ' = ' + variableName(node.outVariable) + '.' + p.path.map((p) => attribute(p)).join('.'));
            }
          });
          if (parts.length) {
            accessString = '   ' + keyword('LET') + ' ' + parts.join(', ') + accessString;
          }
        }
        let varString = '';
        if (node.hasOwnProperty('oldDocVariable')) {
          if (node.outVariable.id !== node.oldDocVariable.id) {
            varString = variableName(node.oldDocVariable) + ' ' + keyword('INTO') + ' ' + variableName(node.outVariable);
          } else {
            varString = variableName(node.oldDocVariable);
          }
        } else {
          varString = variableName(node.outVariable);
        }
        return keyword('MATERIALIZE') + ' ' + varString + (annotations.length > 0 ? annotation(` /*${annotations} */`) : '') + accessString;
      case 'OffsetMaterializeNode':
        return keyword('LET ') + variableName(node.outVariable) + ' = ' +
               func('OFFSET_INFO') + '(' + variableName(node.viewVariable) + ', ' + buildExpression(node.options) + ')';
      case 'MutexNode':
        return keyword('MUTEX') + '   ' + annotation('/* end async execution */');
      case 'AsyncNode':
        return keyword('ASYNC') + '   ' + annotation('/* begin async execution */');
      case 'WindowNode': {
        var window = keyword('WINDOW') + ' ';
        if (node.rangeVariable) {
          window += variableName(node.rangeVariable) + ' ' + keyword("WITH") + ' ';
        }
        window += `{ preceding: ${JSON.stringify(node.preceding)}, following: ${JSON.stringify(node.following)} } `;
        if (node.hasOwnProperty('aggregates') && node.aggregates.length > 0) {
          window += keyword('AGGREGATE') + ' ' +
            node.aggregates.map(function (node) {
              let aggregateString = variableName(node.outVariable) + ' = ' + func(node.type) + '(';
              if (node.inVariable !== undefined) {
                aggregateString += variableName(node.inVariable);
              }
              aggregateString += ')';
              return aggregateString;
            }).join(', ');
        }
        return window;
      }
    }

    return 'unhandled node type (' + node.type + ')';
  };

  var level = 0, subqueries = [], subqueryCallbacks = [];
  const indent = function (level, isRoot) {
    return pad(1 + level + level) + (isRoot ? '* ' : '- ');
  };

  const nodePrefix = (node) => {
    let line = ' ' +
      pad(1 + maxIdLen - String(node.id).length) + variable(node.id) + '   ' +
      keyword(node.type) + pad(1 + maxTypeLen - String(node.type).length) + '   ';

    if (isCoord) {
      line += variable(node.site) + pad(1 + maxSiteLen - String(node.site).length) + '  ';
    }
    return line;
  };

  const preHandle = function (node) {
    usedVariables = {};
    currentNode = node.id;
    isConst = true;
    if (node.type === 'SubqueryNode' || node.type === 'SubqueryStartNode') {
      subqueries.push(level);
    }
    if (node.type === 'SubqueryEndNode' && !node.inVariable && subqueries.length > 0) {
      // special hack for spliced subqueries that do not return any data
      --level;
    }
  };

  const postHandle = function (node) {
    if (node.type === 'JoinNode') {
      ++level;
      node.indexInfos.forEach((info) => {
        let line = nodePrefix(node);
        if (profileMode) {
          line += pad(1 + maxCallsLen) +  '   ' +
            pad(1 + maxParallelLen) + '   ' +
            pad(1 + maxItemsLen) + '   ' +
            pad(1 + maxFilteredLen) + '   ' +
            pad(1 + maxRuntimeLen) + '   ';
        } else {
          line += pad('Par'.length) + value(info.isAsyncPrefetchEnabled ? '✓' : ' ') + '   ' +
            pad(1 + maxEstimateLen - String(info.estimatedNrItems).length) + value(info.estimatedNrItems) + '   ';
        }
        let label = keyword('FOR ') + variableName(info.outVariable) + keyword(' IN ') + collection(info.collection);
        let filter = '';
        if (info.filter && info.filter.hasOwnProperty('type')) {
          filter = '   ' + keyword('FILTER') + ' ' + buildExpression(info.filter);
        }
        let accessString = '';
        if (!info.indexCoversProjections && info.producesOutput) {
          accessString += "index scan + document lookup";
        } else if (info.indexCoversProjections) {
          accessString += "index scan";
        } else {
          accessString += "index scan only";
        }
        if (info.isLateMaterialized === true) {
          accessString += ' with late materialization';
        }
        if (info.projections) {
          accessString += projections(info, "projections", "projections");
        }
        if (info.filterProjections) {
          accessString += projections(info, 'filterProjections', 'filter projections');
        }
        accessString = '   ' + annotation('/* ' + accessString + ' */');
        if (info.projections) {
          // produce LET nodes for each projection output register
          let parts = [];
          info.projections.forEach((p) => {
            if (p.hasOwnProperty('variable')) {
              parts.push(variableName(p.variable) + ' = ' + variableName(info.outVariable) + '.' + p.path.map((p) => attribute(p)).join('.'));
            }
          });
          if (parts.length) {
            accessString = '   ' + keyword('LET') + ' ' + parts.join(', ') + accessString;
          }
        }
        line += indent(level, false) + label + filter + accessString;
        stringBuilder.appendLine(line);
      });
      --level;
    }
    if (node.type === 'SubqueryEndNode' && subqueries.length > 0) {
      level = subqueries.pop();
    }
    const isLeafNode = !parents.hasOwnProperty(node.id);

    if (['EnumerateCollectionNode',
      'EnumerateListNode',
      'EnumerateViewNode',
      'IndexNode',
      'TraversalNode',
      'SubqueryStartNode',
      'SubqueryNode'].indexOf(node.type) !== -1) {
      level++;
    } else if (isLeafNode && subqueries.length > 0) {
      level = subqueries.pop();
    } else if (node.type === 'SingletonNode') {
      level++;
    }
  };

  const callstackSplit = function(node) {
    if (node.isCallstackSplitEnabled) {
      return annotation(' /* callstack split */');
    }
    return '';
  };

  const constNess = function () {
    if (isConst) {
      return '   ' + annotation('/* const assignment */');
    }
    return '';
  };

  const variablesUsed = function () {
    let used = [];
    for (let a in usedVariables) {
      if (usedVariables.hasOwnProperty(a)) {
        used.push(variable(a) + ' : ' + collection(usedVariables[a]));
      }
    }
    if (used.length > 0) {
      return '   ' + annotation('/* collections used:') + ' ' + used.join(', ') + ' ' + annotation('*/');
    }
    return '';
  };

  const isCoord = isCoordinator();

  const printNode = function (node) {
    preHandle(node);
    let line = nodePrefix(node); 

    if (profileMode) {
      line += pad(1 + maxCallsLen - String(node.calls).length) + value(node.calls) + '   ' +
        pad(1 + maxParallelLen - String(node.parallel).length) + value(node.parallel) + '   ' +
        pad(1 + maxItemsLen - String(node.items).length) + value(node.items) + '   ' +
        pad(1 + maxFilteredLen - String(node.filtered).length) + value(node.filtered) + '   ' +
        pad(1 + maxRuntimeLen - node.runtime.length) + value(node.runtime) + '   ';
    } else {
      line += pad('Par'.length) + value(node.isAsyncPrefetchEnabled ? '✓' : ' ') + '   ' +
        pad(1 + maxEstimateLen - String(node.estimatedNrItems).length) + value(node.estimatedNrItems) + '   ';
    }
    line += indent(level, node.type === 'SingletonNode') + label(node) + callstackSplit(node);

    stringBuilder.appendLine(line);
    postHandle(node);
  };

  if (planIndex === undefined) {
    printQuery(query, explain.cacheable, explain.planCacheKey);
  }

  stringBuilder.appendLine(section('Execution plan:'));

  let line = ' ' +
    pad(1 + maxIdLen - String('Id').length) + header('Id') + '   ' +
    header('NodeType') + pad(1 + maxTypeLen - String('NodeType').length) + '   ';

  if (isCoord) {
    line += header('Site') + pad(1 + maxSiteLen - String('Site').length) + '  ';
  }

  if (profileMode) {
    line += pad(1 + maxCallsLen - String('Calls').length) + header('Calls') + '   ' +
      pad(1 + maxParallelLen - String('Par').length) + header('Par') + '   ' +
      pad(1 + maxItemsLen - String('Items').length) + header('Items') + '   ' +
      pad(1 + maxFilteredLen - String('Filtered').length) + header('Filtered') + '   ' +
      pad(1 + maxRuntimeLen - String('Runtime [s]').length) + header('Runtime [s]') + '   ' +
      header('Comment');
  } else {
    line += 
      pad(1 + maxParallelLen - String('Par').length) + header('Par') + '   ' +
      pad(1 + maxEstimateLen - String('Est.').length) + header('Est.') + '   ' +
      header('Comment');
  }


  stringBuilder.appendLine(line);

  var walk = [rootNode];
  while (walk.length > 0) {
    var id = walk.pop();
    var node = nodes[id];
    printNode(node);
    if (parents.hasOwnProperty(id)) {
      walk = walk.concat(parents[id]);
    }
    if (node.type === 'SubqueryNode') {
      walk = walk.concat([node.subquery.nodes[0].id]);
    }
  }

  stringBuilder.appendLine();
  printIndexes(indexes);
  printFunctions(functions);
  printTraversalDetails(traversalDetails);
  printShortestPathDetails(shortestPathDetails);
  printKShortestPathsDetails(kShortestPathsDetails);
  stringBuilder.appendLine();

  printRules(plan.rules, explain.stats);
  // printing of "write query options" normally does not help to get any
  // better insights, so it has been deactivated. the code is kept around
  // in case we still need it later.
  // printModificationFlags(modificationFlags);
  if (profileMode) {
    printStats(explain.stats, isCoord);
    printProfile(explain.profile);
  }
  printWarnings(explain.warnings);
}

/* the exposed explain function */
function explain(data, options, shouldPrint) {
  'use strict';
  // we need to clone here because options are modified later
  if (typeof data === 'string') {
    data = { query: data, options: _.clone(options) };
  }
  if (!(data instanceof Object)) {
    throw 'ArangoStatement needs initial data';
  }

  if (options === undefined) {
    options = _.clone(data.options);
  }
  options = options || {};
  options.explainInternals = false;
  options.verbosePlans = true;
  setColors(options.colors === undefined ? true : options.colors);

  if (!options.profile) {
    options.profile = 2;
  }
  stringBuilder.clearOutput();
  let stmt = db._createStatement(data);
  let result = stmt.explain(options);
  if (options.allPlans === true) {
    // multiple plans
    printQuery(data.query);
    for (let i = 0; i < result.plans.length; ++i) {
      if (i > 0) {
        stringBuilder.appendLine();
      }
      stringBuilder.appendLine(section("Plan #" + (i + 1) + " of " + result.plans.length + " (estimated cost: " + result.plans[i].estimatedCost.toFixed(2) + ")"));
      stringBuilder.prefix = ' ';
      stringBuilder.appendLine();
      processQuery(data.query, result, i);
      stringBuilder.prefix = '';
    }
  } else {
    // single plan
    processQuery(data.query, result, undefined);
  }

  if (shouldPrint === undefined || shouldPrint) {
    internal.print(stringBuilder.getOutput());
  } else {
    return stringBuilder.getOutput();
  }
}


/* the exposed profile query function */
function profileQuery(data, shouldPrint) {
  'use strict';
  if (!(data instanceof Object) || !data.hasOwnProperty("options")) {
    throw 'ArangoStatement needs initial data';
  }
  let options = data.options || {};
  if (!options.hasOwnProperty('silent')) {
    options.silent = true;
  }
  options.allPlans = false; // always turn this off, as it will not work with profiling
  setColors(options.colors === undefined ? true : options.colors);

  stringBuilder.clearOutput();
  let stmt = db._createStatement(data);
  let cursor = stmt.execute();
  let extra = cursor.getExtra();
  if (cursor.planCacheKey !== undefined) {
    extra.planCacheKey = cursor.planCacheKey;
  }
  processQuery(data.query, extra, undefined);

  if (shouldPrint === undefined || shouldPrint) {
    internal.print(stringBuilder.getOutput());
  } else {
    return stringBuilder.getOutput();
  }
}

/* the exposed debug function */
function debug(query, bindVars, options) {
  'use strict';
  let input = {};

  if (query instanceof Object) {
    if (typeof query.toAQL === 'function') {
      query = query.toAQL();
    }
    input = query;
  } else {
    input.query = query;
    if (bindVars !== undefined) {
      input.bindVars = anonymize(bindVars);
    }
    if (options !== undefined) {
      input.options = options;
    }
  }
  if (!input.options) {
    input.options = {};
  }
  input.options.explainRegisters = true;
  let dbProperties = db._properties();
  delete dbProperties.id;
  delete dbProperties.isSystem;
  delete dbProperties.path;

  let result = {
    engine: db._engine(),
    version: db._version(true),
    database: dbProperties,
    query: input,
    queryCache: require('@arangodb/aql/cache').properties(),
    analyzers: {},
    collections: {},
    views: {}
  };

  try {
    // engine stats may be not available in cluster, at least in older versions
    result.engineStats = db._engineStats();
  } catch (err) {
    // we need to ignore any errors here
  }

  result.fancy = require('@arangodb/aql/explainer').explain(input, { colors: false }, false);

  let stmt = db._createStatement(input);
  result.explain = stmt.explain(input.options);

  let graphs = {};
  let collections = result.explain.plan.collections;
  let map = {};
  collections.forEach(function (c) {
    map[c.name] = true;
  });

  // export graphs
  let findGraphs = function (nodes) {
    nodes.forEach(function (node) {
      if (node.type === 'TraversalNode') {
        if (node.graph && node.graphDefinition) {
          // named graphs have their name in "graph", but non-graph traversal queries too
          // the distinction can thus be made only by peeking into graphDefinition, which
          // is only populated for named graphs
          try {
            graphs[node.graph] = db._graphs.document(node.graph);
          } catch (err) { }
        }
        if (node.graphDefinition) {
          try {
            node.graphDefinition.vertexCollectionNames.forEach(function (c) {
              if (!map.hasOwnProperty(c)) {
                map[c] = true;
                collections.push({ name: c });
              }
            });
          } catch (err) { }
          try {
            node.graphDefinition.edgeCollectionNames.forEach(function (c) {
              if (!map.hasOwnProperty(c)) {
                map[c] = true;
                collections.push({ name: c });
              }
            });
          } catch (err) { }
        } else {
          node.graph.forEach(edgeColl => {
            collections.push({name: edgeColl});
          });
        }
      } else if (node.type === 'SubqueryNode') {
        // recurse into subqueries
        findGraphs(node.subquery.nodes);
      }
    });
  };
  // mangle with graphs used in query
  findGraphs(result.explain.plan.nodes);

  let handleCollection = function(collection) {
    let c = db._collection(collection.name);
    if (c === null) {
      // probably a view...
      let v = db._view(collection.name);
      if (v === null) {
        return;
      }

      result.views[collection.name] = {
        type: v.type(),
        properties: v.properties()
      };
    } else if (isNaN(collection.name)) {
      if (c.type() === 3 && collection.name.match(/^_(local|from|to)_.+/)) {
        // an internal smart-graph collection. let's skip this
        return;
      }
      let examples;
      if (input.options.examples) {
        // include example data from collections
        let max = 10; // default number of documents
        if (typeof input.options.examples === 'number') {
          max = input.options.examples;
        }
        if (max > 100) {
          max = 100;
        } else if (max < 0) {
          max = 0;
        }
        examples = db._query("FOR doc IN @@collection LIMIT @max RETURN doc", { max, "@collection": collection.name }).toArray();
        if (input.options.anonymize) {
          examples = examples.map(anonymize);
        }
      }
      result.collections[collection.name] = {
        name: collection.name,
        type: c.type(),
        properties: c.properties(),
        indexes: c.indexes(true),
        count: c.count(),
        counts: c.count(true),
        examples
      };
    }
  };

  // add collection information
  collections.forEach(function (collection) {
    handleCollection(collection);
  });

  // add prototypes used for distributeShardsLike
  let sortedCollections = [];
  Object.values(result.collections).forEach(function(collection) {
    if (collection.properties.distributeShardsLike &&
        !result.collections.hasOwnProperty(collection.distributeShardsLike)) {
      handleCollection({ name: collection.name });
    }
    sortedCollections.push(collection);
  });

  sortedCollections.sort(function(l, r) {
    if (l.properties.distributeShardsLike && !r.properties.distributeShardsLike) {
      return 1;
    } else if (!l.properties.distributeShardsLike && r.properties.distributeShardsLike) {
      return -1;
    }
    if (l.type === 2 && r.type === 3) {
      return -1;
    } else if (r.type === 3 && l.type === 2) {
      return 1;
    }
    if (l.name < r.name) {
      return -1;
    } else if (l.name > r.name) {
      return 1;
    }
    // should not happen
    return 0;
  });

  result.collections = {};
  sortedCollections.forEach(function(c) {
    result.collections[c.name] = c;
  });

  let used_analyzers = new Set();
  Object.values(result.views).forEach(v => {
    let links = v["properties"]["links"];
    Object.values(links).forEach(c => {
      let curr_analyzers = c["analyzers"];
      curr_analyzers.forEach(a => {
        used_analyzers.add(a);
      });
    });
  });

  let analyzers = require("@arangodb/analyzers");
  let all_analyzers = analyzers.toArray();
  used_analyzers.forEach(used_a => {
    let re = new RegExp(String.raw`::${used_a}$`, "g");
    let found = all_analyzers.find((a) => a.name().match(re));
    if(found) {
      // it means that it is not a system analyzer
      result.analyzers[used_a] = {
        type: analyzers.analyzer(used_a).type(),
        properties: analyzers.analyzer(used_a).properties()
      };
    }
  });
  
  result.graphs = graphs;
  return result;
}

function debugDump(filename, query, bindVars, options) {
  let result = debug(query, bindVars, options);
  require('fs').write(filename, JSON.stringify(result));
  require('console').log("stored query debug information in file '" + filename + "'");
}

function inspectDump(filename, outfile) {
  let print = internal.print;
  if (outfile !== undefined) {
    internal.startCaptureMode();
  }

  let data = JSON.parse(require('fs').read(filename));
  let version = "";
  let license = "";
  if (data.version) {
    version = data.version;
    license = version.license;
    if (version && version.details) {
      version = version.details;
    }
    if (version && version.hasOwnProperty('server-version')) {
      version = version['server-version'];
    }
    print("/* original data is from " + version + ", " + license + " */");
  }
  if (data.database) {
    print("/* original data gathered from database " + JSON.stringify(data.database) + " */");
  }

  try {
    if (db._engine().name !== data.engine.name) {
      print("/* using different storage engine (' " + db._engine().name + "') than in debug information ('" + data.engine.name + "') */");
    }
    print();
  } catch (err) {
    // ignore errors here. db._engine() will fail if we are not connected
    // to a server instance. swallowing the error here allows us to continue
    // and analyze the dump offline
  }

  print("/* graphs */");
  let graphs = data.graphs || {};
  Object.keys(graphs).forEach(function (graph) {
    let details = graphs[graph];
    print("try { db._graphs.remove(" + JSON.stringify(graph) + "); } catch (err) {}");
    print("db._graphs.insert(" + JSON.stringify(details) + ");");
  });
  print();

  // drop views to be able to recreate analyzers
  print("/*drop views */");
  Object.keys(data.views || {}).forEach(function (view) {
    print("db._dropView(" + JSON.stringify(view) + ");");
  });
  print();

  // all analyzers if they are exists
  if (data.hasOwnProperty("analyzers")) {
    print("/* analyzers setup */");
    const analyzers_names = Object.keys(data.analyzers);
    if (analyzers_names.length > 0) {
      print("var analyzers = require('@arangodb/analyzers')");
      for (let i = 0; i < analyzers_names.length; ++i) {
        let name = analyzers_names[i];
        print(`try { analyzers.remove("${name}"); } catch (err) { print(String(err)); }`);
        print(`analyzers.save("${name}", ${JSON.stringify(data.analyzers[name]["type"])}, ${JSON.stringify(data.analyzers[name]["properties"])})`);
      }
    }
    print();
  }

  // all collections and indexes first, as data insertion may go wrong later
  print("/* collections and indexes setup */");
  const keys = Object.keys(data.collections);
  if (keys.length > 0) {
    // drop in reverse order, because of distributeShardsLike
    for (let i = keys.length; i > 0; --i) {
      let collection = keys[i - 1];
      let details = data.collections[collection];
      let name = details.name || collection;
      if (name[0] === '_') {
        // system collection
        print("try { db._drop(" + JSON.stringify(collection) + ", true); } catch (err) { print(String(err)); }");
      } else {
        print("try { db._drop(" + JSON.stringify(collection) + "); } catch (err) { print(String(err)); }");
      }
    }
    // create in forward order because of distributeShardsLike
    for (let i = 0; i < keys.length; ++i) {
      let collection = keys[i];
      let details = data.collections[collection];
      if (details.type === false || details.type === 3) {
        if (details.properties.isSmart) {
          delete details.properties.numberOfShards;
        }
        delete details.properties.objectId;
        delete details.properties.statusString;
        print("db._createEdgeCollection(" + JSON.stringify(collection) + ", " + JSON.stringify(details.properties) + ");");
      } else {
        print("db._create(" + JSON.stringify(collection) + ", " + JSON.stringify(details.properties) + ");");
      }
      details.indexes.forEach(function (index) {
        delete index.figures;
        delete index.selectivityEstimate;
        if (index.type !== 'primary' && index.type !== 'edge') {
          print("db[" + JSON.stringify(collection) + "].ensureIndex(" + JSON.stringify(index) + ");");
        }
      });
      print();
    }
  }
  print();

  // insert example data
  print("/* example data */");
  Object.keys(data.collections).forEach(function (collection) {
    let details = data.collections[collection];
    if (details.examples) {
      details.examples.forEach(function (example) {
        print("db[" + JSON.stringify(collection) + "].insert(" + JSON.stringify(example) + ");");
      });
    }
    let missing = details.count;
    if (details.examples) {
      missing -= details.examples.length;
    }
    if (missing > 0) {
      print("/* collection '" + collection + "' needs " + missing + " more document(s) */");
    }
    print();
  });
  print();

  // views
  print("/* views */");
  Object.keys(data.views || {}).forEach(function (view) {
    let details = data.views[view];
    print("db._createView(" + JSON.stringify(view) + ", " + JSON.stringify(details.type) + ", " + JSON.stringify(details.properties) + ");");
  });
  print();

  print("/* explain result */");
  print(data.fancy.trim().split(/\n/).map(function (line) { return "// " + line; }).join("\n"));
  print();

  print("/* explain command */");
  if (data.query.options) {
    delete data.query.options.anonymize;
    delete data.query.options.colors;
    delete data.query.options.examples;
  }
  print("db._explain(" + JSON.stringify(data.query) + ");");
  print();

  if (outfile !== undefined) {
    require('fs').write(outfile, internal.stopCaptureMode());
    require('console').log("stored query restore script in file '" + outfile + "'");
    require('console').log("to run it, execute  require('internal').load('" + outfile + "');");
  }
}

function explainQueryRegisters(plan) {
  let print = internal.print;

  /**
   * @typedef Node
   * @type {
   *   {
   *     depth: number,
   *     id: number,
   *     nrRegsArray: number[],
   *     regsToClear: number[],
   *     regsToKeepStack: Array<number[]>,
   *     type: string,
   *     unusedRegsStack: Array<number[]>,
   *     varInfoList: Array<{VariableId: number, RegisterId: number, depth: number}>,
   *     varsSetHere: Array<{id: number, name: string, isFullDocumentFromCollection: boolean}>,
   *     varsUsedHere: Array<{id: number, name: string, isFullDocumentFromCollection: boolean}>,
   *   }
   * }
   */

  // Currently, we need nothing but the nodes from plan.
  const {nodes} = plan;
  const symbols = {
    clearRegister: '⮾',
    keepRegister: '↓',
    writeRegister: '→',
    readRegister: '←',
    unusedRegister: '-',
    subqueryBegin: '↘',
    subqueryEnd: '↙',
  };
  Object.freeze(symbols);

  const getNodesById = (planNodes, result = {}) => {
    for (let node of planNodes) {
      result[node.id] = node;
      if (node.type === 'SubqueryNode') {
        getNodesById(node.subquery.nodes, result);
      }
    }

    return result;
  };
  const nodesById = getNodesById(nodes);
  //////////////////////////////////////////////////////////////////////////////
  // First, we build up the array nodesData. This will (though not while its
  // being built!) contain all nodes in a linearized fashion in the "canonical"
  // order, that is, starting from the leaf (singleton) at index 0. Subqueries
  // are inlined in that way as well. Each subquery node will occur *twice*,
  // once before and once after the actual subquery.
  //////////////////////////////////////////////////////////////////////////////

  /**
   * @type {Array<{node:Node, direction:('open'|'close'|undefined)}>}
   */
  const nodesData = [];

  // TODO Check whether its okay to assume that the root node is always the last
  //      node.
  const rootNode = nodes[nodes.length - 1];

  const subqueryStack = [];
  for (let node = rootNode; node !== undefined; ) {
    const current = {node};
    nodesData.push(current);
    if (node.type === 'SubqueryNode') {
      subqueryStack.push(node);
      current.direction = 'close';
      node = node.subquery.nodes[node.subquery.nodes.length - 1];
    } else if (node.dependencies && node.dependencies.length > 0) {
      node = nodesById[node.dependencies[0]];
    } else if (subqueryStack.length > 0) {
      node = subqueryStack.pop();
      const current = {node};
      current.direction = 'open';
      nodesData.push(current);
      node = nodesById[node.dependencies[0]];
    } else {
      node = undefined;
    }
  }

  // for debugging. TODO remove it or replace it with saner exceptions
  const assert = (bool) => {
    if (!bool) {
      throw new Error();
    }
  };

  nodesData.reverse();

  //////////////////////////////////////////////////////////////////////////////
  // Second, we build up rowsInfos. Will contain one element per row/line that
  // shall be printed, with enough information that each field can be printed
  // and formatted on its own.
  //////////////////////////////////////////////////////////////////////////////

  /**
   * @typedef RowInfo
   * @type { { registerFields: string[], separator: string }
   *       | { registerFields: string[], meta: {depth: number, id: number, type: string} }
   *       | { registerFields: string[] }
   *       }
   */
  /**
   * @type {Array<RowInfo>} rowsInfos
   */
  const rowsInfos = nodesData.flatMap(data => {
    /**
     * @type {{node: Node, direction: ('open'|'close'|undefined)}} data
     */
    const {
      node: {
        depth, id, nrRegs: nrRegsArray, regsToClear, regsToKeepStack, type,
        unusedRegsStack, varInfoList, varsSetHere, varsUsedHere, regVarMapStack
      },
      direction
    } = data;
    const nrRegs = nrRegsArray[depth];
    const regIdByVarId = Object.fromEntries(
      varInfoList.filter(varInfo => varInfo.RegisterId < 1000).map(varInfo => [varInfo.VariableId, varInfo.RegisterId]));

    const meta = {id, type, depth};
    const result = [];

    const varName = (variable) => {
      if (/^[0-9_]/.test(variable.name)) {
        return '#' + variable.name;
      }
      return variable.name;
    };

    const regIdToVarNameSetHere = Object.fromEntries(
      varsSetHere.map(variable => [regIdByVarId[variable.id], varName(variable)]));

    const regIdToVarNameUsedHere = Object.fromEntries(
      varsUsedHere.map(variable => [regIdByVarId[variable.id], varName(variable)]));

    const regIdToVarNameMapStack = regVarMapStack.map(rvmap =>
        Object.fromEntries(_.toPairsIn(rvmap).map(
            (p) => [parseInt(p[0]), varName(p[1])]
        )
    ));

    /**
     * @param {number} nrRegs
     * @param { { regIdToVarNameSetHere: (undefined|!Object<number, string>),
     *            regIdToVarNameUsedHere: (undefined|!Object<number, string>),
     *            unusedRegs: (undefined|!number[]),
     *            regVarMap: (undefined|!Object<number, string>)}
     *          | { regsToClear: (undefined|!number[]), regsToKeep: (undefined|!number[]) } } regInfo
     * @return { !Array<string> }
     */
    const createRegisterFields = (nrRegs, regInfo = {}) => {
      const registerFields = [...Array(nrRegs)].map(_ => '');

      const hasRegIdToVarNameSetHere = regInfo.hasOwnProperty('regIdToVarNameSetHere');
      const hasRegIdToVarNameUsedHere = regInfo.hasOwnProperty('regIdToVarNameUsedHere');
      const hasUnusedRegs = regInfo.hasOwnProperty('unusedRegs');
      const hasRegsToClear = regInfo.hasOwnProperty('regsToClear');
      const hasRegsToKeep = regInfo.hasOwnProperty('regsToKeep');
      const hasRegVarMap = regInfo.hasOwnProperty('regVarMap');

      assert(!((hasRegIdToVarNameSetHere || hasRegIdToVarNameUsedHere || hasUnusedRegs) && (hasRegsToClear || hasRegsToKeep)));

      if (hasRegIdToVarNameUsedHere) {
        for (const [regId, varName] of Object.entries(regInfo.regIdToVarNameUsedHere)) {
          registerFields[regId] += symbols.readRegister + varName + ' ';
        }
      }
      if (hasRegIdToVarNameSetHere) {
        for (const [regId, varName] of Object.entries(regInfo.regIdToVarNameSetHere)) {
          registerFields[regId] += symbols.writeRegister + varName + ' ';
        }
      }
      if (hasUnusedRegs) {
        for (const regId of regInfo.unusedRegs) {
          if (registerFields[regId].length === 0) {
            registerFields[regId] += symbols.unusedRegister;
          }
        }
      }
      if (hasRegsToClear) {
        for (const regId of regInfo.regsToClear) {
          registerFields[regId] += symbols.clearRegister;
        }
      }
      if (hasRegsToKeep) {
        for (const regId of regInfo.regsToKeep) {
          registerFields[regId] += symbols.keepRegister;
        }
      }
      if (hasRegVarMap) {
        for (const [regId, name] of Object.entries(regInfo.regVarMap)) {
          if (registerFields[regId].length === 0) {
            registerFields[regId] = name;
          }
        }
      }


      return registerFields;
    };

    switch (type) {
      case 'SubqueryStartNode': {
        assert(regsToKeepStack.length >= 2);
        let regsToKeep = regsToKeepStack[regsToKeepStack.length - 2];
        let regVarMap = regIdToVarNameMapStack[regIdToVarNameMapStack.length - 2];
        const unusedRegs = unusedRegsStack[unusedRegsStack.length - 1];
        result.push({meta, registerFields: createRegisterFields(nrRegs, {regIdToVarNameUsedHere, unusedRegs, regVarMap})});
        result.push({registerFields: createRegisterFields(nrRegs, {regsToClear, regsToKeep})});
        result.push({separator: symbols.subqueryBegin, registerFields: createRegisterFields(nrRegs)});
        regsToKeep = regsToKeepStack[regsToKeepStack.length - 1];
        regVarMap = regIdToVarNameMapStack[regIdToVarNameMapStack.length - 1];
        result.push({meta, registerFields: createRegisterFields(nrRegs, {regIdToVarNameSetHere, regVarMap, unusedRegs})});
        result.push({registerFields: createRegisterFields(nrRegs, {regsToKeep})});
      }
        break;
      case 'SubqueryEndNode': {
        const nrRegsInner = nrRegsArray[depth-1];
        const unusedRegs = unusedRegsStack[unusedRegsStack.length - 1];
        result.push({meta, registerFields: createRegisterFields(nrRegsInner, {unusedRegs, regIdToVarNameUsedHere})});
        result.push({separator: symbols.subqueryEnd, registerFields: createRegisterFields(nrRegsInner)});
        const regsToKeep = regsToKeepStack[regsToKeepStack.length - 1];
        const regVarMap = regIdToVarNameMapStack[regIdToVarNameMapStack.length - 1];
        result.push({meta, registerFields: createRegisterFields(nrRegs, {regIdToVarNameSetHere, regVarMap})});
        result.push({registerFields: createRegisterFields(nrRegs, {regsToClear, regsToKeep})});
      }
        break;
      case 'SubqueryNode': {
        const regVarMap = regIdToVarNameMapStack[regIdToVarNameMapStack.length - 1];
        switch (direction) {
          case 'open': {
            // We could print regIdToVarNameUsedHere here...
            result.push({separator: symbols.subqueryBegin, registerFields: createRegisterFields(nrRegs)});
          }
            break;
          case 'close': {
            const regsToKeep = regsToKeepStack[regsToKeepStack.length - 1];
            const unusedRegs = unusedRegsStack[unusedRegsStack.length - 1];
            result.push({separator: symbols.subqueryEnd, registerFields: createRegisterFields(nrRegs)});
            result.push({meta, registerFields: createRegisterFields(nrRegs, {regIdToVarNameSetHere, unusedRegs, regVarMap})});
            result.push({registerFields: createRegisterFields(nrRegs, {regsToClear, regsToKeep})});
          }
            break;
        }
      }
        break;
      default: {
        const regsToKeep = regsToKeepStack[regsToKeepStack.length - 1];
        const unusedRegs = unusedRegsStack[unusedRegsStack.length - 1];
        const regVarMap = regIdToVarNameMapStack[regIdToVarNameMapStack.length - 1];
        result.push({meta, registerFields: createRegisterFields(nrRegs, {regIdToVarNameUsedHere, regIdToVarNameSetHere, unusedRegs, regVarMap})});
        result.push({registerFields: createRegisterFields(nrRegs, {regsToClear, regsToKeep})});
      }
    }

    return result;
    }
  );

  //////////////////////////////////////////////////////////////////////////////
  // Third, set up additional information we'll need for formatting whole lines,
  // like the maximum width of all columns.
  //////////////////////////////////////////////////////////////////////////////
  const columns = [
    {name: 'id', alignment: 'r'},
    {name: 'type', alignment: 'l'},
    {name: 'depth', alignment: 'r'},
  ];

  /**
   * @type {Object<string, number>}
   */
  const colWidths = Object.fromEntries(columns.map(col => [col.name, col.name.length]));
  const regColWidths = [];

  for (const row of rowsInfos) {
    if (row.hasOwnProperty('meta')) {
      for (const col of columns) {
        colWidths[col.name] = Math.max(("" + row.meta[col.name]).length, colWidths[col.name]);
      }
    }
    if (row.hasOwnProperty('registerFields')) {
      row.registerFields.forEach((value, i) => {
        regColWidths[i] = regColWidths[i] || 0;
        regColWidths[i] = Math.max(value.length, regColWidths[i]);
      });
    }
  }

  //////////////////////////////////////////////////////////////////////////////
  // Fourth, do the actual formatting of each line and print them.
  //////////////////////////////////////////////////////////////////////////////

  /**
   * @param {string} value
   * @param {'l'|'r'|'c'} alignment
   * @param {number} width - Must be at least value.length
   * @returns {string} - Will be exactly `width` chars in length
   */
  const formatField = (value, alignment, width) => {
    const str = '' + value;
    const paddingWidth = width - str.length;

    switch (alignment) {
      case 'l':
        return str + ' '.repeat(paddingWidth);
      case 'r':
        return ' '.repeat(paddingWidth) + str;
      default:
        const halfPadding = Math.floor(paddingWidth / 2);
        const otherHalf = paddingWidth - halfPadding;
        // halfPadding <= otherHalf <= halfPadding + 1
        return ' '.repeat(halfPadding) + str + ' '.repeat(otherHalf);
    }
  };

  print(' ' + columns.map(col =>
    formatField(col.name, 'c', colWidths[col.name])
  ).join('│') + ' ');
  print('─' + Object.values(colWidths).map(w => '─'.repeat(w)).join('┼') + '─');

  for (const row of rowsInfos) {
    // print meta table
    if (row.hasOwnProperty('meta')) {
      output(' ');
      output(columns.map(col =>
        formatField(row.meta[col.name], col.alignment, colWidths[col.name])
      ).join('│'));
      output(' ');
    } else if (row.hasOwnProperty('separator')) {
      const widths = Object.values(colWidths);
      const width = _.sum(widths) + widths.length + 1;
      output(row.separator.repeat(width));
    } else {
      output(' ');
      output(Object.values(colWidths).map(w => ' '.repeat(w)).join('│'));
      output(' ');
    }
    output('  ');
    // print register table
    if (row.hasOwnProperty('registerFields')) {
      output('│');
      output(row.registerFields.map((value, r) =>
        formatField(value, 'c', regColWidths[r])
      ).join('│'));
      output('│');
    }
    output("\n");
  }


  //////////////////////////////////////////////////////////////////////////////
  // Print a legend
  //////////////////////////////////////////////////////////////////////////////
  // TODO Should this be optional?

  print(``);
  print(` Legend:`);
  print(` [ ${symbols.clearRegister} ]: Clear register`);
  print(` [ ${symbols.keepRegister} ]: Keep register`);
  print(` [ ${symbols.writeRegister} ]: Write register`);
  print(` [ ${symbols.readRegister} ]: Read register`);
  print(` [ ${symbols.unusedRegister} ]: Register is unused`);
  print(` [ ${symbols.subqueryBegin} ]: Entering a subquery`);
  print(` [ ${symbols.subqueryEnd} ]: Leaving a subquery`);
}

function explainRegisters(data, options, shouldPrint) {
  'use strict';

  let print = internal.print;
  require('console').warn("explainRegisters() is purposefully undocumented and may be changed or removed without notice.");

  if (typeof data === 'string') {
    data = { query: data, options: options };
  }
  if (!(data instanceof Object)) {
    throw 'ArangoStatement needs initial data';
  }

  if (options === undefined) {
    options = data.options;
  }
  options = options || {};
  options.verbosePlans = true;
  options.explainInternals = true;
  options.explainRegisters = true;
  setColors(options.colors === undefined ? true : options.colors);

  stringBuilder.clearOutput();
  let stmt = db._createStatement(data);
  let result = stmt.explain(options);
  if (options.allPlans) {
    // multiple plans
    printQuery(data.query);
    for (let i = 0; i < result.plans.length; ++i) {
      if (i > 0) {
        stringBuilder.appendLine();
      }
      stringBuilder.appendLine(section("Plan #" + (i + 1) + " of " + result.plans.length));
      stringBuilder.prefix = ' ';
      stringBuilder.appendLine();
      explainQueryRegisters(result.plans[i]);
      stringBuilder.prefix = '';
    }
  } else {
    // single plan
    explainQueryRegisters(result.plan);
  }

  if (shouldPrint === undefined || shouldPrint) {
    print(stringBuilder.getOutput());
  } else {
    return stringBuilder.getOutput();
  }
}

exports.explain = explain;
exports.explainRegisters = explainRegisters;
exports.profileQuery = profileQuery;
exports.debug = debug;
exports.debugDump = debugDump;
exports.inspectDump = inspectDump;
