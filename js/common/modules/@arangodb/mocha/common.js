'use strict';

const Suite = require('./suite');

function createMissingArgumentError(message, argument, expected) {
  var err = new TypeError(message);
  err.code = 'ERR_MOCHA_INVALID_ARG_TYPE';
  err.argument = argument;
  err.expected = expected;
  err.actual = typeof argument;
  return err;
}

/**
 * Functions common to more than one interface.
 *
 * @param {Suite[]} suites
 * @param {Context} context
 * @param {Mocha} mocha
 * @return {Object} An object containing common functions.
 */
module.exports = function(suites, context, mocha) {
  /**
   * Check if the suite should be tested.
   *
   * @private
   * @param {Suite} suite - suite to check
   * @returns {boolean}
   */
  function shouldBeTested(suite) {
    return (
      !mocha.options.grep ||
      (mocha.options.grep &&
        mocha.options.grep.test(suite.fullTitle()) &&
        !mocha.options.invert)
    );
  }

  return {
    /**
     * This is only present if flag --delay is passed into Mocha. It triggers
     * root suite execution.
     *
     * @param {Suite} suite The root suite.
     * @return {Function} A function which runs the root suite
     */
    runWithSuite: function runWithSuite(suite) {
      return function run() {
        suite.run();
      };
    },

    /**
     * Execute before running tests.
     *
     * @param {string} name
     * @param {Function} fn
     */
    before: function(name, fn) {
      suites[0].beforeAll(name, fn);
    },

    /**
     * Execute after running tests.
     *
     * @param {string} name
     * @param {Function} fn
     */
    after: function(name, fn) {
      suites[0].afterAll(name, fn);
    },

    /**
     * Execute before each test case.
     *
     * @param {string} name
     * @param {Function} fn
     */
    beforeEach: function(name, fn) {
      suites[0].beforeEach(name, fn);
    },

    /**
     * Execute after each test case.
     *
     * @param {string} name
     * @param {Function} fn
     */
    afterEach: function(name, fn) {
      suites[0].afterEach(name, fn);
    },

    suite: {
      /**
       * Create an exclusive Suite; convenience function
       * See docstring for create() below.
       *
       * @param {Object} opts
       * @returns {Suite}
       */
      only: function only(opts) {
        opts.isOnly = true;
        return this.create(opts);
      },

      /**
       * Create a Suite, but skip it; convenience function
       * See docstring for create() below.
       *
       * @param {Object} opts
       * @returns {Suite}
       */
      skip: function skip(opts) {
        opts.pending = true;
        return this.create(opts);
      },

      /**
       * Creates a suite.
       *
       * @param {Object} opts Options
       * @param {string} opts.title Title of Suite
       * @param {Function} [opts.fn] Suite Function (not always applicable)
       * @param {boolean} [opts.pending] Is Suite pending?
       * @param {string} [opts.file] Filepath where this Suite resides
       * @param {boolean} [opts.isOnly] Is Suite exclusive?
       * @returns {Suite}
       */
      create: function create(opts) {
        var suite = Suite.create(suites[0], opts.title);
        suite.pending = Boolean(opts.pending);
        suite.file = opts.file;
        suites.unshift(suite);
        if (opts.isOnly) {
          if (mocha.options.forbidOnly && shouldBeTested(suite)) {
            throw new Error('`.only` forbidden');
          }

          suite.parent.appendOnlySuite(suite);
        }
        if (suite.pending) {
          if (mocha.options.forbidPending && shouldBeTested(suite)) {
            throw new Error('Pending test forbidden');
          }
        }
        if (typeof opts.fn === 'function') {
          opts.fn.call(suite);
          suites.shift();
        } else if (typeof opts.fn === 'undefined' && !suite.pending) {
          throw createMissingArgumentError(
            'Suite "' +
              suite.fullTitle() +
              '" was defined but no callback was supplied. ' +
              'Supply a callback or explicitly skip the suite.',
            'callback',
            'function'
          );
        } else if (!opts.fn && suite.pending) {
          suites.shift();
        }

        return suite;
      }
    },

    test: {
      /**
       * Exclusive test-case.
       *
       * @param {Object} mocha
       * @param {Function} test
       * @returns {*}
       */
      only: function(mocha, test) {
        test.parent.appendOnlyTest(test);
        return test;
      },

      /**
       * Pending test case.
       *
       * @param {string} title
       */
      skip: function(title) {
        context.test(title);
      },

      /**
       * Number of retry attempts
       *
       * @param {number} n
       */
      retries: function(n) {
        context.retries(n);
      }
    }
  };
};
