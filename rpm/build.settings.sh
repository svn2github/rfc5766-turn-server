#!/bin/bash

# Common settings script.

TURNVERSION=3.3.0.0
BUILDDIR=~/rpmbuild
ARCH=`uname -p`
TURNSERVER_SVN_URL=http://rfc5766-turn-server.googlecode.com/svn
TURNSERVER_SVN_URL_VER=trunk

WGETOPTIONS="--no-check-certificate"
RPMOPTIONS="-ivh --force"


