#!/bin/sh

set -e

image=$1
arch=$2

docker build --platform linux/$arch -t $image:latest-$arch --file arangodb-build.Dockerfile .

echo "Built docker image $image:latest-$arch"
