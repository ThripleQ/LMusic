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

	// Init cookie jar—使用固定路径，不受 CWD 影响
	home, _ := os.UserHomeDir()
	cacheDir := filepath.Join(home, ".cache", "lmusic")
	os.MkdirAll(cacheDir, 0755)
	cookiePath := filepath.Join(cacheDir, "cookies.txt")
	jar, _ := cookiejar.NewFileJar(cookiePath, nil)
	util.SetGlobalCookieJar(jar)

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
			die("usage: netease-cli song-url <id> [br]")
		}
		s := service.SongUrlService{ID: os.Args[2]}
		if len(os.Args) > 3 {
			s.Br = os.Args[3]
		}
		_, body := s.SongUrl()
		output(body)

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

	case "liked":
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

		likeSvc := service.LikeListService{UID: fmt.Sprintf("%d", uid)}
		_, body := likeSvc.LikeList()
		output(body)

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
		// login success → 再调一次 AccountInfo 让 cookie 保存到文件
		if code == 803 {
			accountSvc := service.UserAccountService{}
			accountSvc.AccountInfo()
		}
		fmt.Printf("{\"code\":%.0f,\"body\":%s}\n", code, string(body))

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
