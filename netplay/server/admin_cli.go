package main

import (
	"bytes"
	"encoding/hex"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"text/tabwriter"
	"time"

	"ac6lobby/internal/lobby"
)

const adminUsage = `usage: ac6lobby admin [-addr 127.0.0.1:47802] [-token-file FILE | -data DIR] [-json] <command>

commands:
  players [-addr]                  online players (-addr also shows their IP address)
  sessions                         sessions: host, mode, type, state, members
  kick <who> [reason...]           disconnect a player (they may reconnect)
  ban [-for 24h|7d] <who> [reason...]
                                   ban an account (permanent without -for) and disconnect it
  unban <who>                      lift a ban
  bans                             list active bans
  broadcast <message...>           send an info notice to every online client (logged by clients)
  forget <who>                     delete an account and its leaderboard rows (deletion requests)
  metrics                          counts, counters, CPU and memory
  audit [-n 50]                    recent admin actions, newest first (<data>/audit.jsonl)

<who> is a 16-hex-digit peer_id or a gamertag; force one with peer:<id> or tag:<name>.
The token file defaults to <data>/admin.token, searched in -data, $AC6LOBBY_DATA, ./data,
/var/lib/ac6lobby and /data. The panel is at http://127.0.0.1:47802/admin/ui/ (see
netplay/docs/HOSTING_A_SERVER.md for reaching it through an SSH tunnel).`

type adminClient struct {
	base  string
	token string
	hc    *http.Client
}

func findTokenFile(explicit, dataDir string) (string, error) {
	if explicit != "" {
		return explicit, nil
	}
	dirs := []string{}
	if dataDir != "" {
		dirs = append(dirs, dataDir)
	} else {
		if env := os.Getenv("AC6LOBBY_DATA"); env != "" {
			dirs = append(dirs, env)
		}
		dirs = append(dirs, "data", "/var/lib/ac6lobby", "/data")
	}
	for _, d := range dirs {
		p := filepath.Join(d, "admin.token")
		if _, err := os.Stat(p); err == nil {
			return p, nil
		}
	}
	return "", fmt.Errorf("no admin.token found in %s (pass -token-file or -data; is the server running with -admin?)",
		strings.Join(dirs, ", "))
}

func (c *adminClient) do(method, path string, body any, out any) error {
	var rd io.Reader
	if body != nil {
		b, err := json.Marshal(body)
		if err != nil {
			return err
		}
		rd = bytes.NewReader(b)
	}
	req, err := http.NewRequest(method, c.base+adminPrefix+path, rd)
	if err != nil {
		return err
	}
	req.Header.Set("Authorization", "Bearer "+c.token)
	req.Header.Set(hdrAdminClient, "cli")
	if op := cliOperator(); op != "" {
		req.Header.Set(hdrAdminOperator, op)
	}
	if body != nil {
		req.Header.Set("Content-Type", "application/json")
	}
	resp, err := c.hc.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	data, err := io.ReadAll(io.LimitReader(resp.Body, 16<<20))
	if err != nil {
		return err
	}
	if resp.StatusCode != http.StatusOK {
		var e struct {
			Error string `json:"error"`
		}
		if json.Unmarshal(data, &e) == nil && e.Error != "" {
			return fmt.Errorf("%s (HTTP %d)", e.Error, resp.StatusCode)
		}
		return fmt.Errorf("HTTP %d", resp.StatusCode)
	}
	if raw, ok := out.(*json.RawMessage); ok {
		*raw = data
		return nil
	}
	return json.Unmarshal(data, out)
}

// cliOperator names the person running the CLI for the audit log: the user who ran sudo, else
// the current user.
func cliOperator() string {
	for _, k := range []string{"SUDO_USER", "USER", "LOGNAME"} {
		if v := cleanText(os.Getenv(k), 64); v != "" {
			return v
		}
	}
	return ""
}

// parseWho turns a command-line account name into a Target.
func parseWho(s string) lobby.Target {
	switch {
	case strings.HasPrefix(s, "peer:"):
		return lobby.Target{PeerID: s[5:]}
	case strings.HasPrefix(s, "tag:"):
		return lobby.Target{Gamertag: s[4:]}
	}
	if b, err := hex.DecodeString(s); err == nil && len(b) == 8 {
		return lobby.Target{PeerID: s}
	}
	return lobby.Target{Gamertag: s}
}

func runAdmin(args []string, stdout io.Writer) error {
	fs := flag.NewFlagSet("ac6lobby admin", flag.ContinueOnError)
	fs.Usage = func() { fmt.Fprintln(fs.Output(), adminUsage) }
	addr := fs.String("addr", "127.0.0.1:47802", "admin interface address")
	tokenFile := fs.String("token-file", "", "admin token file")
	dataDir := fs.String("data", "", "server data directory holding admin.token")
	asJSON := fs.Bool("json", false, "print the raw JSON response")
	if err := fs.Parse(args); err != nil {
		return err
	}
	if fs.NArg() == 0 {
		fs.Usage()
		return flag.ErrHelp
	}
	path, err := findTokenFile(*tokenFile, *dataDir)
	if err != nil {
		return err
	}
	tok, err := os.ReadFile(path)
	if err != nil {
		return err
	}
	c := &adminClient{base: "http://" + *addr, token: strings.TrimSpace(string(tok)),
		hc: &http.Client{Timeout: 10 * time.Second}}
	cmd, rest := fs.Arg(0), fs.Args()[1:]

	var raw json.RawMessage
	call := func(method, p string, body any) error { return c.do(method, p, body, &raw) }
	needWho := func(n int) error {
		if len(rest) < n {
			return fmt.Errorf("%s: missing <who>", cmd)
		}
		return nil
	}
	switch cmd {
	case "players":
		sub := flag.NewFlagSet("players", flag.ContinueOnError)
		withAddr := sub.Bool("addr", false, "show registered IP addresses")
		if err := sub.Parse(rest); err != nil {
			return err
		}
		q := "players"
		if *withAddr {
			q += "?addr=1"
		}
		err = call(http.MethodGet, q, nil)
	case "sessions", "bans", "metrics":
		err = call(http.MethodGet, cmd, nil)
	case "audit":
		sub := flag.NewFlagSet("audit", flag.ContinueOnError)
		n := sub.Int("n", 50, "how many entries")
		if err := sub.Parse(rest); err != nil {
			return err
		}
		err = call(http.MethodGet, fmt.Sprintf("audit?limit=%d", max(*n, 1)), nil)
	case "kick":
		if err = needWho(1); err == nil {
			err = call(http.MethodPost, "kick", adminReqBody(parseWho(rest[0]), strings.Join(rest[1:], " "), ""))
		}
	case "ban":
		sub := flag.NewFlagSet("ban", flag.ContinueOnError)
		dur := sub.String("for", "", "ban duration, e.g. 30m, 24h, 7d (default permanent)")
		if err := sub.Parse(rest); err != nil {
			return err
		}
		rest = sub.Args()
		if err = needWho(1); err == nil {
			err = call(http.MethodPost, "ban", adminReqBody(parseWho(rest[0]), strings.Join(rest[1:], " "), *dur))
		}
	case "unban", "forget":
		if err = needWho(1); err == nil {
			err = call(http.MethodPost, cmd, parseWho(rest[0]))
		}
	case "broadcast":
		if len(rest) == 0 {
			return errors.New("broadcast: missing message")
		}
		err = call(http.MethodPost, "broadcast", map[string]string{"msg": strings.Join(rest, " ")})
	default:
		fs.Usage()
		return fmt.Errorf("unknown admin command %q", cmd)
	}
	if err != nil {
		return err
	}
	if *asJSON {
		var buf bytes.Buffer
		if json.Indent(&buf, raw, "", "  ") == nil {
			raw = buf.Bytes()
		}
		fmt.Fprintln(stdout, strings.TrimSpace(string(raw)))
		return nil
	}
	return printAdmin(stdout, cmd, raw)
}

func adminReqBody(t lobby.Target, reason, dur string) map[string]string {
	m := map[string]string{"reason": reason}
	if t.PeerID != "" {
		m["peer_id"] = t.PeerID
	} else {
		m["gamertag"] = t.Gamertag
	}
	if dur != "" {
		m["duration"] = dur
	}
	return m
}

func ago(sec int64) string {
	return (time.Duration(sec) * time.Second).String()
}

func printAdmin(w io.Writer, cmd string, raw json.RawMessage) error {
	tw := tabwriter.NewWriter(w, 0, 4, 2, ' ', 0)
	defer tw.Flush()
	switch cmd {
	case "players":
		var r struct{ Players []lobby.PlayerInfo }
		if err := json.Unmarshal(raw, &r); err != nil {
			return err
		}
		fmt.Fprintln(tw, "GAMERTAG\tPEER_ID\tVIP\tONLINE\tIDLE\tHOSTING\tPEERS\tRELAY_PKTS\tPRIVACY\tOLD_KEY\tCLIENT\tADDR")
		for _, p := range r.Players {
			fmt.Fprintf(tw, "%s\t%s\t%s\t%s\t%s\t%d\t%d\t%d\t%v\t%v\t%s\t%s\n", p.Gamertag, p.PeerID, p.VIP,
				ago(p.OnlineS), ago(p.IdleS), p.Hosting, p.Peers, p.RelayPkts, p.Privacy, p.OldKey, p.Client, dash(p.Addr))
		}
		fmt.Fprintf(tw, "%d online\n", len(r.Players))
	case "sessions":
		var r struct{ Sessions []lobby.SessionInfo }
		if err := json.Unmarshal(raw, &r); err != nil {
			return err
		}
		fmt.Fprintln(tw, "SESSION\tHOST\tMODE\tTYPE\tSTATE\tPLAYERS\tAGE\tMEMBERS")
		for _, s := range r.Sessions {
			mode := dash(s.ModeName)
			if s.ModeName == "" && s.Mode >= 0 {
				mode = fmt.Sprint(s.Mode)
			}
			host := s.Host
			if !s.HostOnline {
				host += " (offline)"
			}
			fmt.Fprintf(tw, "%s\t%s\t%s\t%s\t%s\t%d/%d\t%s\t%s\n", s.SessionID, host, mode, dash(s.Type), s.State,
				s.FilledPublic+s.FilledPrivate, s.PublicSlots+s.PrivateSlots, ago(s.AgeS), strings.Join(s.Members, ", "))
		}
		fmt.Fprintf(tw, "%d sessions\n", len(r.Sessions))
	case "bans":
		var r struct{ Bans []lobby.BanInfo }
		if err := json.Unmarshal(raw, &r); err != nil {
			return err
		}
		fmt.Fprintln(tw, "GAMERTAG\tPEER_ID\tSINCE\tEXPIRES\tREASON")
		for _, b := range r.Bans {
			exp := b.Expires
			if exp == "" {
				exp = "never"
			}
			fmt.Fprintf(tw, "%s\t%s\t%s\t%s\t%s\n", b.Gamertag, b.PeerID, b.Created, exp, b.Reason)
		}
		fmt.Fprintf(tw, "%d bans\n", len(r.Bans))
	case "kick":
		var r struct{ Kicked lobby.PlayerInfo }
		if err := json.Unmarshal(raw, &r); err != nil {
			return err
		}
		fmt.Fprintf(tw, "kicked %s (%s)\n", r.Kicked.Gamertag, r.Kicked.PeerID)
	case "ban":
		var r struct{ Banned lobby.BanInfo }
		if err := json.Unmarshal(raw, &r); err != nil {
			return err
		}
		exp := "permanently"
		if r.Banned.Expires != "" {
			exp = "until " + r.Banned.Expires
		}
		online := ""
		if r.Banned.WasOnline {
			online = "; disconnected"
		}
		fmt.Fprintf(tw, "banned %s (%s) %s%s\n", r.Banned.Gamertag, r.Banned.PeerID, exp, online)
	case "unban":
		var r struct{ Unbanned lobby.BanInfo }
		if err := json.Unmarshal(raw, &r); err != nil {
			return err
		}
		fmt.Fprintf(tw, "unbanned %s (%s)\n", r.Unbanned.Gamertag, r.Unbanned.PeerID)
	case "forget":
		var r struct{ Forgotten lobby.ForgetInfo }
		if err := json.Unmarshal(raw, &r); err != nil {
			return err
		}
		f := r.Forgotten
		fmt.Fprintf(tw, "deleted account %s (%s, xuid %s) and %d leaderboard rows\n", f.Gamertag, f.PeerID, f.XUID, f.StatsRows)
		if f.Banned {
			fmt.Fprintln(tw, "its ban is kept (the key stays banned); run unban first to forget the ban too")
		}
	case "audit":
		var r struct{ Audit []auditEntry }
		if err := json.Unmarshal(raw, &r); err != nil {
			return err
		}
		fmt.Fprintln(tw, "TIME\tWHO\tACTION\tTARGET\tRESULT\tDETAIL")
		for _, e := range r.Audit {
			who := e.Via
			if e.Operator != "" {
				who += ":" + e.Operator
			}
			target := strings.TrimSpace(e.Gamertag + " " + e.PeerID)
			result := "ok"
			if !e.OK {
				result = "failed: " + e.Error
			}
			var detail []string
			for _, d := range []string{e.Reason, e.Duration, e.Expires, e.Message, e.Detail} {
				if d != "" {
					detail = append(detail, d)
				}
			}
			fmt.Fprintf(tw, "%s\t%s\t%s\t%s\t%s\t%s\n", e.Time.Local().Format("2006-01-02 15:04:05"), who, e.Action,
				dash(target), result, strings.Join(detail, "; "))
		}
		fmt.Fprintf(tw, "%d entries\n", len(r.Audit))
	case "broadcast":
		var r struct{ Sent int }
		if err := json.Unmarshal(raw, &r); err != nil {
			return err
		}
		fmt.Fprintf(tw, "sent to %d clients\n", r.Sent)
	default: // metrics
		var buf bytes.Buffer
		if err := json.Indent(&buf, raw, "", "  "); err != nil {
			return err
		}
		fmt.Fprintln(tw, strings.TrimSpace(buf.String()))
	}
	return nil
}

func dash(s string) string {
	if s == "" {
		return "-"
	}
	return s
}
