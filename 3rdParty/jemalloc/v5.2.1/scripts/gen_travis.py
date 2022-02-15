#!/usr/bin/env python3

from itertools import combinations, chain
from enum import Enum, auto


LINUX = 'linux'
OSX = 'osx'
WINDOWS = 'windows'


AMD64 = 'amd64'
ARM64 = 'arm64'
PPC64LE = 'ppc64le'


TRAVIS_TEMPLATE = """\
# This config file is generated by ./scripts/gen_travis.py.
# Do not edit by hand.

# We use 'minimal', because 'generic' makes Windows VMs hang at startup. Also
# the software provided by 'generic' is simply not needed for our tests.
# Differences are explained here:
# https://docs.travis-ci.com/user/languages/minimal-and-generic/
language: minimal
dist: focal

jobs:
  include:
{jobs}

before_install:
  - |-
    if test -f "./scripts/$TRAVIS_OS_NAME/before_install.sh"; then
      source ./scripts/$TRAVIS_OS_NAME/before_install.sh
    fi

before_script:
  - |-
    if test -f "./scripts/$TRAVIS_OS_NAME/before_script.sh"; then
      source ./scripts/$TRAVIS_OS_NAME/before_script.sh
    else
      scripts/gen_travis.py > travis_script && diff .travis.yml travis_script
      autoconf
      # If COMPILER_FLAGS are not empty, add them to CC and CXX
      ./configure ${{COMPILER_FLAGS:+ CC="$CC $COMPILER_FLAGS" \
CXX="$CXX $COMPILER_FLAGS"}} $CONFIGURE_FLAGS
      make -j3
      make -j3 tests
    fi

script:
  - |-
    if test -f "./scripts/$TRAVIS_OS_NAME/script.sh"; then
      source ./scripts/$TRAVIS_OS_NAME/script.sh
    else
      make check
    fi
"""


class Option(object):
    class Type:
        COMPILER = auto()
        COMPILER_FLAG = auto()
        CONFIGURE_FLAG = auto()
        MALLOC_CONF = auto()
        FEATURE = auto()

    def __init__(self, type, value):
        self.type = type
        self.value = value

    @staticmethod
    def as_compiler(value):
        return Option(Option.Type.COMPILER, value)

    @staticmethod
    def as_compiler_flag(value):
        return Option(Option.Type.COMPILER_FLAG, value)

    @staticmethod
    def as_configure_flag(value):
        return Option(Option.Type.CONFIGURE_FLAG, value)

    @staticmethod
    def as_malloc_conf(value):
        return Option(Option.Type.MALLOC_CONF, value)

    @staticmethod
    def as_feature(value):
        return Option(Option.Type.FEATURE, value)

    def __eq__(self, obj):
        return (isinstance(obj, Option) and obj.type == self.type
                and obj.value == self.value)


# The 'default' configuration is gcc, on linux, with no compiler or configure
# flags.  We also test with clang, -m32, --enable-debug, --enable-prof,
# --disable-stats, and --with-malloc-conf=tcache:false.  To avoid abusing
# travis though, we don't test all 2**7 = 128 possible combinations of these;
# instead, we only test combinations of up to 2 'unusual' settings, under the
# hope that bugs involving interactions of such settings are rare.
MAX_UNUSUAL_OPTIONS = 2


GCC = Option.as_compiler('CC=gcc CXX=g++')
CLANG = Option.as_compiler('CC=clang CXX=clang++')
CL = Option.as_compiler('CC=cl.exe CXX=cl.exe')


compilers_unusual = [CLANG,]


CROSS_COMPILE_32BIT = Option.as_feature('CROSS_COMPILE_32BIT')
feature_unusuals = [CROSS_COMPILE_32BIT]


configure_flag_unusuals = [Option.as_configure_flag(opt) for opt in (
    '--enable-debug',
    '--enable-prof',
    '--disable-stats',
    '--disable-libdl',
    '--enable-opt-safety-checks',
    '--with-lg-page=16',
)]


malloc_conf_unusuals = [Option.as_malloc_conf(opt) for opt in (
    'tcache:false',
    'dss:primary',
    'percpu_arena:percpu',
    'background_thread:true',
)]


all_unusuals = (compilers_unusual + feature_unusuals
    + configure_flag_unusuals + malloc_conf_unusuals)


def get_extra_cflags(os, compiler):
    if os == WINDOWS:
        # For non-CL compilers under Windows (for now it's only MinGW-GCC),
        # -fcommon needs to be specified to correctly handle multiple
        # 'malloc_conf' symbols and such, which are declared weak under Linux.
        # Weak symbols don't work with MinGW-GCC.
        if compiler != CL.value:
            return ['-fcommon']
        else:
            return []

    # We get some spurious errors when -Warray-bounds is enabled.
    extra_cflags = ['-Werror', '-Wno-array-bounds']
    if compiler == CLANG.value or os == OSX:
        extra_cflags += [
            '-Wno-unknown-warning-option',
            '-Wno-ignored-attributes'
        ]
    if os == OSX:
        extra_cflags += [
            '-Wno-deprecated-declarations',
        ]
    return extra_cflags


# Formats a job from a combination of flags
def format_job(os, arch, combination):
    compilers = [x.value for x in combination if x.type == Option.Type.COMPILER]
    assert(len(compilers) <= 1)
    compiler_flags = [x.value for x in combination if x.type == Option.Type.COMPILER_FLAG]
    configure_flags = [x.value for x in combination if x.type == Option.Type.CONFIGURE_FLAG]
    malloc_conf = [x.value for x in combination if x.type == Option.Type.MALLOC_CONF]
    features = [x.value for x in combination if x.type == Option.Type.FEATURE]

    if len(malloc_conf) > 0:
        configure_flags.append('--with-malloc-conf=' + ','.join(malloc_conf))

    if not compilers:
        compiler = GCC.value
    else:
        compiler = compilers[0]

    extra_environment_vars = ''
    cross_compile = CROSS_COMPILE_32BIT.value in features
    if os == LINUX and cross_compile:
        compiler_flags.append('-m32')

    features_str = ' '.join([' {}=yes'.format(feature) for feature in features])

    stringify = lambda arr, name: ' {}="{}"'.format(name, ' '.join(arr)) if arr else ''
    env_string = '{}{}{}{}{}{}'.format(
            compiler,
            features_str,
            stringify(compiler_flags, 'COMPILER_FLAGS'),
            stringify(configure_flags, 'CONFIGURE_FLAGS'),
            stringify(get_extra_cflags(os, compiler), 'EXTRA_CFLAGS'),
            extra_environment_vars)

    job = '    - os: {}\n'.format(os)
    job += '      arch: {}\n'.format(arch)
    job += '      env: {}'.format(env_string)
    return job


def generate_unusual_combinations(unusuals, max_unusual_opts):
    """
    Generates different combinations of non-standard compilers, compiler flags,
    configure flags and malloc_conf settings.

    @param max_unusual_opts: Limit of unusual options per combination.
    """
    return chain.from_iterable(
            [combinations(unusuals, i) for i in range(max_unusual_opts + 1)])


def included(combination, exclude):
    """
    Checks if the combination of options should be included in the Travis
    testing matrix.

    @param exclude: A list of options to be avoided.
    """
    return not any(excluded in combination for excluded in exclude)


def generate_jobs(os, arch, exclude, max_unusual_opts, unusuals=all_unusuals):
    jobs = []
    for combination in generate_unusual_combinations(unusuals, max_unusual_opts):
        if included(combination, exclude):
            jobs.append(format_job(os, arch, combination))
    return '\n'.join(jobs)


def generate_linux(arch):
    os = LINUX

    # Only generate 2 unusual options for AMD64 to reduce matrix size
    max_unusual_opts = MAX_UNUSUAL_OPTIONS if arch == AMD64 else 1

    exclude = []
    if arch == PPC64LE:
        # Avoid 32 bit builds and clang on PowerPC
        exclude = (CROSS_COMPILE_32BIT, CLANG,)

    return generate_jobs(os, arch, exclude, max_unusual_opts)


def generate_macos(arch):
    os = OSX

    max_unusual_opts = 1

    exclude = ([Option.as_malloc_conf(opt) for opt in (
            'dss:primary',
            'percpu_arena:percpu',
            'background_thread:true')] +
        [Option.as_configure_flag('--enable-prof')] +
        [CLANG,])

    return generate_jobs(os, arch, exclude, max_unusual_opts)


def generate_windows(arch):
    os = WINDOWS

    max_unusual_opts = 3
    unusuals = (
        Option.as_configure_flag('--enable-debug'),
        CL,
        CROSS_COMPILE_32BIT,
    )
    return generate_jobs(os, arch, (), max_unusual_opts, unusuals)



def get_manual_jobs():
    return """\
    # Development build
    - os: linux
      env: CC=gcc CXX=g++ CONFIGURE_FLAGS="--enable-debug \
--disable-cache-oblivious --enable-stats --enable-log --enable-prof" \
EXTRA_CFLAGS="-Werror -Wno-array-bounds"
    # --enable-expermental-smallocx:
    - os: linux
      env: CC=gcc CXX=g++ CONFIGURE_FLAGS="--enable-debug \
--enable-experimental-smallocx --enable-stats --enable-prof" \
EXTRA_CFLAGS="-Werror -Wno-array-bounds"
"""


def main():
    jobs = '\n'.join((
        generate_linux(AMD64),
        generate_linux(PPC64LE),

        generate_macos(AMD64),

        #generate_windows(AMD64),

        get_manual_jobs()
    ))

    print(TRAVIS_TEMPLATE.format(jobs=jobs))


if __name__ == '__main__':
    main()
