# Contributing

We welcome bug fixes and patches from 3rd party contributors. Please
see the [Contributor Agreement](https://www.arangodb.com/community#contribute)
for details.

Please follow these guidelines if you want to contribute to ArangoDB:

# Reporting Bugs

When reporting bugs, please use our issue tracker on GitHub. Please make sure
to include the version number of ArangoDB in your bug report, along with the
platform you are using (e.g. `Linux OpenSuSE x86_64`). Please also include the
ArangoDB startup mode (daemon, console, supervisor mode) plus any special
configuration. This will help us reproducing and finding bugs.

Please also take the time to check there are no similar/identical issues open
yet.

# Contributing features, documentation, tests

- Create a new branch in your fork, based on the **devel** branch

- Develop and test your modifications there

- Commit as you like, but preferably in logical chunks. Use meaningful commit
  messages and make sure you do not commit unnecessary files (e.g. object
  files). It is normally a good idea to reference the issue number from the
  commit message so the issues will get updated automatically with comments.

- If the modifications change any documented behavior or add new features,
  document the changes. It should be written in American English.
  The documentation can be found in [`docs-hugo` repository](https://github.com/arangodb/docs-hugo#readme).

- When done, run the complete test suite and make sure all tests pass.

- When finished, push the changes to your GitHub repository and send a pull
  request from your fork to the ArangoDB repository. Please make sure to select
  the appropriate branches there. This will most likely be **devel**.

- You must use the Apache License for your changes and have signed our
  [CLA](https://arangodb.com/community/#contribute). We cannot accept pull requests
  from contributors who didn't sign the CLA.

- Please let us know if you plan to work on a ticket. This way we can make sure
  redundant work is avoided.

# Main sections

- [Source Code](#source-code)
- [Building](#building)
- [Running](#running)
- [Debugging](#debugging)
  - [Linux Core Dumps](#linux-core-dumps)
- [Unittests](#unittests)
  - [invoking driver tests](#driver-tests) via scripts/unittests
  - [capturing test HTTP communication](#running-tcpdump--windump-for-the-sut) but
    [forcing communication to use plain-text JSON](#forcing-downgrade-from-vpack-to-json)
  - [Evaluating previous testruns](#evaluating-json-test-reports-from-previous-testruns)
    sorting by setup time etc.
- [What to test where and how](tests/README.md)

## Source Code

### Get the source code

Download the latest source code with Git, including submodules:

    git clone --recurse-submodules --jobs 8 https://github.com/arangodb/arangodb

This automatically clones the **devel** branch.

If you only plan to compile a specific version of ArangoDB locally and do not
want to commit changes, you can speed up cloning substantially by using the
`--depth` and `--shallow-submodules` parameters and specifying a branch, tag,
or commit hash using the `--branch` parameter for the clone command as follows:

    git clone --depth 1 --recurse-submodules --shallow-submodules --jobs 8 --branch v3.12.0 https://github.com/arangodb/arangodb

On Windows, Git's automatic conversion to CRLF line endings needs to be disabled.
Otherwise, [compiling](#building-the-binaries) in the Linux build container fails.
You can use `git clone --config core.autocrlf=input ...` so that it only affects
this working copy.

### Git Setup

Setting up git for automatically merging certain automatically generated files
in the ArangoDB source tree:

    git config --global merge.ours.driver true

### Style Guide

We use `clang-format` to enforce consistent code formatting. Check 
[STYLEGUIDE.md](STYLEGUIDE.md) for a comprehensive description of ArangoDB's 
Coding Guidelines.

### Compiler support policy

We support only officially released compiler versions which major version is
at least 9 month old.
ArangoDB does not guarantee successful compilation with arbitrary compilers, 
but uses a certain compiler make and version for building ArangoDB. Both the
compiler make and version can change over time.

### Unique Log Ids

We have unique log ids in order to allow for easy locating of code producing
errors.

    LOG_TOPIC("2dead", ....)

To ensure that the ids are unique we run the script `./utils/checkLogIds.py`
during CI runs. The script will fail with a non-zero status if id collisions are
found. You can use `openssl rand -hex 3 | sed 's/.//;s/\(.*\)/"\1"/'` or
anything that suits you to generate a **5 hex digit log** id.

### ESLint

Use:

    ./utils/giteslint.sh

to lint your modified files.

    ./utils/eslint.sh

to find out whether all of your files comply to eslint. This is required to
make continuous integration work smoothly.

If you want to add new files / patterns to this make target, edit the respective
shell scripts.

To be safe from committing non-linted stuff add **.git/hooks/pre-commit** with:

    ./utils/eslint.sh

### Adding startup options

Startup option example with explanations:

```cpp
options->
    addOption("--section.option-name",
              "A brief description of the startup option, maybe over multiple "
              "lines, with an upper-case first letter and ending with a .",
              // for numeric options, you may need to indicate the unit like "timeout (in seconds)"
                new XyzParameter(&_xyzVariable),
                arangodb::options::makeFlags(
                    arangodb::options::Flags::DefaultNoComponents,
                    arangodb::options::Flags::OnCoordinator, // in a cluster, it only has an effect on Coordinators
                    arangodb::options::Flags::OnSingle,      // supported in single server mode, too
                    arangodb::options::Flags::Enterprise,    // only available in the Enterprise Edition
                    arangodb::options::Flags::Uncommon))     // don't show with --help but only --help-all or --help-<section>
    .setIntroducedIn(30906) // format XYYZZ, X = major, YY = minor, ZZ = bugfix version
    .setIntroducedIn(31002) // list all versions the feature is added in but exclude versions that are implied, e.g. 31100
    .setLongDescription(R"(You can optionally add details here. They are only
shown in the online documentation (and --dump-options). 

The _text_ is interpreted as **Markdown**, allowing formatting like
`inline code`, fenced code blocks, and even tables.)");
```

See [`lib/ProgramOptions/Option.h`](lib/ProgramOptions/Option.h) for details.

For a feature that is added to v3.9.6, v3.10.2, and devel, you only need to set
`.setIntroducedIn()` for 3.9 and 3.10 but not 3.11 (in devel) because all later
versions (after v3.10.2) can reasonably be expected to include the feature, too.
Leaving v3.10.2 out would be unclear, however, because users may assume to find
the feature in v3.10.0 and v3.10.1 if it was added to v3.9.6.

In the 3.9 branch, you should only set the versions up to 3.9 but not 3.10,
pretending no later version exists to avoid additional maintenance burdens.

In 3.9:

```cpp
    .setIntroducedIn(30906)
```

In 3.10 and later:

```cpp
    .setIntroducedIn(30906)
    .setIntroducedIn(31002)
```

These version remarks can be removed again when all of the listed versions for
an option are obsolete (and removed from the online documentation).

Note that `.setDeprecatedIn()` should not be removed until the startup option is
obsoleted or fully removed.

### Adding metrics

As of 3.8 we have enforced documentation for metrics. This works as
follows. Every metric which is generated has a name. The metric must be
declared by using one of the macros

```
  DECLARE_COUNTER(name, helpstring);
  DECLARE_GAUGE(name, type, helpstring);
  DECLARE_HISTOGRAM(name, scaletype, helpstring);
```

in some `.cpp` file (please put only one on a line). Then, when the
metric is actually requested in the source code, it gets a template
added to the metrics feature with the `add` method and its name.
Labels can be added with the `withLabel` method. In this way, the
compiler ensures that the metric declaration is actually there if
the metric is used.

Then there is a helper script `utils/generateAllMetricsDocumentation.py`
which needs `python3` with the `yaml` module. It will check and do the
following things:

- Every declared metric in some `.cpp` file in the source has a
  corresponding documentation snippet in `Documentation/Metrics`
  under the name of the metric with `.yaml` appended
- Each such file is a YAML file of a certain format (see template
  under `Documentation/Metrics/template.yaml`)
- Many of the components are required, so please provide adequate
  information about your metric
- Make sure to set `introducedIn` to the version the metric is added to the
  current branch's version (e.g. `"3.9.6"` in the 3.9 branch, `"3.10.2"` in
  the 3.10 and devel branches)
- The script can also assemble all these YAML documentation snippets
  into a single file under `Documentation/Metrics/allMetrics.yaml`,
  the format is again a structured YAML file which can easily be
  processed by the documentation tools, this is only needed when
  we update the documentation web pages.

Please, if you have added or modified a metric, make sure to declare
the metric as shown above and add a documentation YAML snippet in the
correct format. Afterwards, run

    utils/generateAllMetricsDocumentation.py

but do not include `Documentation/Metrics/allMetrics.yaml` in your PR
(as was a previous policy). The file is only generated if you add
`-d` as command line option to the script.

---

## Building

Note: Make sure that your source path does not contain spaces. Otherwise, the
build process will likely fail.

### Building the binaries

To compile ArangoDB 3.12 or later, you can use a Docker build container that
is based on Ubuntu and includes the necessary toolchain. You need Git on the
host system to [get the source code](#get-the-source-code) and mount it into the
container, however.

Check the [`.circleci/base_config.yml`](.circleci/base_config.yml) file for the
default `build-docker-image` value to ensure it's the matching image tag for the
version you want to build.

You can start the build container as follows in a Bash-like terminal:

    cd arangodb
    docker run -it -v $(pwd):/root/project -p 3000:3000 arangodb/ubuntubuildarangodb-devel:3

In PowerShell, use `${pwd}` instead of `$(pwd)`.

The port mapping is for accessing the web interface in case you want to run a
development server for working on the frontend.

In the fish shell of the container, run the following commands for a release-like
build using the officially supported version of the Clang compiler
(also see the [`VERSIONS`](VERSIONS)):

    cd /root/project
    cmake --preset community -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_EXE_LINKER_FLAGS="-fuse-ld=lld" -DCMAKE_LIBRARY_PATH=$OPENSSL_ROOT_DIR/lib -DOPENSSL_ROOT_DIR=/opt
    cmake --build --preset community --parallel (nproc)

For a debug build, you can use the `community-developer` preset instead.

The default target CPU architecture is `sandybridge` for the x86-64 platform and
`graviton1` for 64-bit ARM chips. You can specify a different architecture to
enable more optimizations or to limit the compiler to the available instructions
for a particular architecture:

    cmake --preset community -DTARGET_ARCHITECTURE=native ...

On ARM, disable the `USE_LIBUNWIND` option:

    cmake --preset community -DUSE_LIBUNWIND=Off ...

To build specific targets instead of all, you can specify them like shown below:

    cmake --build --preset community --target arangod arangosh arangodump arangorestore arangoimport arangoexport arangobench arangovpack frontend

You can find the output files at `build-presets/<preset-name>/`,
with the compiled executables in the `bin/` subfolder.

To collect all files for packaging, you can run the following command (with
`<preset-name>` substituted, e.g. with `community`):

    cmake --install ./build-presets/<preset-name> --prefix install/usr

It copies the needed files to a folder called `install`.
It may fail if you only built a subset of the targets.
The folder is organized following the Linux directory structure, with the
server executable at `usr/sbin/arangod`, the tools in `usr/bin/`, the
configuration files in `usr/etc/arangodb3/`, and so on.

For building the ArangoDB Starter (`arangodb` executable), see the
[ArangoDB Starter repository](https://github.com/arangodb-helper/arangodb).

### Building the Web Interface

The web interface is also known as the Web UI, frontend, or _Aardvark_.
Building ArangoDB also builds the frontend unless `-DUSE_FRONTEND=Off` was
specified when configuring CMake or if the `frontend` target was left out for
the build.

To build the web interface via CMake, you can run the following commands:

    cmake --preset community
    cmake --build --preset community --target frontend

To remove all installed Node.js modules and start a clean installation, run:

    cmake --build --preset community --target frontend_clean

You can also build the frontend manually without CMake.
[Node.js](https://nodejs.org/) and [Yarn](https://yarnpkg.com/) need to be
installed.

    cd <SourceRoot>/js/apps/system/_admin/aardvark/APP/react
    yarn install
    yarn build

To run a development server, go to `js/apps/system/_admin/aardvark/APP/react`
and run the following command:

    yarn start

It should start your browser and open the web interface at `http://localhost:3000`.
Changes to any frontend source files trigger an automatic re-build and reload
the browser tab. If you use the build container, make sure to start it with a
port mapping for the development server.

#### Cross Origin Policy (CORS) error

Our front-end development server currently runs on port:`3000`, while the backend
runs on port:`8529` respectively. This implies that when the front-end sends a
request to the backend would result in Cross-Origin-Policy security checks which
recently got enforced by some browsers for security reasons. Until recently, we
never had reports of CORS errors when running both the backend and front-end dev
servers independently, however, we recently confirmed that this error occurs in
(Chrome version 98.0.4758.102 and Firefox version 96.0.1).

In case you run into CORS errors while running the development server, here is a quick fix:

Simply stop your already running backend process with the command below:

```bash
cmd + c
```

Then restart `arangodb` server with the command below:

```bash
./build-presets/<preset-name>/bin/arangod ../Arango --http.trusted-origin '*'
```

Note:

a: `./build-presets/<preset-name>/bin/arangod`: represents the location of `arangodb` binaries in your machine.

b: `../Arango`: is the database directory where the data will be stored.

c: `--http.trusted-origin '*'` the prefix that allows cross-origin requests.

#### NPM Dependencies

To add new NPM dependencies, switch into the `js/node` folder and install them
with npm using the following options:

`npm install [<@scope>/]<name> --global-style --save --save-exact`

or simply

`npm install [<@scope>/]<name> --global-style -s -E`

The `save` and `save-exact` options are necessary to make sure the
`package.json` file is updated correctly.

The `global-style` option prevents newer versions of npm from unrolling nested
dependencies inside the `node_modules` folder. Omitting this option results in
exposing _all_ dependencies of _all_ modules to ArangoDB users.

Finally add the module's licensing information to
`LICENSES-OTHER-COMPONENTS.md`.

If you need to make adjustments/modifications to dependencies or replace
transitive dependencies with mocks, make sure to run `npx patch-package $dependencyName`
in the `js/node` folder and commit the resulting patch file in `js/node/patches`.
This will ensure the changes are persisted if the dependency is overwritten by `npm`
in the future.

For example to commit a patch for the transitive dependency `is-wsl` of the dependency
`node-netstat`, make your changes in `js/node/node_modules/node-netstat/node_modules/is-wsl`
and then run `npx patch-package node-netstat/is-wsl` in `js/node` and commit the resulting
patch file in `js/node/patches`.

#### Build the HTTP API documentation for Swagger-UI

The REST HTTP API of the ArangoDB server is described using the OpenAPI
specification (formerly Swagger). The source code is in documentation repository
at <https://github.com/arangodb/docs-hugo>.

To build the `api-docs.json` file for viewing the API documentation in the
Swagger-UI of the web interface (**SUPPORT** section, **Rest API** tab), run
the following commands in a terminal:

1. Get a working copy of the documentation content with Git:

   `git clone https://github.com/arangodb/docs-hugo`

2. Enter the `docs-hugo` folder:

   `cd docs-hugo`

3. Optional: Switch to a tag, branch, or commit if you want to build the
   API documentation for a specific version of the docs:
   
   `git checkout <reference>`

4. Enter the folder of the Docker toolchain, `amd64` on the x86-64 architecture
   and `arm64` on ARM CPUs:

   ```shell
   cd toolchain/docker/amd64 # x86-64
   cd toolchain/docker/arm64 # ARM 64-bit
   ```

5. Set the environment variable `ENV` to any value other than `local` to make
   the documentation tooling not start a live server in watch mode but rather
   create and static build and exit:

   ```shell
   export ENV=static  # Bash
   set -xg ENV static # Fish
   $Env:ENV='static'  # PowerShell
   ```

6. Run Docker Compose using the plain build configuration for the documentation:

   `docker compose -f docker-compose.plain-build.yml up --abort-on-container-exit`

7. When the docs building finishes successfully, you can find the `api-docs.json`
   files in `site/data/<version>/`.

8. Copy the respective `api-docs.json` file into the ArangoDB working copy or
   installation folder under `js/apps/system/_admin/aardvark/APP/api-docs.json`
   and refresh the web interface.

---

## Running

### Temporary files and temp directories

ArangoDB tries to locate the temporary directory like this:

- the environment variable `TMPDIR` is evaluated.
- the startup option `--temp.path` overrules the above system provided settings.

Our testing framework uses this path in the cluster test cases to set an
environment variable `ARANGOTEST_ROOT_DIR` which is global to the running
cluster, but specific to the current test suite. You can access this as 
`global.instanceManager.rootDir` in Javascript client tests and via the
environment variable on the C++ level.

### Local Cluster Startup

The scripts `scripts/startLocalCluster` helps you to quickly fire up a testing
cluster on your local machine. `scripts/shutdownLocalCluster` stops it again.

`scripts/startLocalCluster [numDBServers numCoordinators [mode]]`

Without arguments it starts 2 DB-Servers and 1 Coordinator in the background,
running on ports 8629, 8630 and 8530 respectively. The Agency runs on port 4001.

Mode:

- `C`: Starts the first Coordinator with `--console` in a separate window
  (using an `xterm`).
- `D`: Starts all DB-Servers in the GNU debugger in separate windows
  (using `xterm`s). Hit _ENTER_ in the original terminal where the script
  runs to continue once all processes have been started up in the debugger.

---

## Debugging

### Runtime

- start arangod with `--console` to get a debug console
- Cheapen startup for valgrind: `--server.rest-server false`

### Startup

Arangod has a startup rc file: `~/.arangod.rc`. It's evaled as JavaScript. A
sample version to help working with the arangod rescue console may look like
that:

    internal = require("internal");
    fs = require("fs");
    db = internal.db;
    time = internal.time;
    timed = function (cb) {
      let s = time();
      cb();
      return time() - s;
    };
    print = internal.print;

_Hint_: You shouldn't lean on these variables in your Foxx services.

### Debugging AQL execution blocks

To debug AQL execution blocks, two steps are required:

- turn on logging for queries using `--extraArgs:log.level queries=info`
- divert this facilities log output into individual files: `--extraArgs --log.output queries file://@ARANGODB_SERVER_DIR@/arangod_queries.log`
- send queries enabling block debugging: `db._query('RETURN 1', {}, { profile: 4 })`

You now will get log entries with the contents being passed between the blocks.

### Crashes

The Linux builds of the arangod executable contain a built-in crash handler
(introduced in v3.7.0).
The crash handler is supposed to log basic crash information to the ArangoDB logfile in
case the arangod process receives one of the signals SIGSEGV, SIGBUS, SIGILL, SIGFPE or
SIGABRT. SIGKILL signals, which the operating system can send to a process in case of OOM
(out of memory), are not interceptable and thus cannot be intercepted by the crash handler,

In case the crash handler receives one of the mentioned interceptable signals, it will
write basic crash information to the logfile and a backtrace of the call site.
The backtrace can be provided to the ArangoDB support for further inspection. Note that
backtraces are only usable if debug symbols for ArangoDB have been installed as well.

After logging the crash information, the crash handler will execute the default action for
the signal it has caught. If core dumps are enabled, the default action for these signals
is to generate a core file. If core dumps are not enabled, the crash handler will simply
terminate the program with a non-zero exit code.

The crash handler can be disabled at server start by setting the environment variable
`ARANGODB_OVERRIDE_CRASH_HANDLER` to `0` or `off`.

An example log output from the crash handler looks like this:

```
2020-05-26T23:26:10Z [16657] FATAL [a7902] {crash} ArangoDB 3.7.1-devel enterprise [linux], thread 22 [Console] caught unexpected signal 11 (SIGSEGV) accessing address 0x0000000000000000: signal handler invoked
2020-05-26T23:26:10Z [16657] INFO [308c3] {crash} frame 1 [0x00007f9124e93ece]: _ZN12_GLOBAL__N_112crashHandlerEiP9siginfo_tPv (+0x000000000000002e)
2020-05-26T23:26:10Z [16657] INFO [308c3] {crash} frame 2 [0x00007f912687bfb2]: sigprocmask (+0x0000000000000021)
2020-05-26T23:26:10Z [16657] INFO [308c3] {crash} frame 3 [0x00007f9123e08024]: _ZN8arangodb3aql10Expression23executeSimpleExpressionEPKNS0_7AstNodeEPNS_11transaction7MethodsERbb (+0x00000000000001c4)
2020-05-26T23:26:10Z [16657] INFO [308c3] {crash} frame 4 [0x00007f9123e08314]: _ZN8arangodb3aql10Expression7executeEPNS0_17ExpressionContextERb (+0x0000000000000064)
2020-05-26T23:26:10Z [16657] INFO [308c3] {crash} frame 5 [0x00007f9123feaab2]: _ZN8arangodb3aql19CalculationExecutorILNS0_15CalculationTypeE0EE12doEvaluationERNS0_15InputAqlItemRowERNS0_16OutputAqlItemRowE (+0x0000000000000062)
2020-05-26T23:26:10Z [16657] INFO [308c3] {crash} frame 6 [0x00007f9123feae85]: _ZN8arangodb3aql19CalculationExecutorILNS0_15CalculationTypeE0EE11produceRowsERNS0_22AqlItemBlockInputRangeERNS0_16OutputAqlItemRowE (+0x00000000000000f5)
...
2020-05-26T23:26:10Z [16657] INFO [308c3] {crash} frame 31 [0x000018820ffc6d91]: *no symbol name available for this frame
2020-05-26T23:26:10Z [16657] INFO [ded81] {crash} available physical memory: 41721995264, rss usage: 294256640, vsz usage: 1217839104, threads: 46
Segmentation fault (core dumped)
```

The first line of the crash output will contain the cause of the crash (SIGSEGV in
this case). The following lines contain information about the stack frames. The
hexadecimal value presented for each frame is the instruction pointer, and if debug
symbols are installed, there will be name information about the called procedures (in
mangled format) plus the offsets into the procedures. If no debug symbols are
installed, symbol names and offsets cannot be shown for the stack frames.

### Memory profiling

Starting in 3.6 we have support for heap profiling when using jemalloc on Linux.
Here is how it is used:

Use this `cmake` option in addition to the normal options:

```
-DUSE_JEMALLOC_PROF
```

when building. Make sure you are using the builtin `jemalloc`. You need
debugging symbols to analyze the heap dumps later on, so compile with
`-DCMAKE_BUILD_TYPE=Debug` or `RelWithDebInfo`. `Debug` is probably
less confusing in the end.

Then set the environment variable for each instance you want to profile:

```
export MALLOC_CONF="prof:true"
```

When this is done, `jemalloc` internally does some profiling. You can
then use this endpoint to get a heap dump:

```
curl "http://localhost:8529/_admin/status?memory=true" > heap.dump
```

You can analyze such a heap dump with the `jeprof` tool, either by
inspecting a single dump like this:

```
jeprof build/bin/arangod heap.dump
```

or compare two dumps which were done on the same process to analyze
the difference:

```
jeprof build/bin/arangod --base=heap.dump heap.dump2
```

So far, we have mostly used the `web` command to open a picture in a
browser. It produces an `svg` file.

The tool isn't perfect, but can often point towards the place which
produces for example a memory leak or a lot of memory usage.

This is known to work under Linux and the `jeprof` tool comes with the
`libjemalloc-dev` package on Ubuntu.

Using it with the integration tests is possible; snapshots will be taken before
the first and after each subsequent testcase executed.

### Core Dumps

A core dump consists of the recorded state of the working memory of a process at
a specific time. Such a file can be created on a program crash to analyze the
cause of the unexpected termination in a debugger.

#### Linux Core Dumps

##### Linux Core Dump Generation

Generally core dumps have to be enabled using:

     ulimit -c unlimited

You should then see:

     ulimit -a
     core file size          (blocks, -c) unlimited

for each shell and its subsequent processes.

_Hint_: on Ubuntu the `apport` package may interfere with this; however you may
use the `systemd-coredump` package which automates much of the following:

So that the unit testing framework can autorun gdb it needs to reliably find the
corefiles. In Linux this is configured via the `/proc` filesystem, you can make
this reboot permanent by creating the file `/etc/sysctl.d/corepattern.conf` (or
add the following lines to `/etc/sysctl.conf`)

    # We want core files to be located in a central location
    # and know the PID plus the process name for later use.
    kernel.core_uses_pid = 1
    kernel.core_pattern =  /var/tmp/core-%e-%p-%t

to reload the above settings most systems support:

    sudo sysctl -p

Note that the `proc` paths translate sub-directories to dots. The non permanent
way of doing this in a running system is:

    echo 1 > /proc/sys/kernel/core_uses_pid
    echo '/var/tmp/core-%e-%p-%t' > /proc/sys/kernel/core_pattern

(you may also inspect these files to validate the current settings)

More modern systems facilitate
[`systemd-coredump`](https://www.freedesktop.org/software/systemd/man/systemd-coredump.html)
(via a similar named package) to control core dumps. On most systems it will
put compressed core dumps to `/var/lib/systemd/coredump`.

In order to use automatic core dump analysis with the unittests you need to
configure `/etc/systemd/coredump.conf` and set `Compress=no` - so instant
analysis may take place.

Please note that we can't support
[Ubuntu Apport](https://wiki.ubuntu.com/Apport). Please use `apport-unpack` to
send us the bare core dumps.

In order to get core dumps from binaries changing their UID the system needs to
be told that its allowed to write cores from them. Default ArangoDB
installations will do exactly that, so the following is necessary to make the
system produce core dumps from production ArangoDB instances:

Edit `/etc/security/limits.conf` to contain:

```
arangodb        -       core            infinity
```

Edit the systemd unit file `/lib/systemd/system/arangodb3.service` (USE infinity!!!):

```
## setting for core files
# Any dir that is writable by the user running arangod
WorkingDirectory=/var/lib/arangodb3
# core limit - set this to infinity to enable cores
LimitCORE=0
```

Enable new systemd settings:

`systemctl daemon-reload && systemctl restart arangodb3.service`

Enable suid process dumping:

`echo 1 >/proc/sys/fs/suid_dumpable`

Make the above change permanent:

`echo "sys.fs.suid_dumpable = 1" >> /etc/sysctl.d/99-suid-coredump.conf`

**Please note that at least GDB 9 is required.**

You can also generate core dumps from running processes without killing them by
using gdb:

    # sleep 100000 &
    [2] 6942
    # gdb /bin/sleep 6942
    ...
    0x00007faaa7abd4e4 in __GI___nanosleep (requested_time=0x7ffd047c9940, remaining=0x0) at ../sysdeps/unix/sysv/linux/nanosleep.c:28
    gdb> gcore
    Saved corefile core.6942
    gdb> quit
    Detaching from program: /bin/sleep, process 6942
    # ls -l core*
    -rw-r--r--  1 me users  352664 Nov 27 10:48  core.6942

##### Installing GDB 8 on RedHat7 or Centos7

RedHat7 and Centos7 have a package called `devtoolset-7` which contains a
complete set of relative modern development tools. It can be installed by running

```
sudo yum install devtoolset-7
```

These will be installed under some path under `/opt`. To actually use these
tools, run

```
scl enable devtoolset-7 bash
```

to start a shell in which these are used. For example, you can pull a core dump
with `gcore` using this version, even for ArangoDB >= 3.4.

##### Analyzing Core Dumps on Linux

We offer debug packages containing the debug symbols for your binaries. Please
install them if you didn't compile yourselves.

Given you saw in the log of the arangod with the PID `25216` that it died, you
should then find `/var/tmp/core-V8 WorkerThread-25216-1490887259` with this
information. We may now start GDB and inspect whats going on:

    gdb /usr/sbin/arangod /var/tmp/*25216*

These commands give useful information about the incident:

    backtrace full
    thread apply all bt

The first gives the full stacktrace including variables of the last active
thread, the later one the stacktraces of all threads.

## Unittests

### Dependencies

- _Google Test_ (compile time, shipped in the 3rdParty directory)

### Folder Locations

There are several major places where unittests live:

| Path / File                                                  | Description                                                                                                                        |
| :----------------------------------------------------------- | :--------------------------------------------------------------------------------------------------------------------------------- |
| `tests/js/server/`                                           | JavaScript tests, runnable on the server                                                                                           |
| `tests/js/common/`                                           | JavaScript tests, runnable on the server & via arangosh                                                                            |
| `tests/js/common/test-data/`                                 | Mock data used for the JavaScript tests                                                                                            |
| `tests/js/client/`                                           | JavaScript tests, runnable via arangosh                                                                                            |
| `tests/` (remaining)                                         | Google Test unittests                                                                                                              |
| implementation specific files                                |
| `scripts/unittest`                                           | Entry wrapper script for `UnitTests/unittest.js`                                                                                   |

                                          |
see [js/client/modules/@arangodb/README.md] about implementation details of the framework.

### Filename conventions

Special patterns in the test filenames are used to select tests to be executed
or skipped depending on parameters:

| Substring                | Description                                                                                                                                                                                                                                                                                                                                                                                   |
| :----------------------- | :-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `-cluster`               | These tests will only run if clustering is tested (option 'cluster' needs to be true).                                                                                                                                                                                                                                                                                                        |
| `-noncluster`            | These tests will only run if no cluster is used (option 'cluster' needs to be false)                                                                                                                                                                                                                                                                                                          |
| `-noinstr_or_noncluster` | These tests will not be ran if instrumented binaries are used and we are running in cluster mode                                                                                                                                                                                                                                                                                              |
| `-noasan`                | These tests will not be ran if *san instrumented binaries are used                                                                                                                                                                                                                                                                                                                            |
|  `-noinstr`              | These tests will not be ran if instrumented binaries are used, be it *san or gcov                                                                                                                                                                                                                                                                                                             | 
|  `-nocov`                | These tests will not be ran if gcov instrumented binaries are used.                                                                                                                                                                                                                                                                                                                           | 
|  `-fp`                   | These tests will only be ran if failurepoints are enabled while building the binaries to be used in the tests                                                                                                                                                                                                                                                                                 |
|  `-r2`                   | These tests will not be ran if --replicationVersion 1 (default) is specified for an ArangoDB deployment.                                                                                                                                                                                                                                                                                      |
| `-timecritical`          | These tests are critical to execution time - and thus may fail if arangod is to slow. This may happen i.e. if you run the tests in valgrind, so you want to avoid them since they will fail anyways. To skip them, set the option `skipTimeCritical` to _true_.                                                                                                                               |
| `-spec`                  | These tests are run using the mocha framework instead of jsunity.                                                                                                                                                                                                                                                                                                                             |
| `-nightly`               | These tests produce a certain thread on infrastructure or the test system, and therefore should only be executed once per day.                                                                                                                                                                                                                                                                |
| `-grey`                  | These tests are currently listed as "grey", which means that they are known to be unstable or broken. These tests will not be executed by the testing framework if the option `--skipGrey` is given. If `--onlyGrey` option is given then non-"grey" tests are skipped. See `tests/Greylist.txt` for up-to-date information about greylisted tests. Please help to keep this file up to date. |

### JavaScript Framework

Since several testing technologies are utilized, and different ArangoDB startup
options may be required (even different compilation options may be required) the
framework is split into testsuites.

Get a list of the available testsuites and options by invoking:

    ./scripts/unittest

To locate the suite(s) associated with a specific test file use:

    ./scripts/unittest find --test tests/js/client/shell/api/aqlfunction.js

Run all suite(s) associated with a specific test file in single server mode:

    ./scripts/unittest auto --test tests/js/client/shell/api/aqlfunction.js

Run all suite(s) associated with a specific test file in cluster mode:

    ./scripts/unittest auto --cluster true --test tests/js/client/shell/api/aqlfunction.js

Run all C++ based Google Test (gtest) tests using the `arangodbtests` binary:

    ./scripts/unittest gtest

Run specific gtest tests:

    ./scripts/unittest gtest --testCase "IResearchDocumentTest.*:*ReturnExecutor*"
    # equivalent to:
    ./build/bin/arangodbtests --gtest_filter="IResearchDocumentTest.*:*ReturnExecutor*"

Controlling the place where the test-data is stored:

    TMPDIR=/some/other/path ./scripts/unittest shell_client_aql

Note that the `arangodbtests` executable is not compiled and shipped for
production releases (`-DUSE_GOOGLE_TESTS=off`).

`scripts/unittest` is only a wrapper for the most part, the backend
functionality lives in `js/client/modules/@arangodb/testutils`.
The actual testsuites are located in the
`js/client/modules/@arangodb/testsuites` folder.

#### Passing Options

The first parameter chooses the facility to execute. Available choices include:

- **shell_client**
- **shell_api**
- **shell_api_multi**
- **shell_client_aql**
- **shell_client_multi**

Different facilities may take different options. The above mentioned usage
output contains the full detail.

Instead of starting its own instance, `unittest` can also make use of a
previously started arangod instance. You can launch the instance as you want
including via a debugger or `rr` and prepare it for what you want to test with
it. You then launch the test on it like this (assuming the default endpoint):

    ./scripts/unittest shell_client --server tcp://127.0.0.1:8529/

A commandline for running a single test (-> with the facility 'single_server')
using valgrind could look like this. Options are passed as regular long values
in the syntax --option value --sub:option value. Using Valgrind could look like
this:

    ./scripts/unittest shell_client --test tests/js/server/aql/aql-escaping.js \
      --cluster true \
      --extraArgs:server.threads 1 \
      --extraArgs:scheduler.threads 1 \
      --extraArgs:javascript.gc-frequency 1000000 \
      --extraArgs:javascript.gc-interval 65536 \
      --extraArgs:agent.log.level trace \
      --extraArgs:log.level request=debug \
      --extraArgs:log.force-direct true \
      --javascript.v8-contexts 2 \
      --valgrind /usr/bin/valgrind \
      --valgrindargs:log-file /tmp/valgrindlog.%p

- We specify the test to execute
- We specify some arangod arguments via --extraArgs which increase the server performance
- We specify to run using valgrind (this is supported by all facilities)
- We specify some valgrind commandline arguments
- We set the log levels for agents to `trace` (the Iinstance type can be specified by:
  - `single`
  - `agent`
  - `dbserver`
  - `coordinator`
  )
- We set the `requests` log level to debug on all instances
- We force the logging not to happen asynchronous
- Eventually you may still add temporary `console.log()` statements to tests you debug.

#### Running a Single Unittest Suite

Testing a single test with the framework directly from the arangosh:

    scripts/unittest shell_client --test tests/js/client/shell/shell-client.js

You can also only execute a filtered test case in a jsunity/mocha/gtest test
suite (in this case `testTokens`):

    scripts/unittest shell_client_aql --test tests/js/client/aql/aql-escaping.js --testCase testTokens

    scripts/unittest shell_client --test shell-util-spec.js --testCase zip

    scripts/unittest gtest --testCase "IResearchDocumentTest.*"

Running a test against a server you started (instead of letting the script start its own server):

    scripts/unittest shell_api --test analyzer.js --server tcp://127.0.0.1:8529 --serverRoot /tmp/123

Re-running previously failed tests:

    scripts/unittest <args> --failed

Specifying a `--test ` filter containing `-cluster` will implicitly set `--cluster true` and launch a cluster test.

The `<args>` should be the same as in the previous run, only `--test`/`--testCase` can be omitted.
The information which tests failed is taken from the `UNITTEST_RESULT.json` in your test output folder.
This failed filter should work for all jsunity and mocha tests.

#### Running several Suites in one go

Several testsuites can be launched consequently in one run by specifying them as coma separated list.
They all share the specified commandline arguments. Individual arguments can be passed as a JSON array.
The JSON Array has to contain the same number of elements as testsuites specified. The specified individual
parameters will overrule global and default values.

Running the same testsuite twice with different and shared parameters would look like this:

    ./scripts/unittest  shell_client_multi,shell_client_multi --test shell-admin-status.js  --optionsJson '[{"http2":true,"suffix":"http2"},{"http":true,"suffix":"http"}]'


#### Running Foxx Tests with a Fake Foxx Repo

Since downloading Foxx apps from GitHub can be cumbersome with shaky DSL and
DoS'ed GitHub, we can fake it like this:

    export FOXX_BASE_URL="http://germany/fakegit/"
    ./scripts/unittest single_server --test 'tests/js/server/shell/shell-foxx-manager-spec.js'

### Running jsUnity Tests

Assume that you have a test file containing:

```js
function exampleTestSuite () {
  return {
    testSizeOfTestCollection : function () {
    assertEqual(5, 5);
  };
}

jsUnity.run(aqlTestSuite);

return jsunity.done();
```

Then you can run the test suite using `jsunity.runTest()`:

```
arangosh> require("jsunity").runTest("test.js");
2012-01-28T19:10:23Z [10671] INFO Running aqlTestSuite
2012-01-28T19:10:23Z [10671] INFO 1 test found
2012-01-28T19:10:23Z [10671] INFO [PASSED] testSizeOfTestCollection
2012-01-28T19:10:23Z [10671] INFO 1 test passed
2012-01-28T19:10:23Z [10671] INFO 0 tests failed
2012-01-28T19:10:23Z [10671] INFO 1 millisecond elapsed
```

#### Running jsUnity Tests with arangosh client

Run tests this way:

    require("jsunity").runTest("tests/js/client/shell/shell-client.js");

You can only run tests which are intended to be ran via arangosh.

### Mocha Tests

All tests with `-spec` in their names are using the
[mochajs.org](https://mochajs.org) framework. To run those tests, e.g. in the
arangosh, use this:

    require("@arangodb/mocha-runner").runTest('tests/js/client/endpoint-spec.js', true)

### Release tests

[RTA](https://github.com/arangodb/release-test-automation) has some tests to verify production integration.
To aid their development, they can also be used from the ArangoDB source tree.

#### MakeData / CheckData suite

The [makedata framework](https://github.com/arangodb/rta-makedata) as git submodule in [3rdParty/rta-makedata](3rdParty/rta-makedata/)
is implemented in arangosh javascript.
It uses the respective interface to execute DDL and DML operations. 
It facilitates a per database approach, and can be run multiple times in loops. 
It has hooks, that are intended to create DDL/DML objects in a way their existence
can be revalidated later on by other script hooks. The check hooks must respect a
flag whether they can create resources or whether they're talking to a read-only source.
Thus, the check-hooks may create data, but must remove it on success.
The checks should be considered not as time intense as the creation of the data.

It is used by RTA to:

- revalidate data can be created
- data survives hot backup / restore
- data makes it across the replication
- data is accessible with clusters with one failing DB-Server
- data makes it across DC2DC replication

With this integration additional DDL/DML checks can be more easily be developed without the
rather time and resource consuming and complex RTA framework.

The `rta_makedata` testsuite can be invoked with:

- `--cluster false` - to be ran on a single server setup.
- `--cluster true` to be ran on a 3 db-server node cluster; one run will check resilience with 2 remaining dbservers.

These combinations are also engaged via [test-definitions.txt](tests/test-definitions.txt).

Invoke it like this:

    ./scripts/unittest rta_makedata --cluster true

(you can override the 3rdParty/rta-makedata with `--rtasource ../rta-makedata` ,if you want to work with a full git clone of RTA-makedata)

### Driver tests

#### Go driver

Pre-requisites:

- have a go-driver checkout next to the ArangoDB source tree
- have the go binary in the path

Once this is completed, you may run it like this:

    ./scripts/unittest go_driver --gosource ../go-driver/ --testCase View --goOptions:timeout 180m --cluster true

This will invoke the test with a filter to only execute tests that have `View` in their name.
As an additional parameter we pass `-timeout 100m` to the driver test.

The driver integration also features JWT pass in. It will launch a cluster with 3 DB-Servers, as
the tests expect to have at least 3 DB-Servers.

#### Java driver

Pre-requisites:

- have a arangodb-java-driver checkout next to the ArangoDB source tree in the 'next' branch
- have a maven binary in the path (mvn) configured to use JDK 11

You can check if maven is correctly configured to use JDK 11 executing: `mvn -version`.
In case you have multiple JVMs in your machine and JDK 11 is not the default one, you can set
the environment variable `JAVA_HOME` to the root path of JDK 11.

Once this is completed, you may run it like this:

    ./scripts/unittest java_driver --javasource ../arangodb-java-driver/ \
        --javaOptions:failIfNoTests false \
        --testCase com.arangodb.next.api.collection.CollectionApiTest#countAndDropCollection \
        --cluster true

For possible `javaOptions` see
[arangodb-java-driver/dev-README.md#test-provided-deployment](https://github.com/arangodb/arangodb-java-driver/blob/master/dev-README.md)
in the java source, or the
[surefire documentation](https://maven.apache.org/surefire/maven-surefire-plugin/examples/single-test.html)

#### ArangoJS

Pre-requisites:

- have a arangojs checkout next to the ArangoDB source tree
- have a nodejs and yarn binaries installed and in the path
- ran `yarn` once in the arangojs source tree

Once this is completed, you may run it like this:

    ./scripts/unittest js_driver --jssource ../arangojs/ \
        --testCase 'kills the given query' \
        --cluster true

#### arangodb-php

Pre-requisites:

- have a arangodb-php checkout next to the ArangoDB source tree
- have `php` and `phpunit` binaries installed and in the path

At the time being phpunit version 6.5 is supported. Install it like this:

    wget "https://phar.phpunit.de/phpunit-6.5.14.phar"
    mv phpunit-6.5.14.phar /usr/bin/phpunit
    chmod a+x /usr/bin/phpunit

Once this is completed, you may run it like this:

    ./scripts/unittest php_driver --phpsource ../arangodb-php/ \
        --testCase testSaveVerticesAndEdgeBetweenThemAndRemoveOneByOne \
        --cluster true \
        --phpkeepalive false

(without connection keepalive)

#### generic driver interface

The generic driver interface expects to find i.e. script inside the
driver source, that does all the plumbing to run the respective tests against
the provided arangodb instance.
The invoked script is expected to exit non-zero on failure.
All content of `stdout` will be forwarded to the testresults.
All required data is passed as parameters:

- driversource - the source directory with the workingcopy of the driver
- driverScript - the script to be executed. defaults to `run_tests.sh`
- driverScriptInterpreter - since currently there is no shebang support,
  this needs to be provided or defaults to `/bin/bash`.
- driverOptions options to be passed on to the driver works in the form of
  `--driverOptions.argname value` evaluating to `--argname` `value`
- `--test testcase` evaluates to `--testsuite testcase`
- `--testCase testcaseExp` evaluates to `--filter testcaseExp`

Statically provided options (with sample values):

- `--instanceUrl http://127.0.0.1:7327`
- `--instanceEndpoint tcp://127.0.0.1:7327`
- `--port 7327`
- `--host 127.0.0.1`
- `--auth false`
- `--username root`
- `--password ''`
- `--[no-]enterprise`
- `--deployment-mode [SINGLE_SERVER|CLUSTER]`

### Debugging Tests

This is quick introduction only.

Running a single arangosh client test:

    ./scripts/unittest shell_client --test api-import.js

Debugging arangosh with gdb:

    server> ./scripts/unittest shell_client --test api-import.js --server tcp://127.0.0.1:7777

    client> gdb --args ./build/bin/arangod --server.endpoint http+tcp://127.0.0.1:6666 \
                                           --server.authentication false \
                                           --log.level communication=trace \
                                           ../arangodb-data-test

Debugging a storage engine:

    host> rm -fr ../arangodb-data-rocksdb/; \
       gdb --args ./build/bin/arangod \
           --console \
           --foxx.queues false \
           --server.statistics false \
           --server.endpoint http+tcp://0.0.0.0:7777 \
           ../arangodb-data-rocksdb
    (gdb) catch throw
    (gdb) r
    arangod> require("jsunity").runTest("tests/js/client/shell/shell-client.js");

### Forcing downgrade from VPack to JSON

While velocypack is better for the machine to machine communication, JSON does a better job
if you want to observe the communication using `tcpdump`.
Hence a downgrade of the communication to JSON can be made at start time:

    arangosh --server.force-json true --server.endpoint ...

### Running tcpdump / windump for the SUT

Don't want to miss a beat of your test? If you want to invoke tcpdump with sudo,
make sure that your current shell has sudo enabled. Try like this:

    sudo -v; ./scripts/unittest http_server \
      --sniff sudo --cleanup false

The pcap file will end up in your tests temporary directory. You may need to
press an additional `ctrl+c` to force stop the sudo'ed tcpdump.

### Evaluating json test reports from previous testruns

All test results of testruns are dumped to a json file named
`UNITTEST_RESULT.json` which can be used for later analyzing of timings etc.

Currently available Analyzers are:

- unitTestPrettyPrintResults - Prints a pretty summary and writes an ASCII representation into `out/testfailures.txt` (if any errors)
- saveToJunitXML - saves jUnit compatible XML files
- locateLongRunning - searches the 10 longest running tests from a testsuite; may add comparison to `--otherFile` specified times
- locateShortServerLife - whether the servers lifetime for the tests isn't at least 10 times as long as startup/shutdown
- locateLongSetupTeardown - locate tests that may use a lot of time in setup/teardown
- yaml - dumps the json file as a yaml file
- unitTestTabularPrintResults - prints a table, add one (or more) of the following columns to print by adding it to `--tableColumns`:

  - `duration` - the time spent in the complete testfile
  - `status` - success/fail
  - `failed` - fail?
  - `total` - the time spent in the testcase
  - `totalSetUp` - the time spent in setup summarized
  - `totalTearDown` - the time spent in teardown summarized
  - `processStats.sum_servers.minorPageFaults` - Delta run values from `/proc/<pid>/io` summarized over all instances
  - `processStats.sum_servers.majorPageFaults` -
  - `processStats.sum_servers.userTime` -
  - `processStats.sum_servers.systemTime` -
  - `processStats.sum_servers.numberOfThreads` -
  - `processStats.sum_servers.residentSize` -
  - `processStats.sum_servers.residentSizePercent` -
  - `processStats.sum_servers.virtualSize` -
  - `processStats.sum_servers.rchar` -
  - `processStats.sum_servers.wchar` -
  - `processStats.sum_servers.syscr` -
  - `processStats.sum_servers.syscw` -
  - `processStats.sum_servers.read_bytes` -
  - `processStats.sum_servers.write_bytes` -
  - `processStats.sum_servers.cancelled_write_bytes` -
  - `processStats.sum_servers.sockstat_sockets_used` - Absolute values from `/proc/<pid>/net/sockstat` summarized over all instances
  - `processStats.sum_servers.sockstat_TCP_inuse` -
  - `processStats.sum_servers.sockstat_TCP_orphan` -
  - `processStats.sum_servers.sockstat_TCP_tw` -
  - `processStats.sum_servers.sockstat_TCP_alloc` -
  - `processStats.sum_servers.sockstat_TCP_mem` -
  - `processStats.sum_servers.sockstat_UDP_inuse` -
  - `processStats.sum_servers.sockstat_UDP_mem` -
  - `processStats.sum_servers.sockstat_UDPLITE_inuse` -
  - `processStats.sum_servers.sockstat_RAW_inuse` -
  - `processStats.sum_servers.sockstat_FRAG_inuse` -
  - `processStats.sum_servers.sockstat_FRAG_memory` -

  Process stats are kept by process.
  So if your DB-Server had the PID `1721882`, you can dial its values by specifying
  `processStats.1721882_dbserver.sockstat_TCP_tw`
  into the generated table.

i.e.

    ./scripts/examine_results.js -- 'yaml,locateLongRunning' --readFile out/UNITTEST_RESULT.json

or:

    ./scripts/examine_results.js -- 'unitTestTabularPrintResults' \
       --readFile out/UNITTEST_RESULT.json \
       --tableColumns 'duration,processStats.sum_servers.sockstat_TCP_orphan,processStats.sum_servers.sockstat_TCP_tw

revalidating one testcase using jq:

    jq '.shell_client."enterprise/tests/js/common/shell/smart-graph-enterprise-cluster.js"' < \
      out/UNITTEST_RESULT.json |grep sockstat_TCP_tw

getting the PIDs of the server in the testrun using jq:

    jq '.shell_client."enterprise/tests/js/common/shell/smart-graph-enterprise-cluster.js"' < \
      out/UNITTEST_RESULT.json |grep '"[0-9]*_[agent|dbserver|coordinator]'
    "1721674_agent": {
    "1721675_agent": {
    "1721676_agent": {
    "1721882_dbserver": {
    "1721883_dbserver": {
    "1721884_dbserver": {
    "1721885_coordinator": {

### Installing bash completion for `./scripts/unittest`

Calling

```bash
./scripts/buildUnittestBashCompletion.bash > ~/arango_unittest_comp.bash
```

generates a bash completion script for `./scripts/unittest` which can be sourced
in your `~/.bashrc` or `~/.bash_profile`:

```bash
. ~/arango_unittest_comp.bash
```

You can also install completions directly by running

```bash
  eval "$(./scripts/buildUnittestBashCompletion.bash)"
```

in your shell, or `.bashrc` etc., but note that it has to be executed in the
arangodb directory.

# Additional Resources

- [ArangoDB website](https://www.arangodb.com/)

- [ArangoDB on Twitter](https://twitter.com/arangodb)

- [General GitHub documentation](https://help.github.com/)

- [GitHub pull request documentation](https://docs.github.com/en/github/collaborating-with-pull-requests/proposing-changes-to-your-work-with-pull-requests/creating-a-pull-request-from-a-fork/)
