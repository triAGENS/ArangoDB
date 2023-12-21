#!/bin/sh

image=$1
arch=$2

GCC_VERSION=$(docker run --rm -it $image:latest-$arch g++ "--version" | grep -Po " \K\d+\.\d+(?=\.\d+ )")
CLANG_VERSION=$(docker run --rm -it $image:latest-$arch clang++ "--version" | grep -Po " \K\d+\.\d+(?=\.\d+)")
OPENSSL_VERSION=$(docker run --rm -it $image:latest-$arch sh -c "cat \${OPENSSL_ROOT_DIR}/lib*/pkgconfig/openssl.pc" | grep -Po "Version: \K\d+\.\d+\.\d+")
OS_VERSION=$(docker run --rm -it $image:latest-$arch cat /etc/alpine-release | grep -Po "\d+\.\d+")

echo ${OS_VERSION}-clang${CLANG_VERSION}-gcc${GCC_VERSION}-openssl${OPENSSL_VERSION}-$(git rev-parse --short HEAD)

