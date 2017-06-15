#!/bin/bash

concurrency="$1"
engine="$2"
type="test_singleserver_community_mmfiles_linux"

echo "TYPE: $type"
echo "ENGINE: $engine"
echo "CONCURRENY: $concurrency"
echo "HOST: `hostname`"
echo "PWD: `pwd`"
echo "CORE PATTERN: `cat /proc/sys/kernel/core_pattern`"
echo "CORE LIMIT: `ulimit -c`"

echo
echo "ARANGOD VERSION: `build/bin/arangod --version`"

rm -rf core.* *.log out
rm -rf tmp && mkdir tmp
export TMPDIR=$(pwd)/tmp
export TEMPDIR=$(pwd)/tmp

PORT01=`./Installation/Pipeline/port.sh`
PORT02=`./Installation/Pipeline/port.sh`
PORT03=`./Installation/Pipeline/port.sh`

trap "./Installation/Pipeline/port.sh --clean $PORT01 $PORT02 $PORT03" EXIT

# note that: shebang does not work if path contains a '@'

OPTS="--storageEngine $engine --skipNondeterministic true --skipTimeCritical true  --configDir etc/jenkins --skipLogAnalysis true"

rm -rf log-output
mkdir log-output

echo "$type
scripts/unittest boost                     --skipCache false                                                                      2>&1
scripts/unittest agency                                       --minPort `expr $PORT01 +   0` --maxPort `expr $PORT01 +   9` $OPTS 2>&1
scripts/unittest arangobench                                  --minPort `expr $PORT01 +  10` --maxPort `expr $PORT01 +  19` $OPTS 2>&1
scripts/unittest arangosh                  --skipShebang true --minPort `expr $PORT01 +  20` --maxPort `expr $PORT01 +  29` $OPTS 2>&1
scripts/unittest authentication                               --minPort `expr $PORT01 +  30` --maxPort `expr $PORT01 +  39` $OPTS 2>&1
scripts/unittest authentication_parameters                    --minPort `expr $PORT01 +  40` --maxPort `expr $PORT01 +  49` $OPTS 2>&1
scripts/unittest cluster_sync                                 --minPort `expr $PORT01 +  50` --maxPort `expr $PORT01 +  59` $OPTS 2>&1
scripts/unittest config                                       --minPort `expr $PORT01 +  60` --maxPort `expr $PORT01 +  69` $OPTS 2>&1
scripts/unittest dfdb                                         --minPort `expr $PORT01 +  70` --maxPort `expr $PORT01 +  79` $OPTS 2>&1
scripts/unittest dump                                         --minPort `expr $PORT01 +  80` --maxPort `expr $PORT01 +  89` $OPTS 2>&1
scripts/unittest dump_authentication                          --minPort `expr $PORT01 +  90` --maxPort `expr $PORT01 +  99` $OPTS 2>&1
scripts/unittest endpoints                                    --minPort `expr $PORT02 + 100` --maxPort `expr $PORT02 + 109` $OPTS 2>&1
scripts/unittest http_replication                             --minPort `expr $PORT02 + 110` --maxPort `expr $PORT02 + 119` $OPTS 2>&1
scripts/unittest http_server                                  --minPort `expr $PORT02 + 120` --maxPort `expr $PORT02 + 129` $OPTS 2>&1
scripts/unittest replication_ongoing                          --minPort `expr $PORT02 + 130` --maxPort `expr $PORT02 + 139` $OPTS 2>&1
scripts/unittest replication_static                           --minPort `expr $PORT02 + 140` --maxPort `expr $PORT02 + 149` $OPTS 2>&1
scripts/unittest replication_sync                             --minPort `expr $PORT02 + 150` --maxPort `expr $PORT02 + 159` $OPTS 2>&1
scripts/unittest server_http                                  --minPort `expr $PORT02 + 160` --maxPort `expr $PORT02 + 169` $OPTS 2>&1
scripts/unittest shell_client                                 --minPort `expr $PORT02 + 170` --maxPort `expr $PORT02 + 179` $OPTS 2>&1
scripts/unittest shell_replication                            --minPort `expr $PORT02 + 180` --maxPort `expr $PORT02 + 189` $OPTS 2>&1
scripts/unittest shell_server                                 --minPort `expr $PORT02 + 190` --maxPort `expr $PORT02 + 199` $OPTS 2>&1
scripts/unittest shell_server_aql          --testBuckets 4/0  --minPort `expr $PORT03 +   0` --maxPort `expr $PORT03 +   9` $OPTS 2>&1
scripts/unittest shell_server_aql          --testBuckets 4/1  --minPort `expr $PORT03 +  10` --maxPort `expr $PORT03 +  19` $OPTS 2>&1
scripts/unittest shell_server_aql          --testBuckets 4/2  --minPort `expr $PORT03 +  20` --maxPort `expr $PORT03 +  29` $OPTS 2>&1
scripts/unittest shell_server_aql          --testBuckets 4/3  --minPort `expr $PORT03 +  30` --maxPort `expr $PORT03 +  39` $OPTS 2>&1
scripts/unittest ssl_server                                   --minPort `expr $PORT03 +  40` --maxPort `expr $PORT03 +  49` $OPTS 2>&1
scripts/unittest upgrade                                      --minPort `expr $PORT03 +  50` --maxPort `expr $PORT03 +  59` $OPTS 2>&1
" | parallel --header 1 --results log-output --files --no-notice --load 10 --jobs $concurrency > log-output/${type}.log

result=$?

echo
echo "================================================================================"
echo "Result: $result"
echo "Options: $OPTS"
echo

for file in log-output/*/*/stdout; do
    base=`echo "$file" | sed -e 's:^\\(log-output/[^/]*\\)/.*$:\\1:'`
    long=`echo "$file" | sed -e 's:/stdout$::'`
    short=`echo "$long" | sed -e 's:^.*_unittest *::' | sed -e 's: *--minPort.*$::' | sed -e 's:\\\\::g' | sed -e 's: *2>&1$::g' | sed -e 's:  *: :g'`

    mv -- "$long/stdout" "$base/$short.log"
    rm -rf -- "$long"
done

fgrep "Overall state: Fail" log-output/*/*/stdout

exit $result
