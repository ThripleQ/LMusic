package main

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strings"

	"github.com/go-musicfox/netease-music/service"
	"github.com/go-musicfox/netease-music/util"
	"github.com/skip2/go-qrcode"
	"github.com/telanflow/cookiejar"
)

func main() {
	if len(os.Args) < 2 {
		fmt.Fprintln(os.Stderr, "usage: netease-cli <cmd> [args...]")
		os.Exit(1)
	}

	// Init cookie jar + UNM
	home, _ := os.UserHomeDir()
	cacheDir := filepath.Join(home, ".cache", "lmusic")
	os.MkdirAll(cacheDir, 0755)
	cookiePath := filepath.Join(cacheDir, "cookies.txt")
	jar, _ := cookiejar.NewFileJar(cookiePath, nil)
	util.SetGlobalCookieJar(jar)
	// 配置 UNM（UnblockNeteaseMusic）自动解锁受限歌曲
	util.ForceBestQuality = true
	util.EnableLocalVip = true
	util.UNMSwitch = true
	util.ConfigInit()

	cmd := os.Args[1]
	switch cmd {
	case "search":
		s := service.SearchService{
			S:     strings.Join(os.Args[2:], " "),
			Type:  "1",
			Limit: "30",
		}
		_, body := s.Search()
		output(body)

	case "song-url":
		if len(os.Args) < 3 {
			die("usage: netease-cli song-url <id> [level]")
		}
		id := os.Args[2]
		level := service.Standard
		if len(os.Args) > 3 {
			level = service.SongQualityLevel(os.Args[3])
		}
		// 策略同 musicfox：先 V1，受限则回退旧 API
		v1 := service.SongUrlV1Service{ID: id, Level: level, SkipUNM: false}
		v1Code, v1Body, v1Err := v1.SongUrl()
		v1Url := ""
		var v1Data map[string]interface{}
		if v1Err == nil && json.Unmarshal(v1Body, &v1Data) == nil {
			if items, ok := v1Data["data"].([]interface{}); ok && len(items) > 0 {
				if item, ok := items[0].(map[string]interface{}); ok {
					if u, ok := item["url"].(string); ok {
						v1Url = u
					}
					// V1 返回非 200 码或 freeTrialInfo（受限），强制回退
					if itemCode, _ := item["code"].(float64); itemCode != 0 && itemCode != 200 {
						v1Url = ""
					} else if _, hasTrial := item["freeTrialInfo"]; hasTrial && item["freeTrialInfo"] != nil {
						v1Url = ""
					}
				}
			}
		}
		// 最终结果
		var finalCode float64
		var finalBody []byte
		if v1Url != "" {
			finalCode = v1Code
			finalBody = v1Body
		} else {
			// 回退旧 API（musicfox 映射: standard/higher/exhigh→320000, lossless/hires→999000）
			br := "320000"
			if level == service.Lossless || level == service.Hires {
				br = "999000"
			}
			oldSvc := service.SongUrlService{ID: id, Br: br}
			finalCode, finalBody = oldSvc.SongUrl()
		}
		// 以标准格式输出
		var result map[string]interface{}
		json.Unmarshal(finalBody, &result)
		result["code"] = finalCode
		b, _ := json.Marshal(result)
		fmt.Println(string(b))

	case "song-detail":
		if len(os.Args) < 3 {
			die("usage: netease-cli song-detail <ids>")
		}
		s := service.SongDetailService{Ids: os.Args[2]}
		_, body := s.SongDetail()
		output(body)

	case "playlist":
		if len(os.Args) < 3 {
			die("usage: netease-cli playlist <id>")
		}
		s := service.PlaylistDetailService{Id: os.Args[2], S: "0"}
		_, body := s.PlaylistDetail()
		output(body)

	case "login-email":
		if len(os.Args) < 4 {
			die("usage: netease-cli login-email <email> <password>")
		}
		s := service.LoginEmailService{
			Email:    os.Args[2],
			Password: os.Args[3],
		}
		_, body := s.LoginEmail()
		output(body)

	case "login-cellphone":
		if len(os.Args) < 4 {
			die("usage: netease-cli login-cellphone <phone> <password>")
		}
		s := service.LoginCellphoneService{
			Phone:    os.Args[2],
			Password: os.Args[3],
		}
		_, body, _ := s.LoginCellphone()
		output(body)

	case "login-refresh":
		s := service.LoginRefreshService{}
		_, body, _ := s.LoginRefresh()
		output(body)

	case "login-status":
		output([]byte(fmt.Sprintf("{\"status\":\"check %s\"}", cookiePath)))

	case "user-playlist":
		if len(os.Args) < 3 {
			die("usage: netease-cli user-playlist <uid>")
		}
		s := service.UserPlaylistService{Uid: os.Args[2]}
		_, body := s.UserPlaylist()
		output(body)

	case "liked": {
		// 获取用户信息
		accountSvc := service.UserAccountService{}
		_, acctBody := accountSvc.AccountInfo()
		var acctData map[string]interface{}
		if err := json.Unmarshal(acctBody, &acctData); err != nil {
			die(fmt.Sprintf("parse account failed: %v", err))
		}
		uid := int64(0)
		if acct, ok := acctData["account"].(map[string]interface{}); ok {
			if id, ok := acct["id"].(float64); ok {
				uid = int64(id)
			}
		}
		if uid == 0 {
			die("failed to get uid, need login first")
		}

		// 获取红心歌曲 ID 列表
		likeSvc := service.LikeListService{UID: fmt.Sprintf("%d", uid)}
		_, body := likeSvc.LikeList()
		var likeData map[string]interface{}
		if err := json.Unmarshal(body, &likeData); err != nil {
			die(fmt.Sprintf("parse liked failed: %v", err))
		}
		idsRaw, ok := likeData["ids"].([]interface{})
		if !ok || len(idsRaw) == 0 {
			die("no liked songs or parse failed")
		}
		// 拼成逗号分隔字符串
		var idStrs []string
		for _, id := range idsRaw {
			if f, ok := id.(float64); ok {
				idStrs = append(idStrs, fmt.Sprintf("%.0f", f))
			}
		}
		if len(idStrs) == 0 {
			die("no liked songs")
		}
		// 分批调 SongDetail（网易云 API 对单次查询有限制），合并结果
		var allSongs []interface{}
		batchSize := 200
		for i := 0; i < len(idStrs); i += batchSize {
			end := i + batchSize
			if end > len(idStrs) { end = len(idStrs) }
			batch := strings.Join(idStrs[i:end], ",")
			detailSvc := service.SongDetailService{Ids: batch}
			_, detailBody := detailSvc.SongDetail()
			var batchData map[string]interface{}
			json.Unmarshal(detailBody, &batchData)
			if songs, ok := batchData["songs"].([]interface{}); ok {
				allSongs = append(allSongs, songs...)
			}
		}
		if len(allSongs) > 0 {
			out := map[string]interface{}{"code": 200, "result": map[string]interface{}{"songs": allSongs}}
			b, _ := json.Marshal(out)
			fmt.Println(string(b))
		} else {
			fmt.Println()
		}
	}
	
	case "recommend-songs":
		s := service.RecommendSongsService{}
		_, body := s.RecommendSongs()
		output(body)

	case "qr-render":
		if len(os.Args) < 3 {
			die("usage: netease-cli qr-render <url>")
		}
		qr, err := qrcode.New(os.Args[2], qrcode.Medium)
		if err != nil {
			die(fmt.Sprintf("qr error: %v", err))
		}
		fmt.Print(qr.ToSmallString(false))

	case "qr-key":
		s := service.LoginQRService{}
		_, _, qrUrl, err := s.GetKey()
		if err != nil {
			die(fmt.Sprintf("get qr key failed: %v", err))
		}
		// 不用 json.Marshal — URL 里的 & 会被编码成 \u0026 破坏二维码
		fmt.Printf("{\"unikey\":\"%s\",\"url\":\"%s\"}\n", s.UniKey, qrUrl)

	case "qr-check":
		if len(os.Args) < 3 {
			die("usage: netease-cli qr-check <unikey>")
		}
		s := service.LoginQRService{UniKey: os.Args[2]}
		code, body, err := s.CheckQR()
		if err != nil {
			die(fmt.Sprintf("check failed: %v", err))
		}
		if code == 803 {
			var resp map[string]interface{}
			if err := json.Unmarshal(body, &resp); err == nil {
				if c, _ := resp["cookie"].(string); c != "" { saveNeteaseCookies(c) }
				if d, ok := resp["data"].(map[string]interface{}); ok {
					if c, _ := d["cookie"].(string); c != "" { saveNeteaseCookies(c) }
				}
			}
		}
		fmt.Printf("{\"code\":%.0f,\"body\":%s}\n", code, string(body))

case "playlists": {
	accountSvc := service.UserAccountService{}
	_, acctBody := accountSvc.AccountInfo()
	var acctData map[string]interface{}
	if err := json.Unmarshal(acctBody, &acctData); err != nil {
		die(fmt.Sprintf("parse account failed: %v", err))
	}
	uid := int64(0)
	if acct, ok := acctData["account"].(map[string]interface{}); ok {
		if id, ok := acct["id"].(float64); ok {
			uid = int64(id)
		}
	}
	if uid == 0 {
		die("failed to get uid, need login first")
	}
	type plItem struct {
		ID   int64  `json:"id"`
		Name string `json:"name"`
	}
	var allItems []plItem
	offset := 0
	limit := 100
	for {
		svc := service.UserPlaylistService{Uid: fmt.Sprintf("%d", uid), Limit: fmt.Sprintf("%d", limit), Offset: fmt.Sprintf("%d", offset)}
		_, body := svc.UserPlaylist()
		var raw map[string]interface{}
		if err := json.Unmarshal(body, &raw); err != nil {
			fmt.Println(string(body))
			return
		}
		playlistRaw, ok := raw["playlist"].([]interface{})
		if !ok || len(playlistRaw) == 0 {
			break
		}
		for _, p := range playlistRaw {
			if pm, ok := p.(map[string]interface{}); ok {
				var item plItem
				if id, ok := pm["id"].(float64); ok { item.ID = int64(id) }
				if name, ok := pm["name"].(string); ok { item.Name = name }
				if item.ID > 0 { allItems = append(allItems, item) }
			}
		}
		if len(playlistRaw) < limit {
			break
		}
		offset += limit
	}
	out := map[string]interface{}{"code": 200, "playlists": allItems}
	b, _ := json.Marshal(out)
	fmt.Println(string(b))
}

	case "playlist-tracks": {
	if len(os.Args) < 3 {
		die("usage: netease-cli playlist-tracks <id>")
	}
	svc := service.PlaylistDetailService{Id: os.Args[2], S: "0"}
	_, body := svc.PlaylistDetail()
	var raw map[string]interface{}
	if err := json.Unmarshal(body, &raw); err != nil {
		fmt.Println(string(body))
		return
	}
	pl, ok := raw["playlist"].(map[string]interface{})
	if !ok {
		fmt.Println(string(body))
		return
	}
	tracks, ok := pl["tracks"].([]interface{})
	if !ok {
		fmt.Println(string(body))
		return
	}
	out := map[string]interface{}{"code": 200, "result": map[string]interface{}{"songs": tracks}}
	b, _ := json.Marshal(out)
	fmt.Println(string(b))
}

case "account-name":
	acctSvc := service.UserAccountService{}
	_, body := acctSvc.AccountInfo()
	var raw map[string]interface{}
	if json.Unmarshal(body, &raw) != nil {
		fmt.Println("error")
		return
	}
	name := ""
	if prof, ok := raw["profile"].(map[string]interface{}); ok {
		if n, ok := prof["nickname"].(string); ok { name = n }
	}
	if name == "" {
		if acct, ok := raw["account"].(map[string]interface{}); ok {
			if n, ok := acct["userName"].(string); ok { name = n }
		}
	}
	if name != "" {
		fmt.Println(name)
	} else {
		fmt.Println("未登录")
	}

	default:
		fmt.Fprintf(os.Stderr, "unknown cmd: %s\n", cmd)
		os.Exit(1)
	}
}

func output(body []byte) {
	var pretty map[string]interface{}
	if err := json.Unmarshal(body, &pretty); err == nil {
		b, _ := json.Marshal(pretty)
		os.Stdout.Write(b)
		os.Stdout.Write([]byte("\n"))
	} else {
		os.Stdout.Write(body)
	}
}

func die(msg string) {
	fmt.Fprintln(os.Stderr, msg)
	os.Exit(1)
}

func saveNeteaseCookies(cookieStr string) {
	home, _ := os.UserHomeDir()
	cp := filepath.Join(home, ".cache", "lmusic", "cookies.txt")
	os.MkdirAll(filepath.Dir(cp), 0755)
	parts := strings.Split(cookieStr, ";")
	for _, part := range parts {
		part = strings.TrimSpace(part)
		kv := strings.SplitN(part, "=", 2)
		if len(kv) != 2 {
			continue
		}
		n := kv[0]
		if strings.EqualFold(n, "Path") || strings.EqualFold(n, "Domain") ||
			strings.EqualFold(n, "Expires") || strings.EqualFold(n, "Max-Age") ||
			strings.EqualFold(n, "Secure") || strings.EqualFold(n, "HttpOnly") ||
			strings.EqualFold(n, "SameSite") {
			continue
		}
		f, _ := os.OpenFile(cp, os.O_APPEND|os.O_CREATE|os.O_WRONLY, 0600)
		if f != nil {
			fmt.Fprintf(f, "music.163.com\tFALSE\t/\tFALSE\t253402300799\t%s\t%s\n", n, kv[1])
			f.Close()
		}
	}
}
