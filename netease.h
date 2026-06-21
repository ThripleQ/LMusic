// netease.h — 网易云音乐 API 接口
// 通过 netease-cli Go 桥接调用，不依赖 Node.js

#ifndef NETEASE_H
#define NETEASE_H

#include "song.h"

#define NETEASE_CLI "./netease-cli/netease-cli"

// ── 歌曲数据接口 ──────────────────────────────────────────

// 搜索歌曲，返回结果数。results 需预分配 MAX_NETEASE_SONGS 空间
int netease_search(const char *keyword, Song *results, int max);

// 获取歌曲播放直链 URL（MP3），返回 0 成功
int netease_song_url(const char *id, char *url, int url_len);

// 获取歌单详情（歌曲列表），返回结果数
int netease_playlist_detail(const char *id, Song *results, int max);

// 每日推荐歌曲，返回结果数
int netease_recommend_songs(Song *results, int max);

// 用户歌单列表（返回歌单 meta，不是歌曲），返回结果数
int netease_user_playlist(const char *uid, Song *results, int max);

// ── 登录 ──────────────────────────────────────────────────

// 手机号 + 密码登录。返回 0 成功，cookie 存入文件
int netease_login_cellphone(const char *phone, const char *password);

// 检查登录状态
int netease_login_status(void);

#endif // NETEASE_H
