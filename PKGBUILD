# Maintainer: ThripleQ <thripleq@users.noreply.github.com>
pkgname=lmusic-git
pkgver=r50.fa73c17
pkgrel=1
pkgdesc="Lightweight cmus-style terminal music player with FFmpeg streaming decode"
arch=('x86_64')
url="https://github.com/ThripleQ/LMusic"
license=('MIT')
depends=('ffmpeg' 'alsa-lib' 'ncurses')
makedepends=('git' 'gcc')
provides=('lmusic')
conflicts=('lmusic')
source=("${pkgname}::git+https://github.com/ThripleQ/LMusic.git")
sha256sums=('SKIP')

build() {
  cd "$srcdir/${pkgname}"
  make
}

package() {
  cd "$srcdir/${pkgname}"
  install -Dm755 lmusic "$pkgdir/usr/bin/lmusic"
  install -Dm644 README.md "$pkgdir/usr/share/doc/lmusic/README.md"
}
