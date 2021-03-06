#!/bin/bash

set -e 

base_name="dicomautomaton_webserver_debian_stable"

commit_id=$(git rev-parse HEAD)

clean_dirty="clean"
sstat=$(git diff --shortstat)
if [ ! -z "${sstat}" ] ; then
    clean_dirty="dirty"
fi

build_datetime=$(date '+%Y%m%_0d-%_0H%_0M%_0S')

reporoot=$(git rev-parse --show-toplevel)
cd "${reporoot}"

time sudo docker build \
    --network=host \
    --no-cache=true \
    -t "${base_name}":"built_${build_datetime}" \
    -t "${base_name}":"commit_${commit_id}_${clean_dirty}" \
    -t "${base_name}":latest \
    -f docker/builders/debian_stable/Dockerfile \
    .

