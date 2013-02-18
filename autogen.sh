#!/bin/sh
# $Id$

if [ "x$(uname)" = "xOpenBSD" ]; then
	[ -z "$AUTOMAKE_VERSION" ] && export AUTOMAKE_VERSION=1.10
	[ -z "$AUTOCONF_VERSION" ] && export AUTOCONF_VERSION=2.65
fi

die()
{
    echo "$@" >&2
    exit 1
}

mkdir -p etc
aclocal || die "aclocal failed"
automake --add-missing --force-missing --copy --foreign || die "automake failed"
autoreconf || die "autoreconf failed"
