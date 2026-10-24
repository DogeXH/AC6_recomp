// Command ac6lobby is the lobby, rendezvous and relay server for the AC6 recomp's online play.
// See README.md, netplay/docs/PROTOCOL.md and netplay/docs/HOSTING_A_SERVER.md.
//
// Usage:
//
//	ac6lobby [flags]                 run the server (see -h)
//	ac6lobby admin <command> ...     talk to a running server's admin interface
//	ac6lobby key show|rotate         print or rotate the server key
//	ac6lobby healthcheck             check that a server answers on UDP
//	ac6lobby version
package main

import (
	"context"
	"errors"
	"flag"
	"fmt"
	"io"
	"log/slog"
	"net"
	"net/http"
	"os"
	"os/signal"
	"path/filepath"
	"syscall"
	"time"

	"ac6lobby/internal/lobby"
)

// version is set at build time with -ldflags "-X main.version=...".
var version = "dev"

// udpBuffer is the requested UDP socket buffer size; the kernel may cap it.
const udpBuffer = 4 << 20

func main() {
	var err error
	args := os.Args[1:]
	cmd := ""
	if len(args) > 0 {
		cmd = args[0]
	}
	switch cmd {
	case "admin":
		err = runAdmin(args[1:], os.Stdout)
	case "key":
		err = runKey(args[1:], os.Stdout)
	case "healthcheck":
		err = runHealthcheck(args[1:], os.Stdout)
	case "version":
		fmt.Println("ac6lobby", version)
	case "serve":
		err = run(args[1:])
	default:
		err = run(args)
	}
	if err != nil && !errors.Is(err, flag.ErrHelp) {
		fmt.Fprintln(os.Stderr, "ac6lobby:", err)
		os.Exit(1)
	}
}

type serveFlags struct {
	udp, http, admin, adminToken, data, logLevel, logFormat, motd string
	statsEvery                                                    time.Duration
	logIPs                                                        bool
	helloPerIP, helloBurst, accountsPerIPHour                     float64
	auditDays                                                     int
}

func parseServeFlags(args []string) (*serveFlags, error) {
	f := &serveFlags{}
	fs := flag.NewFlagSet("ac6lobby", flag.ContinueOnError)
	fs.StringVar(&f.udp, "udp", ":47800", "UDP listen address for the protocol")
	fs.StringVar(&f.http, "http", "127.0.0.1:47801", "public status page address: /status, /status/sessions and / (empty = off)")
	fs.StringVar(&f.admin, "admin", "", "admin interface address, loopback only, e.g. 127.0.0.1:47802 (empty = off)")
	fs.StringVar(&f.adminToken, "admin-token-file", "", "admin bearer token file (default <data>/admin.token, created if missing)")
	fs.StringVar(&f.data, "data", "data", "directory for server.key, server.pub, accounts.json, stats.json, bans.json and audit.jsonl")
	fs.StringVar(&f.logLevel, "log-level", "info", "debug, info, warn or error")
	fs.StringVar(&f.logFormat, "log-format", "text", "text or json")
	fs.DurationVar(&f.statsEvery, "stats-interval", time.Minute, "how often to log counts and request rates (0 = never)")
	fs.StringVar(&f.motd, "motd", "", "message of the day sent in HELLO_ACK")
	fs.BoolVar(&f.logIPs, "log-ips", false, "include client IP addresses in rate-limit warnings (debug logs always have them)")
	fs.Float64Var(&f.helloPerIP, "hello-per-ip", 10, "HELLOs per second allowed from one source IP")
	fs.Float64Var(&f.helloBurst, "hello-burst", 20, "HELLO burst allowed from one source IP")
	fs.Float64Var(&f.accountsPerIPHour, "accounts-per-ip-hour", 20, "new accounts per hour allowed from one source IP")
	fs.IntVar(&f.auditDays, "audit-days", 90, "keep admin audit log entries (<data>/audit.jsonl) this many days (0 = no age limit; the newest 5000 are kept)")
	if err := fs.Parse(args); err != nil {
		return nil, err
	}
	if fs.NArg() > 0 {
		return nil, fmt.Errorf("unexpected argument %q (subcommands: admin, key, healthcheck, version)", fs.Arg(0))
	}
	if f.helloPerIP <= 0 || f.helloBurst < 1 || f.accountsPerIPHour <= 0 {
		return nil, errors.New("rate limits must be positive")
	}
	if f.auditDays < 0 {
		return nil, errors.New("-audit-days must be 0 or more")
	}
	return f, nil
}

func newLogger(w io.Writer, level, format string) (*slog.Logger, error) {
	var lv slog.Level
	if err := lv.UnmarshalText([]byte(level)); err != nil {
		return nil, fmt.Errorf("bad -log-level %q", level)
	}
	opts := &slog.HandlerOptions{Level: lv}
	switch format {
	case "text":
		return slog.New(slog.NewTextHandler(w, opts)), nil
	case "json":
		return slog.New(slog.NewJSONHandler(w, opts)), nil
	}
	return nil, fmt.Errorf("bad -log-format %q (text or json)", format)
}

func run(args []string) error {
	f, err := parseServeFlags(args)
	if err != nil {
		return err
	}
	log, err := newLogger(os.Stderr, f.logLevel, f.logFormat)
	if err != nil {
		return err
	}

	if err := os.MkdirAll(f.data, 0o700); err != nil {
		return err
	}
	sk, err := lobby.LoadOrCreateKey(f.data, log)
	if err != nil {
		return err
	}
	prev, err := lobby.LoadPrevKeys(f.data)
	if err != nil {
		return err
	}
	srv, err := lobby.New(lobby.Config{SecretKey: sk, PrevSecretKeys: prev, DataDir: f.data, Logger: log, MOTD: f.motd, LogAddresses: f.logIPs,
		Limits: lobby.Limits{HelloPerSecond: f.helloPerIP, HelloBurst: f.helloBurst,
			AccountsPerIPPerHour: f.accountsPerIPHour}})
	if err != nil {
		return err
	}
	pk := srv.PublicKey()
	fmt.Printf("server public key: %x\n", pk[:])

	laddr, err := net.ResolveUDPAddr("udp", f.udp)
	if err != nil {
		srv.Close()
		return err
	}
	conn, err := net.ListenUDP("udp", laddr)
	if err != nil {
		srv.Close()
		return err
	}
	// Larger socket buffers absorb relay bursts; the OS may cap these (best effort).
	conn.SetReadBuffer(udpBuffer)
	conn.SetWriteBuffer(udpBuffer)
	log.Info("listening", "udp", conn.LocalAddr().String(), "pubkey", fmt.Sprintf("%x", pk[:]), "version", version)
	for _, p := range srv.PrevPublicKeys() {
		log.Info("also accepting the previous server key (rotation grace); remove "+lobby.PrevKeyFile+" to stop",
			"prev_pubkey", fmt.Sprintf("%x", p[:]))
	}
	if rcv, snd := sockBufs(conn); rcv > 0 {
		log.Info("udp socket buffers", "rcvbuf_kib", rcv/1024, "sndbuf_kib", snd/1024)
		if rcv < 1<<20 {
			log.Warn("the UDP receive buffer is small; relay bursts may be dropped by the kernel "+
				"(Linux: raise net.core.rmem_max and net.core.wmem_max to at least 4194304)", "rcvbuf_kib", rcv/1024)
		}
	}

	var httpServers []*http.Server
	fail := func(err error) error {
		for _, hs := range httpServers {
			hs.Close()
		}
		srv.Close()
		conn.Close()
		return err
	}
	serveHTTP := func(name string, hs *http.Server, ln net.Listener) {
		httpServers = append(httpServers, hs)
		go func() {
			if err := hs.Serve(ln); err != nil && !errors.Is(err, http.ErrServerClosed) {
				log.Error(name, "err", err)
			}
		}()
	}
	if f.http != "" {
		ln, err := net.Listen("tcp", f.http)
		if err != nil {
			return fail(err)
		}
		log.Info("status page", "http", "http://"+ln.Addr().String()+"/")
		serveHTTP("status http", statusServer(f.http, srv), ln)
	}
	if f.admin != "" {
		addr, err := loopbackAddr(f.admin)
		if err != nil {
			return fail(err)
		}
		tokenPath := f.adminToken
		if tokenPath == "" {
			tokenPath = filepath.Join(f.data, "admin.token")
		}
		token, err := loadOrCreateToken(tokenPath, log)
		if err != nil {
			return fail(err)
		}
		audit, err := openAudit(filepath.Join(f.data, auditFileName), time.Duration(f.auditDays)*24*time.Hour, log)
		if err != nil {
			return fail(err)
		}
		ln, err := net.Listen("tcp", addr)
		if err != nil {
			return fail(err)
		}
		log.Info("admin interface", "http", "http://"+ln.Addr().String()+"/admin/v1/",
			"panel", "http://"+ln.Addr().String()+adminUIPath, "token_file", tokenPath, "audit_days", f.auditDays)
		serveHTTP("admin http", adminServer(srv, token, audit, log), ln)
	}

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	serveErr := make(chan error, 1)
	go func() { serveErr <- srv.Serve(conn) }()
	statsDone := make(chan struct{})
	go logStats(ctx, srv, log, f.statsEvery, statsDone)

	select {
	case <-ctx.Done():
		st := srv.Status()
		log.Info("shutting down", "online", st.Online, "sessions", st.Sessions)
	case err := <-serveErr:
		log.Error("serve", "err", err)
	}
	stop() // a second signal now kills the process the default way
	<-statsDone
	start := time.Now()
	srv.Close() // sends the shutdown NOTICE and writes pending state
	conn.Close()
	for _, hs := range httpServers {
		sctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
		hs.Shutdown(sctx)
		cancel()
	}
	log.Info("shutdown complete", "notified", srv.Notified(), "took_ms", time.Since(start).Milliseconds())
	return nil
}

// logStats logs counts and per-second rates every interval until ctx ends.
func logStats(ctx context.Context, srv *lobby.Server, log *slog.Logger, every time.Duration, done chan struct{}) {
	defer close(done)
	if every <= 0 {
		<-ctx.Done()
		return
	}
	t := time.NewTicker(every)
	defer t.Stop()
	prev, prevAt := srv.Metrics(), time.Now()
	for {
		select {
		case <-ctx.Done():
			return
		case now := <-t.C:
			m := srv.Metrics()
			dt := now.Sub(prevAt).Seconds()
			rate := func(a, b uint64) float64 {
				if dt <= 0 {
					return 0
				}
				return float64(int64(a-b)) / dt
			}
			ps := readProcStats()
			log.Info("stats",
				"online", m.Online, "sessions", m.Sessions, "accounts", m.Accounts, "bans", m.Bans,
				"hello_per_s", round2(rate(m.Hellos, prev.Hellos)),
				"rpc_per_s", round2(rate(m.RPCs, prev.RPCs)),
				"intro_per_s", round2(rate(m.Intros, prev.Intros)),
				"relay_pkt_per_s", round2(rate(m.Relayed, prev.Relayed)),
				"relay_kbit_per_s", round2(rate(m.RelayedBytes, prev.RelayedBytes)*8/1000),
				"relay_drop_per_s", round2(rate(m.RelayDropped, prev.RelayDropped)),
				"malformed_per_s", round2(rate(m.Malformed, prev.Malformed)),
				"rehello_per_s", round2(rate(m.Rehellos, prev.Rehellos)),
				"banned_refused", m.BannedRefused-prev.BannedRefused,
				"cpu_s", round2(ps.CPUSeconds), "rss_mb", round2(float64(ps.RSSBytes)/(1<<20)),
				"heap_mb", round2(float64(ps.HeapBytes)/(1<<20)), "goroutines", ps.Goroutines)
			prev, prevAt = m, now
		}
	}
}

func round2(v float64) float64 { return float64(int64(v*100+0.5)) / 100 }
