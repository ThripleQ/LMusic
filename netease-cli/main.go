package main

import (
	"encoding/json"
	"fmt"
	"os"
	"strings"

	"github.com/go-musicfox/netease-music/service"
	"github.com/go-musicfox/netease-music/util"
	"github.com/telanflow/cookiejar"
)

func main() {
	if len(os.Args) < 2 {
		fmt.Fprintln(os.Stderr, "usage: netease-cli <cmd> [args...]")
		os.Exit(1)
	}

	// Init cookie jar for login persistence
	jar, _ := cookiejar.NewFileJar("cookie.txt", nil)
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
		output([]byte("{\"status\":\"check cookie.txt\"}"))

	case "user-playlist":
		if len(os.Args) < 3 {
			die("usage: netease-cli user-playlist <uid>")
		}
		s := service.UserPlaylistService{Uid: os.Args[2]}
		_, body := s.UserPlaylist()
		output(body)

	case "recommend-songs":
		s := service.RecommendSongsService{}
		_, body := s.RecommendSongs()
		output(body)

	case "qr-key":
		s := service.LoginQRService{}
		_, body, qrUrl, err := s.GetKey()
		if err != nil {
			die(fmt.Sprintf("get qr key failed: %v", err))
		}
		// 返回 {unikey, url}
		resp := map[string]string{
			"unikey": s.UniKey,
			"url":    qrUrl,
		}
		b, _ := json.Marshal(resp)
		fmt.Println(string(b))
		_ = body

	case "qr-check":
		if len(os.Args) < 3 {
			die("usage: netease-cli qr-check <unikey>")
		}
		s := service.LoginQRService{UniKey: os.Args[2]}
		code, body, err := s.CheckQR()
		if err != nil {
			die(fmt.Sprintf("check failed: %v", err))
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
