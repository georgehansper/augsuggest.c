#!/bin/bash

mkdir -p tmp/etc

echo '---------- hosts ------------'
./augsuggest --pretty --regexp --target=/etc/hosts test.hosts > test.hosts.augtool
AUGEAS_ROOT=$PWD/tmp augtool -f test.hosts.augtool --noload --autosave

diff -bu test.hosts tmp/etc/hosts
echo '======== diff without "-b" to ignore spaces ============================'
diff -u test.hosts tmp/etc/hosts

echo '---------- squid.conf ------------'
mkdir -p tmp/etc/squid
./augsuggest --pretty --regexp --target=/etc/squid/squid.conf test.squid.conf > test.squid.augtool
AUGEAS_ROOT=$PWD/tmp augtool -f test.squid.augtool --noload --autosave
diff -bu test.squid.conf tmp/etc/squid/squid.conf
echo '---------- sudoers ------------'
./augsuggest --pretty          --target=/etc/sudoers test.sudoers > test.sudoers.augtool
AUGEAS_ROOT=$PWD/tmp augtool -f test.sudoers.augtool --noload --autosave

echo '========== augeas does not re-create empty lines for most lenses =========='
echo '======== for new entries, we may get spaces where they are optional ======='
diff -bu test.sudoers tmp/etc/sudoers
