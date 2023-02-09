#!/usr/bin/bash

NAME=qjournalctl
VERSION=0.6.3
PKGREV=3
OUT="$NAME""_$VERSION-$PKGREV"


# Build
QT_SELECT=qt5 qmake ../../qjournalctl.pro -r -spec linux-g++ CONFIG+=release
make -j$(nproc)

# Move required files
mkdir -p $OUT/usr/bin
cp -r ../../packaging/files/usr $OUT/
mv qjournalctl $OUT/usr/bin/

# debian pkg files
mkdir -p "$OUT/DEBIAN/"
cp ../../packaging/deb/control "$OUT/DEBIAN/"
dpkg-deb --build "$OUT"

# Keep ownership
mkdir -p release || true
mv "$OUT.deb" "release/$OUT.deb"
chown $USERID:$GROUPID "release/$OUT.deb"
