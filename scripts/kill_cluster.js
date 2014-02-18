function main (argv) {
  var runInfoName = "runInfo.json";
  var fs = require("fs");
  var print = require("internal").print;
  if (argv.length > 1) {
    runInfoName = argv[1];
    print("Using runInfo from:",runInfoName);
  }
  var Planner = require("org/arangodb/cluster").Planner;
  var Kickstarter = require("org/arangodb/cluster").Kickstarter;
  var runInfo = JSON.parse(fs.read(runInfoName));
  var k = new Kickstarter(runInfo.plan);
  k.runInfo = runInfo.runInfo;
  k.shutdown();
  k.cleanup();
}
