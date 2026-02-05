# Maintainer: Martin Munk Hansen <hansen@martinmunk.com>
pkgname=martinmunk-alsa-control-avr
pkgver=0.1.0
pkgrel=0
pkgdesc="ALSA control plugins for AV receivers"
url="http://gitea.servers.vejen.loc.martinmunk.com:3000/martinmunk/alsa-avr-control"
arch="all"
license="custom"
depends="alsa-lib"
makedepends="cmake alsa-lib-dev"
source=""
builddir="$srcdir"

build() {
	if [ ! -d "$builddir/build" ]; then
		mkdir -p "$builddir/build"
	fi
	cd "$builddir/build"
	cmake "$builddir" \
		-DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_INSTALL_PREFIX=/usr \
		-DBUILD_TESTS=OFF
	make -j${JOBS:-2}
}

package() {
	cd "$builddir/build"
	make DESTDIR="$pkgdir" install
}
