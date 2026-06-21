# LMusic

轻量级终端音乐播放器，cmus 风格双面板界面，FFmpeg 流式解码 + 网易云音乐在线播放。

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![Go Report Card](https://goreportcard.com/badge/github.com/ThripleQ/LMusic)](https://goreportcard.com/report/github.com/ThripleQ/LMusic)
[![GitHub release](https://img.shields.io/github/v/release/ThripleQ/LMusic)](https://github.com/ThripleQ/LMusic/releases)

> **⚠️ 仅支持 Linux** — ALSA 音频输出，不兼容 macOS / Windows。

---

## 功能

### 本地播放
- 支持 MP3、FLAC、WAV、OGG、AAC、M4A、WMA、Opus
- FFmpeg 流式解码（零缓冲延迟）
- cmus 风格双面板：左目录 / 右歌曲，分隔线贯通
- 歌曲标签元数据读取（标题、歌手、专辑）
- 多目录扫描 + 持久化缓存（秒启动）
- 进度条 `━` + 跳播（←/→ ±5秒）
- 循环模式（不循环 / 单曲 / 列表）
- 播放列表管理（删除、下一首、上一首）
- 自动换源播放受限歌曲（UNM）

### 网易云音乐在线播放
- **搜索** — 在线搜索网易云曲库
- **❤ 红心歌单** — 你喜欢的音乐
- **每日推荐** — 网易云每日推荐
- **热歌榜** — 热门歌曲
- **收藏歌单** — 你的自建和收藏歌单
- **扫码登录** — 网易云 QR 码登录
- **自动换源** — 内置 UNM（UnblockNeteaseMusic），自动找可用源
- **异步加载** — 网络请求不卡界面，进度条动画过渡

### UI 特性
- 双面板异步加载，网络请求不卡 UI
- 当前播放歌曲 `>` 标记自动跟随
- 退出确认保护
- 自适应终端宽度
- 跑马灯滚动长歌名

## 安装

### 快速安装

```bash
# 从 Releases 下载
curl -L https://github.com/ThripleQ/LMusic/releases/latest/download/lmusic -o lmusic
chmod +x lmusic && mv lmusic ~/.local/bin/
```

### 从源码编译

依赖: `gcc`, `ffmpeg` (libavformat, libavcodec, libavutil, libswresample), `alsa-lib`, `ncurses`

```bash
git clone https://github.com/ThripleQ/LMusic.git
cd LMusic
make
cp lmusic ~/.local/bin/
```

**Arch Linux:**
```bash
pacman -S gcc ffmpeg alsa-lib ncurses
make && sudo make install
```

**Debian / Ubuntu:**
```bash
apt install gcc libavformat-dev libavcodec-dev \
    libavutil-dev libswresample-dev libasound2-dev libncurses-dev
make && sudo make install
```

**网易云功能还需要 Go 环境编译 CLI 桥接:**
```bash
cd netease-cli && go build -o netease-cli . && cp netease-cli ~/.local/bin/
```

## 使用

```bash
lmusic                       # 读配置目录并启动
lmusic ~/Music               # 追加目录并保存
lmusic ~/Music ~/Downloads   # 追加多个目录
```

### 按键

| 按键 | 功能 |
|------|------|
| `↑↓` | 左面板切换目录 / 右面板切换歌曲 |
| `Tab` | 切换左右面板 |
| `Enter` | 播放选中歌曲 |
| `Space` | 暂停 / 恢复 |
| `s` | 停止 |
| `← →` | 跳播 ±5 秒 |
| `n` `p` | 下一首 / 上一首 |
| `r` | 切换循环模式 |
| `d` | 从播放列表删除歌曲 |
| `q` | 退出（需确认） |
| `Ctrl+R` | 刷新目录缓存 |
| `Ctrl+C` | 快速退出（按两次） |

### 网易云按键

在左面板选中 `网易云` 目录：

| 按键 | 功能 |
|------|------|
| `l` | 扫码登录（二维码在终端内渲染） |
| `Enter` | 选择菜单项进入（搜索、红心歌单等） |
| `Esc` | 返回上级菜单 |

## 配置文件

- 目录列表: `~/.config/simple-player/dirs`
- 歌曲缓存: `~/.cache/simple-player/library.db`
- 网易云 Cookie: `~/.cache/lmusic/cookies.txt`

## 架构

```
player.c      — 主程序、ncurses UI、播放线程
decoder.c/h   — FFmpeg 流式解码封装（本地 + 在线）
netease.c/h   — 网易云 API JSON 解析（轻量 JSON 解析器，无第三方 JSON 库）
song.h        — Song 抽象（SRC_LOCAL / SRC_NETEASE）
netease-cli/  — Go 桥接 (go-musicfox/netease-music)，处理加密/签名
```

### 设计特点

- **Song 抽象**: 本地文件和网易云歌曲统一用 `Song` 结构体，`source` 字段区分来源
- **异步加载**: 网络请求通过 `popen()` + 非阻塞 fd 异步执行，UI 流畅不卡
- **轻量 JSON**: 网易云 JSON 解析仅用 `strstr` 和手动扫描，零依赖
- **内置换源**: 所有网易云歌曲 URL 请求都经过 UNM 层，自动寻找可播放源

## 致谢

- **网易云音乐 API**: [go-musicfox/netease-music](https://github.com/go-musicfox/netease-music) — 完整的网易云 API Go 封装，提供加密/签名/UNM 一站式方案
- **go-musicfox**: [go-musicfox/go-musicfox](https://github.com/go-musicfox/go-musicfox) — 优秀的终端网易云客户端，CLI 桥接思路受其启发
- **cmus**: [cmus/cmus](https://cmus.github.io) — 致敬最好的终端音乐播放器，UI 设计灵感来源
- **FFmpeg**: [FFmpeg](https://ffmpeg.org) — 强大的多媒体解码库
- **ncurses**: [Thomas Dickey](https://invisible-island.net/ncurses/) — 终端 UI 框架

## 许可

MIT
