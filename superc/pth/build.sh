#!/usr/bin/env bash

module load tools/impi/2018
module load plgrid/tools/cmake/3.7.2

pushd "$HOME"/ar-lab1/  > /dev/null

mkdir -p cmake-build-release
pushd cmake-build-release  > /dev/null

cmake -DCMAKE_C_COMPILER=icc -DCMAKE_CXX_COMPILER=icpc -DCMAKE_BUILD_TYPE=Release ../
make "$1"

popd  > /dev/null
popd  > /dev/null