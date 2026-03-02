#!/bin/bash

if [ ! -d external/aap-core/external/cmidi2 ] ; then
    cd external/aapcore && git submodule update --init --recursive & cd ../.. ;
fi

if [ ! -f external/aap-core/local.properties ] ; then
    if [ "`uname`" == 'Darwin' ] ; then
        echo "sdk.dir=$HOME/Library/Android/sdk" > external/aap-core/local.properties ;
    else
        echo "sdk.dir=$HOME/Android/sdk" > external/aap-core/local.properties ;
    fi ;
fi
cd external/aap-core && ./gradlew publishToMavenLocal && cd ../..
./gradlew build

