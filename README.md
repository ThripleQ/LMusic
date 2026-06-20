# simple-player

一个轻量级终端音乐播放器，支持本地音频播放和双面板目录浏览。

## 功能

- 支持 MP3、FLAC、WAV、OGG、AAC、M4A、WMA、Opus 等格式
- FFmpeg 流式解码（边播边解，低内存占用）
- 双面板界面：左侧目录列表，右侧歌曲列表
- 歌曲标签元数据读取（标题、歌手、专辑）
- 多目录扫描 + 持久化缓存（秒启动）
- 歌曲来源目录标注
- 进度条 ←/→ 跳播
- 列表循环 / 单曲循环
- 播放列表管理（删除、下一首、上一首）
- >> 原子变量线程安全，无竞态

## 编译

```bash
# 需要：gcc, FFmpeg, ALSA, ncurses
sudo pacman -S gcc ffmpeg alsa-lib ncurses
# 或 apt (Debian/Ubuntu)
sudo apt install gcc libavformat-dev libavcodec-dev \
    libavutil-dev libswresample-dev libasound2-dev libncurses-dev

cd src
gcc -O2 player.c decoder.c -lasound -lncurses -lpthread \
    -lavformat -lavcodec -lavutil -lswresample -o player
```

## 使用

```bash
./player                    # 读配置目录
./player ~/Music            # 追加目录并保存
./player ~/Music ~/Downloads # 追加多个
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
| `Ctrl+R` | 刷新缓存（重新扫描目录） |
| `Ctrl+C` | 退出（按两次） |

## 配置文件

- 目录列表: `~/.config/simple-player/dirs`
- 歌曲缓存: `~/.cache/simple-player/library.db`

删除缓存后重启会自动重新扫描。

## 架构

```
player.c        — 主程序、UI、播放线程
decoder.c/h     — FFmpeg 流式解码封装
```

Song 抽象设计预留了 `SRC_NETEASE` 源类型，便于未来接入在线音乐。
