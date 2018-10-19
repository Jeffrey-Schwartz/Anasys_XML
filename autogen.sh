#! /bin/sh

aclocal \
&& automake --add-missing \
&& autoconf \
&& ./configure \
&& dos2unix libtool
