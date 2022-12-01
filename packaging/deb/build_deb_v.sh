#/bin/bash

NAME=qjournalctl
VERSION=0.6.3
PKGREV=1
OUT="$NAME""_$VERSION-$PKGREV"


# Build
QT_SELECT=qt5 qmake ../../qjournalctl.pro -r -spec linux-g++ CONFIG+=release
make -j$(nproc)

# Move required files
cp -r ../../packaging/files/* "$OUT/"
mkdir -p "$OUT/usr/bin"
mv qjournalctl "$OUT/usr/bin"

# debian pkg files
mkdir -p "$OUT/DEBIAN/"
cp ../../packaging/deb/control "$OUT/DEBIAN/"
dpkg-deb --build "$OUT"

# Keep ownership
mkdir -p release || true
mv "$OUT.deb" "release/$OUT.deb"
chown $USERID:$GROUPID "release/$OUT.deb"
