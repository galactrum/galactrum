#!/usr/bin/env bash

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
cd $DIR/..

DOCKER_IMAGE=${DOCKER_IMAGE:-galactrum/galactrumd-develop}
DOCKER_TAG=${DOCKER_TAG:-latest}

BUILD_DIR=${BUILD_DIR:-.}

rm docker/bin/*
mkdir docker/bin
cp $BUILD_DIR/src/galactrumd docker/bin/
cp $BUILD_DIR/src/galactrum-cli docker/bin/
cp $BUILD_DIR/src/galactrum-tx docker/bin/
strip docker/bin/galactrumd
strip docker/bin/galactrum-cli
strip docker/bin/galactrum-tx

docker build --pull -t $DOCKER_IMAGE:$DOCKER_TAG -f docker/Dockerfile docker
