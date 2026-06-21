# LMusic

轻量级终端音乐播放器，cmus 风格双面板界面，FFmpeg 流式解码。

## 功能

- 支持 MP3、FLAC、WAV、OGG、AAC、M4A、WMA、Opus 等格式
- FFmpeg 流式解码（边播边解，低内存占用）
- cmus 风格双面板：左目录 / 右歌曲，分隔线贯通
- 歌曲标签元数据读取（标题、歌手、专辑）
- 多目录扫描 + 持久化缓存（秒启动）
- 进度条 `━` + 跳播（←/→ ±5秒）
- 循环模式（不循环 / 单曲 / 列表）
- 播放列表管理（删除、下一首、上一首）

## 安装

### 自动安装

```bash
chmod +x install.sh && ./install.sh
```

### 各发行版手动安装

**Arch Linux:**
```bash
sudo pacman -S gcc ffmpeg alsa-lib ncurses
make && sudo make install
```

**Debian / Ubuntu:**
```bash
sudo apt install gcc libavformat-dev libavcodec-dev \
    libavutil-dev libswresample-dev libasound2-dev libncurses-dev
make && sudo make install
```

**Fedora:**
```bash
sudo dnf install gcc ffmpeg-devel alsa-lib-devel ncurses-devel
make && sudo make install
```

**openSUSE:**
```bash
sudo zypper install gcc ffmpeg-devel alsa-devel ncurses-devel
make && sudo make install
```

**macOS (Homebrew):**
```bash
brew install gcc ffmpeg ncurses
make && sudo make install
```

### AUR (Arch Linux)

```bash
yay -S lmusic-git
```

## 使用

```bash
lmusic                       # 读配置目录
lmusic ~/Music               # 追加目录并保存
lmusic ~/Music ~/Downloads   # 追加多个
```

### 按键

| 按键 | 功能 |
|------|------|
| `↑↓` | 左面板切换目录 / 右面板切换歌曲 |
| `Tab` | 切换左右面板 |
| `Enter` | 播放选中歌曲 |
| `空格` | 暂停 / 恢复 |
| `s` | 停止 |
| `←→` | 跳播（±5秒） |
| `n` `p` | 下一首 / 上一首 |
| `r` | 切换循环模式 |
| `d` | 删除歌曲 |
| `q` | 退出（需确认） |
| `Ctrl+R` | 刷新缓存 |
| `Ctrl+C` | 退出（按两次） |

## 配置文件

- 目录列表: `~/.config/simple-player/dirs`
- 歌曲缓存: `~/.cache/simple-player/library.db`

## 架构

```
player.c        — 主程序、UI、播放线程
decoder.c/h     — FFmpeg 流式解码封装
```

Song 抽象设计预留 `SRC_NETEASE` 源类型，便于未来接入在线音乐。

## 许可

MIT
