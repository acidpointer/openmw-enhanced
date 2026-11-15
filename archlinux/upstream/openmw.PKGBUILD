# Maintainer: Sven-Hendrik Haase <svenstaro@archlinux.org>
# Contributor: txtsd <code@ihavea.quest>
# Contributor: Sandy Carter <bwrsandman@gmail.com>

pkgname=openmw
pkgver=0.51.0
pkgrel=3
pkgdesc="Open-source engine reimplementation for the role-playing game Morrowind"
arch=('x86_64')
url="http://www.openmw.org"
license=('GPL-3.0-or-later' 'OFL-1.1' 'LicenseRef-DejaVuLGCSansMono')
depends=('openal' 'openscenegraph' 'mygui' 'qt6-base' 'qt6-tools' 'qt6-svg' 'ffmpeg' 'sdl2' 'unshield'
         'libxt' 'boost-libs' 'bullet-dp' 'yaml-cpp' 'luajit' 'glibc' 'libgcc' 'libstdc++' 'zlib' 'icu'
         'lz4' 'sqlite')
makedepends=('cmake' 'boost' 'doxygen' 'ninja')
source=("https://gitlab.com/OpenMW/openmw/-/archive/openmw-${pkgver}/openmw-openmw-${pkgver}.tar.gz")
sha512sums=('a993896fe3f9e0c3c4b5d32cf893d88ac09cfefa8ef3afeb3c0e7b726a71272a07350109657effb1dfdc205db49cf02d23d10880d8468206f95b4cd7b146695f')

build() {
  cd openmw-openmw-$pkgver

  export CMAKE_POLICY_VERSION_MINIMUM=3.5

  cmake \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DCMAKE_BUILD_TYPE=Release \
    -DOPENMW_LTO_BUILD=ON \
    -GNinja \
    -Bbuild
  ninja -C build
}

package() {
  cd openmw-openmw-$pkgver
  DESTDIR="$pkgdir" ninja -C build install

  install -Dm644 LICENSE "$pkgdir"/usr/share/licenses/$pkgname/LICENSE
}

# vim: ts=2 sw=2 et:
