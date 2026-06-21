#!/bin/bash
set -e

echo "=== LMusic Installer ==="
echo ""

# Detect OS
if [ -f /etc/os-release ]; then
  . /etc/os-release
  OS=$ID
else
  OS=$(uname -s)
fi

install_deps() {
  case "$OS" in
    arch|manjaro)
      echo "[Arch] Installing dependencies..."
      sudo pacman -S --needed gcc ffmpeg alsa-lib ncurses
      ;;
    debian|ubuntu|linuxmint|pop)
      echo "[Debian/Ubuntu] Installing dependencies..."
      sudo apt update
      sudo apt install -y gcc libavformat-dev libavcodec-dev \
        libavutil-dev libswresample-dev libasound2-dev libncurses-dev
      ;;
    fedora)
      echo "[Fedora] Installing dependencies..."
      sudo dnf install -y gcc ffmpeg-devel alsa-lib-devel ncurses-devel
      ;;
    opensuse*|suse)
      echo "[openSUSE] Installing dependencies..."
      sudo zypper install -y gcc ffmpeg-devel alsa-devel ncurses-devel
      ;;
    Darwin)
      echo "[macOS] Installing dependencies..."
      if ! command -v brew &>/dev/null; then
        echo "请先安装 Homebrew: https://brew.sh"
        exit 1
      fi
      brew install gcc ffmpeg ncurses
      ;;
    *)
      echo "未知系统。请手动安装依赖：gcc, ffmpeg, alsa-lib, ncurses"
      echo "然后运行: make && sudo make install"
      exit 1
      ;;
  esac
}

# 确认
echo "将安装以下依赖: gcc, ffmpeg, alsa-lib, ncurses"
echo "检测到系统: $OS"
read -p "继续？[Y/n] " -n 1 -r
echo
if [[ ! $REPLY =~ ^[Yy]$ ]] && [ -n "$REPLY" ]; then
  echo "已取消"
  exit 0
fi

install_deps

echo ""
echo "=== 编译 LMusic ==="
make clean 2>/dev/null || true
make

echo ""
echo "=== 安装 LMusic ==="
sudo make install
echo ""
echo "LMusic 安装完成！运行 'lmusic' 启动。"
