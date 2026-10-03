# Maintainer: Rusty <rusty@localhost>
# Contributor: breike

pkgname=icewm
pkgver=4.1.0
pkgrel=1
pkgdesc="Window Manager designed for speed, usability, and consistency"
arch=('x86_64')
url="https://ice-wm.org/"
license=('LGPL-2.0-only')
depends=('alsa-lib' 'imlib2' 'libsm' 'librsvg' 'libsndfile' 'libxcomposite' 'libxcursor' 'libxdamage' 'libxinerama' 'libxrandr')
makedepends=('cmake' 'ninja' 'git' 'asciidoctor' 'xorg-mkfontscale')
optdepends=('perl: for icewm-menu-xrandr')

# Source comes from the breike fork (the local icewm origin), not from
# upstream: the fork carries the flex/tiling additions that are not yet
# merged upstream. master is a moving target, so the checksum is
# intentionally SKIP — a fresh snapshot is fetched on every build.
source=("$pkgname::git+https://github.com/breike/icewm")
b2sums=('SKIP')

pkgver() {
  cd "$pkgname"

  # The breike fork carries no tags (they only exist upstream), so a fresh
  # clone has none and `git describe --tags` would fail. Fall back to a
  # date-based version for that case.
  local v
  v="$(git describe --tags 2>/dev/null || true)"
  if [ -z "$v" ]; then
    v="4.1.0.$(git log -1 --format=%cd --date=format:%Y%m%d)"
  fi
  echo "$v" | sed 's/-/./g'
}

build() {
  cmake -B build -S "$pkgname" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -Wno-dev

  cmake --build build
}

package() {
  DESTDIR="$pkgdir" cmake --install build

  # Remove the directory tree created for development purposes only
  rm -rf "${pkgdir}/build"
}