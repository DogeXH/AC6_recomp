// Command loadtest drives an ac6lobby server with simulated AC6 clients: N clients register,
// hosts create sessions, the others search for and join them, every pair inside a session is
// introduced, and each direction of each pair pushes relay traffic at AC6-like rates. It
// reports registration and RPC latency, relay throughput, loss and latency, and (optionally)
// the server's CPU and memory.
//
// Example against a local server started with relaxed per-IP limits (all clients share one IP):
//
//	ac6lobby -data /tmp/lt -udp 127.0.0.1:47860 -admin 127.0.0.1:47862 \
//	    -hello-per-ip 1000 -hello-burst 1000 -accounts-per-ip-hour 100000
//	loadtest -server 127.0.0.1:47860 -key-file /tmp/lt/server.pub \
//	    -admin http://127.0.0.1:47862 -admin-token-file /tmp/lt/admin.token
//
// With -clients 2 -duration 3s it is a quick end-to-end check of a deployment (exit status 1
// on any failure or on loss above -max-loss).
package main

import (
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"math/rand/v2"
	"net"
	"net/http"
	"net/netip"
	"os"
	"os/exec"
	"slices"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"ac6lobby/internal/client"
	"ac6lobby/internal/wire"
)

const (
	title  = "4E4D07D1"
	compat = "a51a535851e632111a59c1dcd86a42ec" // AC6 base v8, protocol 1, 60 fps
)

type config struct {
	server, key, keyFile, local, identities, admin, adminToken, jsonOut, tagPrefix string
	clients, group, helloConc, pid                                                 int
	rateMin, rateMax, sizeMin, sizeMax                                             int
	duration, timeout                                                              time.Duration
	maxLoss                                                                        float64
}

func parseRange(s string, lo, hi *int) error {
	a, b, found := strings.Cut(s, "-")
	x, err := strconv.Atoi(strings.TrimSpace(a))
	if err != nil {
		return err
	}
	y := x
	if found {
		if y, err = strconv.Atoi(strings.TrimSpace(b)); err != nil {
			return err
		}
	}
	if x <= 0 || y < x {
		return fmt.Errorf("bad range %q", s)
	}
	*lo, *hi = x, y
	return nil
}

func main() {
	if err := run(); err != nil {
		fmt.Fprintln(os.Stderr, "loadtest:", err)
		os.Exit(1)
	}
}

func run() error {
	var cfg config
	var rate, size string
	flag.StringVar(&cfg.server, "server", "127.0.0.1:47800", "server host:port")
	flag.StringVar(&cfg.key, "key", "", "server public key (64 hex)")
	flag.StringVar(&cfg.keyFile, "key-file", "", "file holding the server public key (server.pub)")
	flag.StringVar(&cfg.local, "local", "", "local bind address (default 127.0.0.1:0 for a loopback server, else 0.0.0.0:0)")
	flag.StringVar(&cfg.identities, "identities", "", "JSON file of client keys to reuse accounts across runs (created if missing)")
	flag.IntVar(&cfg.clients, "clients", 64, "number of simulated clients")
	flag.IntVar(&cfg.group, "group", 2, "players per session; every pair in a session exchanges traffic (2 = pairs, 16 = full matches)")
	flag.StringVar(&rate, "rate", "20-60", "packets per second per direction of each pair (min-max, chosen per flow)")
	flag.StringVar(&size, "size", "100-300", "game payload bytes per packet (min-max, chosen per packet)")
	flag.DurationVar(&cfg.duration, "duration", 30*time.Second, "traffic phase length")
	flag.DurationVar(&cfg.timeout, "timeout", 30*time.Second, "timeout for registration and setup steps")
	flag.IntVar(&cfg.helloConc, "hello-concurrency", 32, "registrations in flight at once")
	flag.StringVar(&cfg.admin, "admin", "", "server admin URL (e.g. http://127.0.0.1:47802) to sample its CPU and memory")
	flag.StringVar(&cfg.adminToken, "admin-token-file", "", "admin token file for -admin")
	flag.IntVar(&cfg.pid, "pid", 0, "server process ID to sample with ps (alternative to -admin)")
	flag.StringVar(&cfg.jsonOut, "json", "", "also write the summary as JSON to this file")
	flag.StringVar(&cfg.tagPrefix, "tag", "LT", "gamertag prefix")
	flag.Float64Var(&cfg.maxLoss, "max-loss", 1.0, "exit with status 1 if relay loss exceeds this percentage")
	flag.Parse()
	if err := parseRange(rate, &cfg.rateMin, &cfg.rateMax); err != nil {
		return fmt.Errorf("-rate: %w", err)
	}
	if err := parseRange(size, &cfg.sizeMin, &cfg.sizeMax); err != nil {
		return fmt.Errorf("-size: %w", err)
	}
	if cfg.sizeMax > 1200 {
		return errors.New("-size: at most 1200 bytes (one datagram)")
	}
	if cfg.clients < 2 || cfg.group < 2 || cfg.group > 16 {
		return errors.New("need -clients >= 2 and 2 <= -group <= 16")
	}
	sum, err := loadtest(cfg)
	if sum != nil {
		sum.print(os.Stdout)
		if cfg.jsonOut != "" {
			b, _ := json.MarshalIndent(sum, "", "  ")
			if werr := os.WriteFile(cfg.jsonOut, append(b, '\n'), 0o644); werr != nil {
				fmt.Fprintln(os.Stderr, "loadtest: writing -json:", werr)
			}
		}
	}
	if err != nil {
		return err
	}
	if sum.Failures > 0 {
		return fmt.Errorf("%d setup failures", sum.Failures)
	}
	if sum.Traffic.LossPct > cfg.maxLoss {
		return fmt.Errorf("relay loss %.2f%% is above -max-loss %.2f%%", sum.Traffic.LossPct, cfg.maxLoss)
	}
	return nil
}

func serverKey(cfg config) (wire.Key, error) {
	var k wire.Key
	s := cfg.key
	if cfg.keyFile != "" {
		b, err := os.ReadFile(cfg.keyFile)
		if err != nil {
			return k, err
		}
		s = string(b)
	}
	b, err := hex.DecodeString(strings.TrimSpace(s))
	if err != nil || len(b) != wire.KeySize {
		return k, errors.New("need -key or -key-file with a 64-hex server public key")
	}
	copy(k[:], b)
	return k, nil
}

// loadIdentities returns n secret keys, reusing and extending the file when one is given.
func loadIdentities(path string, n int) ([]wire.Key, error) {
	var hexKeys []string
	if path != "" {
		if b, err := os.ReadFile(path); err == nil {
			if err := json.Unmarshal(b, &hexKeys); err != nil {
				return nil, fmt.Errorf("%s: %w", path, err)
			}
		} else if !errors.Is(err, os.ErrNotExist) {
			return nil, err
		}
	}
	keys := make([]wire.Key, 0, n)
	for _, h := range hexKeys {
		if len(keys) == n {
			break
		}
		b, err := hex.DecodeString(h)
		if err != nil || len(b) != wire.KeySize {
			return nil, fmt.Errorf("%s: bad key", path)
		}
		var k wire.Key
		copy(k[:], b)
		keys = append(keys, k)
	}
	grew := false
	for len(keys) < n {
		keys = append(keys, client.NewKey())
		grew = true
	}
	if path != "" && grew {
		out := make([]string, 0, max(n, len(hexKeys)))
		for _, k := range keys {
			out = append(out, hex.EncodeToString(k[:]))
		}
		out = append(out, hexKeys[min(len(hexKeys), n):]...)
		b, _ := json.Marshal(out)
		if err := os.WriteFile(path, b, 0o600); err != nil {
			return nil, err
		}
	}
	return keys, nil
}

// --- per-client state ---

type sim struct {
	idx  int
	c    *client.Client
	ack  client.HelloAck
	xuid string
	vip  uint32
	peer uint64

	group   *group
	peersMu sync.Mutex
	peers   map[uint64]bool

	recvPkts, recvBytes atomic.Uint64
	lat                 *latencies
	lastSeq             map[uint32]uint64 // flow id -> highest seq seen (receiver goroutine only)
	reordered           atomic.Uint64
}

type group struct {
	id      int
	members []*sim
	session string
}

type flow struct {
	id       uint32
	from, to *sim
	rate     int
	sent     atomic.Uint64
	sentB    atomic.Uint64
	wireB    atomic.Uint64
	sendErrs atomic.Uint64
}

// latencies keeps up to max samples (microseconds).
type latencies struct {
	mu  sync.Mutex
	v   []int64
	max int
}

func (l *latencies) add(us int64) {
	l.mu.Lock()
	if len(l.v) < l.max {
		l.v = append(l.v, us)
	}
	l.mu.Unlock()
}

type pct struct {
	N                  int     `json:"n"`
	P50, P90, P99, Max float64 `json:"-"`
	P50ms              float64 `json:"p50_ms"`
	P90ms              float64 `json:"p90_ms"`
	P99ms              float64 `json:"p99_ms"`
	MaxMs              float64 `json:"max_ms"`
}

func percentiles(us []int64) pct {
	if len(us) == 0 {
		return pct{}
	}
	slices.Sort(us)
	at := func(q float64) float64 { return float64(us[min(len(us)-1, int(q*float64(len(us))))]) / 1000 }
	p := pct{N: len(us), P50: at(0.50), P90: at(0.90), P99: at(0.99), Max: float64(us[len(us)-1]) / 1000}
	p.P50ms, p.P90ms, p.P99ms, p.MaxMs = r3(p.P50), r3(p.P90), r3(p.P99), r3(p.Max)
	return p
}

func r3(v float64) float64 { return float64(int64(v*1000+0.5)) / 1000 }
func r2(v float64) float64 { return float64(int64(v*100+0.5)) / 100 }

type rpcStats struct {
	mu sync.Mutex
	m  map[string][]int64
}

func (r *rpcStats) add(method string, d time.Duration) {
	r.mu.Lock()
	r.m[method] = append(r.m[method], d.Microseconds())
	r.mu.Unlock()
}

// --- summary ---

type summary struct {
	Server      string          `json:"server"`
	Clients     int             `json:"clients"`
	Registered  int             `json:"registered"`
	Groups      int             `json:"groups"`
	GroupSize   int             `json:"group_size"`
	Flows       int             `json:"flows"`
	Rate        string          `json:"rate_pps_per_flow"`
	Size        string          `json:"payload_bytes"`
	Failures    int             `json:"failures"`
	FailureMsgs []string        `json:"failure_msgs,omitempty"`
	Register    pct             `json:"register"`
	RegisterS   float64         `json:"register_total_s"`
	RPC         map[string]pct  `json:"rpc"`
	IntroMS     float64         `json:"intro_total_ms"`
	Traffic     trafficSummary  `json:"traffic"`
	ServerProc  *serverSample   `json:"server_process,omitempty"`
	ServerCtrs  *serverCounters `json:"server_counters,omitempty"`
	SelfCPUPct  float64         `json:"loadtest_cpu_pct"`
}

type trafficSummary struct {
	DurationS     float64 `json:"duration_s"`
	SentPkts      uint64  `json:"sent_pkts"`
	RecvPkts      uint64  `json:"recv_pkts"`
	LossPct       float64 `json:"loss_pct"`
	ClientDrops   uint64  `json:"client_queue_drops"`
	SendErrors    uint64  `json:"send_errors"`
	Reordered     uint64  `json:"reordered"`
	SentPPS       float64 `json:"sent_pps"`
	RecvPPS       float64 `json:"recv_pps"`
	PayloadMbit   float64 `json:"payload_mbit_s"`
	WireMbitIn    float64 `json:"server_in_mbit_s"`
	Latency       pct     `json:"latency"`
	PerFlowPPSAvg float64 `json:"per_flow_pps_avg"`
}

type serverSample struct {
	Source     string  `json:"source"`
	CPUPct     float64 `json:"cpu_pct_of_one_core"`
	RSSStartMB float64 `json:"rss_start_mb"`
	RSSEndMB   float64 `json:"rss_end_mb"`
	RSSPeakMB  float64 `json:"rss_peak_mb"`
	HeapEndMB  float64 `json:"heap_end_mb,omitempty"`
	Goroutines int     `json:"goroutines,omitempty"`
}

type serverCounters struct {
	RelayedPkts  uint64  `json:"relayed_pkts"` // forwarded by the server during the traffic phase
	LostBefore   int64   `json:"lost_before_server"`
	LostAfter    int64   `json:"lost_after_server"`
	RelayedPPS   float64 `json:"relayed_pps"`
	RelayedMbit  float64 `json:"relayed_mbit_s"`
	RelayDropped uint64  `json:"relay_dropped"`
	Malformed    uint64  `json:"malformed"`
}

func (s *summary) print(w *os.File) {
	fmt.Fprintf(w, "ac6lobby load test against %s\n", s.Server)
	fmt.Fprintf(w, "  clients %d (registered %d), %d sessions x %d players, %d relay flows at %s pps, payload %s B\n",
		s.Clients, s.Registered, s.Groups, s.GroupSize, s.Flows, s.Rate, s.Size)
	fmt.Fprintf(w, "  register: %.2f s total; per client p50 %.1f ms, p99 %.1f ms, max %.1f ms\n",
		s.RegisterS, s.Register.P50, s.Register.P99, s.Register.Max)
	methods := make([]string, 0, len(s.RPC))
	for m := range s.RPC {
		methods = append(methods, m)
	}
	slices.Sort(methods)
	for _, m := range methods {
		p := s.RPC[m]
		fmt.Fprintf(w, "  rpc %-17s n=%-4d p50 %.2f ms  p99 %.2f ms  max %.2f ms\n", m, p.N, p.P50, p.P99, p.Max)
	}
	fmt.Fprintf(w, "  introductions: %.1f ms until every pair had keys\n", s.IntroMS)
	t := s.Traffic
	fmt.Fprintf(w, "  traffic %.1f s: sent %d pkts (%.0f pps), received %d (%.0f pps), loss %.3f%% (client queue drops %d, send errors %d, reordered %d)\n",
		t.DurationS, t.SentPkts, t.SentPPS, t.RecvPkts, t.RecvPPS, t.LossPct, t.ClientDrops, t.SendErrors, t.Reordered)
	fmt.Fprintf(w, "  throughput: payload %.2f Mbit/s, relay datagrams into the server %.2f Mbit/s, %.1f pps per flow\n",
		t.PayloadMbit, t.WireMbitIn, t.PerFlowPPSAvg)
	fmt.Fprintf(w, "  relay latency (one way, send to receive): p50 %.2f ms, p90 %.2f ms, p99 %.2f ms, max %.2f ms (n=%d)\n",
		t.Latency.P50, t.Latency.P90, t.Latency.P99, t.Latency.Max, t.Latency.N)
	if p := s.ServerProc; p != nil {
		fmt.Fprintf(w, "  server (%s): CPU %.1f%% of one core; RSS start %.1f MB, end %.1f MB, peak %.1f MB",
			p.Source, p.CPUPct, p.RSSStartMB, p.RSSEndMB, p.RSSPeakMB)
		if p.HeapEndMB > 0 {
			fmt.Fprintf(w, "; Go heap %.1f MB, %d goroutines", p.HeapEndMB, p.Goroutines)
		}
		fmt.Fprintln(w)
	}
	if c := s.ServerCtrs; c != nil {
		fmt.Fprintf(w, "  server counters: relayed %d packets (%.0f pps, %.2f Mbit/s), relay dropped %d, malformed %d\n",
			c.RelayedPkts, c.RelayedPPS, c.RelayedMbit, c.RelayDropped, c.Malformed)
		fmt.Fprintf(w, "  where the losses were: %d before the server forwarded them (its socket or the path to it), %d after\n",
			c.LostBefore, c.LostAfter)
	}
	fmt.Fprintf(w, "  load generator CPU: %.1f%% of one core\n", s.SelfCPUPct)
	if s.Failures > 0 {
		fmt.Fprintf(w, "  FAILURES: %d\n", s.Failures)
		for i, m := range s.FailureMsgs {
			if i == 10 {
				fmt.Fprintf(w, "    ... %d more\n", len(s.FailureMsgs)-10)
				break
			}
			fmt.Fprintf(w, "    %s\n", m)
		}
	}
}

// --- the test ---

func loadtest(cfg config) (*summary, error) {
	spk, err := serverKey(cfg)
	if err != nil {
		return nil, err
	}
	raddr, err := net.ResolveUDPAddr("udp", cfg.server)
	if err != nil {
		return nil, err
	}
	server := raddr.AddrPort()
	server = netip.AddrPortFrom(server.Addr().Unmap(), server.Port())
	local := cfg.local
	if local == "" {
		local = "0.0.0.0:0"
		if server.Addr().IsLoopback() {
			local = "127.0.0.1:0"
		}
	}
	keys, err := loadIdentities(cfg.identities, cfg.clients)
	if err != nil {
		return nil, err
	}

	sum := &summary{Server: server.String(), Clients: cfg.clients, GroupSize: cfg.group,
		Rate: fmt.Sprintf("%d-%d", cfg.rateMin, cfg.rateMax), Size: fmt.Sprintf("%d-%d", cfg.sizeMin, cfg.sizeMax),
		RPC: map[string]pct{}}
	var failMu sync.Mutex
	fail := func(format string, a ...any) {
		failMu.Lock()
		sum.Failures++
		if len(sum.FailureMsgs) < 100 {
			sum.FailureMsgs = append(sum.FailureMsgs, fmt.Sprintf(format, a...))
		}
		failMu.Unlock()
	}
	selfStart := selfCPU()
	wallStart := time.Now()

	// 1. Register.
	sims := make([]*sim, cfg.clients)
	regLat := make([]int64, 0, cfg.clients)
	var regMu sync.Mutex
	sem := make(chan struct{}, max(1, cfg.helloConc))
	var wg sync.WaitGroup
	regStart := time.Now()
	for i := range sims {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			sem <- struct{}{}
			defer func() { <-sem }()
			c, err := client.NewWithOptions(server.String(), spk, keys[i], client.Options{Local: local, QueueLen: 4096})
			if err != nil {
				fail("client %d: %v", i, err)
				return
			}
			c.AutoRehello = true
			t0 := time.Now()
			port := c.LocalAddr().Port()
			ack, err := c.Hello(client.HelloInfo{Gamertag: fmt.Sprintf("%s%03d", cfg.tagPrefix, i), Title: title,
				Compat: compat, Local: [][]any{{"127.0.0.1", port}}}, cfg.timeout)
			if err != nil {
				c.Close()
				fail("hello %d: %v", i, err)
				return
			}
			d := time.Since(t0)
			s := &sim{idx: i, c: c, ack: ack, xuid: ack.XUID, peer: c.PeerID(), peers: map[uint64]bool{},
				lat: &latencies{max: 400000 / cfg.clients}, lastSeq: map[uint32]uint64{}}
			if ip := net.ParseIP(ack.VIP).To4(); ip != nil {
				s.vip = binary.BigEndian.Uint32(ip)
			}
			sims[i] = s
			regMu.Lock()
			regLat = append(regLat, d.Microseconds())
			regMu.Unlock()
		}(i)
	}
	wg.Wait()
	sum.RegisterS = r2(time.Since(regStart).Seconds())
	sum.Register = percentiles(regLat)
	live := make([]*sim, 0, len(sims))
	for _, s := range sims {
		if s != nil {
			live = append(live, s)
		}
	}
	sum.Registered = len(live)
	defer func() {
		for _, s := range live {
			s.c.Bye()
			s.c.Close()
		}
	}()
	if len(live) < 2 {
		return sum, errors.New("fewer than two clients registered")
	}

	// 2. Sessions: one per group; the first member hosts, the others search and join.
	rs := &rpcStats{m: map[string][]int64{}}
	timed := func(s *sim, method string, args, out any) error {
		t0 := time.Now()
		err := s.c.Call(method, args, out)
		rs.add(method, time.Since(t0))
		if err != nil {
			return fmt.Errorf("%s (client %d): %w", method, s.idx, err)
		}
		return nil
	}
	var groups []*group
	for i := 0; i+1 < len(live); i += cfg.group {
		g := &group{id: len(groups), members: live[i:min(i+cfg.group, len(live))]}
		for _, m := range g.members {
			m.group = g
		}
		groups = append(groups, g)
	}
	sum.Groups = len(groups)
	grouped := live[:0:0]
	for _, s := range live {
		if s.group != nil {
			grouped = append(grouped, s)
		}
	}
	for _, g := range groups {
		wg.Add(1)
		go func(g *group) {
			defer wg.Done()
			host := g.members[0]
			mode := uint32(g.id % 8)
			mission := int64(1000 + g.id) // unique per group, so each search finds its own host
			var cr struct {
				SessionID string `json:"session_id"`
			}
			err := timed(host, "session.create", map[string]any{"title": title, "compat": compat, "flags": 0x42F,
				"public_slots": 16, "private_slots": 0, "contexts": [][]uint32{{0x800A, 1}, {0x800B, mode}},
				"properties": []map[string]any{{"id": 0x1000000C, "t": 1, "v": mission}, {"id": 0x1000000D, "t": 1, "v": 0}}}, &cr)
			if err != nil {
				fail("%v", err)
				return
			}
			g.session = cr.SessionID
			if err := timed(host, "session.join", map[string]any{"session_id": g.session,
				"members": []map[string]any{{"xuid": host.xuid, "private": false}}}, nil); err != nil {
				fail("%v", err)
			}
			for _, m := range g.members[1:] {
				var sr struct {
					Results []struct {
						SessionID string `json:"session_id"`
					} `json:"results"`
				}
				err := timed(m, "session.search", map[string]any{"title": title, "compat": compat, "proc": 1,
					"max_results": 50, "num_users": 1, "contexts": [][]uint32{{0x800A, 1}, {0x800B, mode}},
					"properties": []map[string]any{{"id": 0x1000000C, "t": 1, "v": mission}}}, &sr)
				if err != nil {
					fail("%v", err)
					continue
				}
				if !slices.ContainsFunc(sr.Results, func(r struct {
					SessionID string `json:"session_id"`
				}) bool {
					return r.SessionID == g.session
				}) {
					fail("search (client %d): own group's session not found among %d results", m.idx, len(sr.Results))
					continue
				}
				if err := timed(m, "session.get", map[string]any{"session_id": g.session}, nil); err != nil {
					fail("%v", err)
				}
				if err := timed(m, "session.join", map[string]any{"session_id": g.session,
					"members": []map[string]any{{"xuid": m.xuid, "private": false}}}, nil); err != nil {
					fail("%v", err)
				}
			}
			if err := timed(host, "session.start", map[string]any{"session_id": g.session}, nil); err != nil {
				fail("%v", err)
			}
		}(g)
	}
	wg.Wait()

	// 3. Introductions: every member asks for each earlier member (by virtual IP), as joiners
	// do when they contact the host and the other players. Both sides then derive peer keys.
	introStart := time.Now()
	expect := func(s *sim) int { return len(s.group.members) - 1 }
	for _, s := range grouped {
		wg.Add(1)
		go func(s *sim) {
			defer wg.Done()
			deadline := time.Now().Add(cfg.timeout)
			for time.Now().Before(deadline) {
				s.peersMu.Lock()
				have := len(s.peers)
				s.peersMu.Unlock()
				if have >= expect(s) {
					break
				}
				in, err := s.c.WaitIntro(200 * time.Millisecond)
				if err != nil {
					continue
				}
				id, perr := strconv.ParseUint(in.Peer.PeerID, 16, 64)
				if perr != nil {
					continue
				}
				s.peersMu.Lock()
				if !s.peers[id] {
					if _, err := s.c.AddPeer(in.Peer); err == nil {
						s.peers[id] = true
					}
				}
				s.peersMu.Unlock()
			}
			s.c.Keepalive(nil, time.Second) // confirms the INTROs (stops the server's resends)
		}(s)
	}
	reqID := uint32(1)
	for _, s := range grouped {
		for _, m := range s.group.members {
			if m == s {
				break
			}
			s.c.IntroReq(reqID, wire.IntroByVIP, uint64(m.vip))
			reqID++
		}
	}
	wg.Wait()
	sum.IntroMS = r2(float64(time.Since(introStart).Microseconds()) / 1000)
	for _, s := range grouped {
		if len(s.peers) < expect(s) {
			fail("intro (client %d): %d of %d peers", s.idx, len(s.peers), expect(s))
		}
	}

	// 4. Traffic.
	var flows []*flow
	for _, s := range grouped {
		for _, m := range s.group.members {
			if m != s && s.peers[m.peer] && m.peers[s.peer] {
				flows = append(flows, &flow{id: uint32(len(flows)), from: s, to: m,
					rate: cfg.rateMin + rand.IntN(cfg.rateMax-cfg.rateMin+1)})
			}
		}
	}
	sum.Flows = len(flows)
	stop := make(chan struct{})
	var recvWG sync.WaitGroup
	for _, s := range live {
		recvWG.Add(1)
		go receiver(s, stop, &recvWG)
	}
	before := sampleServer(cfg)
	var peakRSS float64
	var sampleWG sync.WaitGroup
	sampleWG.Add(1)
	go func() { // RSS peak sampling
		defer sampleWG.Done()
		t := time.NewTicker(time.Second)
		defer t.Stop()
		for {
			select {
			case <-stop:
				return
			case <-t.C:
				if s := sampleServer(cfg); s != nil {
					peakRSS = max(peakRSS, s.rssMB)
				}
			}
		}
	}()
	trafficStart := time.Now()
	var sendWG sync.WaitGroup
	for _, f := range flows {
		sendWG.Add(1)
		go sender(f, cfg, stop, &sendWG)
	}
	for _, s := range live { // keepalives: relay traffic does not count as liveness
		sendWG.Add(1)
		go func(s *sim) {
			defer sendWG.Done()
			t := time.NewTicker(10 * time.Second)
			defer t.Stop()
			for {
				select {
				case <-stop:
					return
				case <-t.C:
					s.c.SendC2S([]byte{wire.KindKeepalive})
				}
			}
		}(s)
	}
	time.Sleep(cfg.duration)
	sendStop := time.Now()
	close(stop)
	sendWG.Wait()
	time.Sleep(300 * time.Millisecond) // let in-flight packets land before the receivers stop
	after := sampleServer(cfg)
	recvWG.Wait()
	sampleWG.Wait()
	elapsed := sendStop.Sub(trafficStart).Seconds()

	t := &sum.Traffic
	t.DurationS = r2(elapsed)
	var payload, wireIn uint64
	allLat := []int64{}
	for _, f := range flows {
		t.SentPkts += f.sent.Load()
		payload += f.sentB.Load()
		wireIn += f.wireB.Load()
		t.SendErrors += f.sendErrs.Load()
	}
	for _, s := range live {
		t.RecvPkts += s.recvPkts.Load()
		t.ClientDrops += s.c.RelayedDropped()
		t.Reordered += s.reordered.Load()
		allLat = append(allLat, s.lat.v...)
	}
	if t.SentPkts > 0 {
		t.LossPct = r3(100 * float64(int64(t.SentPkts)-int64(t.RecvPkts)) / float64(t.SentPkts))
	}
	t.SentPPS = r2(float64(t.SentPkts) / elapsed)
	t.RecvPPS = r2(float64(t.RecvPkts) / elapsed)
	t.PayloadMbit = r3(float64(payload) * 8 / elapsed / 1e6)
	t.WireMbitIn = r3(float64(wireIn) * 8 / elapsed / 1e6)
	if len(flows) > 0 {
		t.PerFlowPPSAvg = r2(t.SentPPS / float64(len(flows)))
	}
	t.Latency = percentiles(allLat)
	if before != nil && after != nil {
		dt := after.at.Sub(before.at).Seconds()
		p := &serverSample{Source: before.source, CPUPct: r2(100 * (after.cpu - before.cpu) / dt),
			RSSStartMB: r2(before.rssMB), RSSEndMB: r2(after.rssMB), RSSPeakMB: r2(max(peakRSS, after.rssMB, before.rssMB)),
			HeapEndMB: r2(after.heapMB), Goroutines: after.goroutines}
		sum.ServerProc = p
		if after.haveCounters {
			relayed := after.relayed - before.relayed
			sum.ServerCtrs = &serverCounters{
				RelayedPkts:  relayed,
				LostBefore:   int64(t.SentPkts) - int64(relayed),
				LostAfter:    int64(relayed) - int64(t.RecvPkts),
				RelayedPPS:   r2(float64(relayed) / dt),
				RelayedMbit:  r3(float64(after.relayedBytes-before.relayedBytes) * 8 / dt / 1e6),
				RelayDropped: after.relayDropped - before.relayDropped,
				Malformed:    after.malformed - before.malformed,
			}
		}
	}

	// 5. Tear down sessions (clients BYE in the deferred cleanup).
	for _, g := range groups {
		if g.session == "" {
			continue
		}
		host := g.members[0]
		if err := timed(host, "session.end", map[string]any{"session_id": g.session}, nil); err != nil {
			fail("%v", err)
		}
		if err := timed(host, "session.delete", map[string]any{"session_id": g.session}, nil); err != nil {
			fail("%v", err)
		}
	}
	for m, v := range rs.m {
		sum.RPC[m] = percentiles(v)
	}
	sum.SelfCPUPct = r2(100 * (selfCPU() - selfStart) / time.Since(wallStart).Seconds())
	return sum, nil
}

// Packet layout: P2P DATA, channel 1 (vnet datagram): proto, src_port, dst_port, msg_id,
// frag_idx, frag_cnt, then the payload: flow id u32, seq u64, send time (unix ns) i64, filler.
const dataHeader = 11
const payloadHeader = 20

func sender(f *flow, cfg config, stop chan struct{}, wg *sync.WaitGroup) {
	defer wg.Done()
	interval := time.Second / time.Duration(f.rate)
	// Spread the flows' phases so they do not all fire together.
	select {
	case <-stop:
		return
	case <-time.After(time.Duration(rand.Int64N(int64(interval)))):
	}
	t := time.NewTicker(interval)
	defer t.Stop()
	buf := make([]byte, dataHeader+cfg.sizeMax)
	for i := dataHeader + payloadHeader; i < len(buf); i++ {
		buf[i] = byte(i * 7)
	}
	var seq uint64
	for {
		select {
		case <-stop:
			return
		case <-t.C:
		}
		seq++
		size := max(payloadHeader, cfg.sizeMin+rand.IntN(cfg.sizeMax-cfg.sizeMin+1))
		pkt := buf[:dataHeader+size]
		pkt[0], pkt[1], pkt[2] = wire.P2PData, 0x01, 17
		binary.BigEndian.PutUint16(pkt[3:], 20000)
		binary.BigEndian.PutUint16(pkt[5:], 20000)
		binary.LittleEndian.PutUint16(pkt[7:], uint16(seq))
		pkt[9], pkt[10] = 0, 1
		p := pkt[dataHeader:]
		binary.LittleEndian.PutUint32(p[0:], f.id)
		binary.LittleEndian.PutUint64(p[4:], seq)
		binary.LittleEndian.PutUint64(p[12:], uint64(time.Now().UnixNano()))
		relay, err := f.from.c.BuildRelay(f.to.peer, pkt)
		if err == nil {
			err = f.from.c.SendRaw(relay)
		}
		if err != nil {
			f.sendErrs.Add(1)
			continue
		}
		f.sent.Add(1)
		f.sentB.Add(uint64(size))
		f.wireB.Add(uint64(len(relay)))
	}
}

func receiver(s *sim, stop chan struct{}, wg *sync.WaitGroup) {
	defer wg.Done()
	ch := s.c.RelayedChan()
	drain := false
	for {
		var r client.Relayed
		if drain {
			select {
			case r = <-ch:
			case <-time.After(250 * time.Millisecond):
				return
			}
		} else {
			select {
			case r = <-ch:
			case <-stop:
				drain = true
				continue
			}
		}
		in := r.Inner
		if len(in) < dataHeader+payloadHeader || in[0] != wire.P2PData || in[1] != 0x01 {
			continue
		}
		p := in[dataHeader:]
		fid := binary.LittleEndian.Uint32(p[0:])
		seq := binary.LittleEndian.Uint64(p[4:])
		sent := int64(binary.LittleEndian.Uint64(p[12:]))
		s.recvPkts.Add(1)
		s.recvBytes.Add(uint64(len(p)))
		if last := s.lastSeq[fid]; seq < last {
			s.reordered.Add(1)
		} else {
			s.lastSeq[fid] = seq
		}
		s.lat.add((time.Now().UnixNano() - sent) / 1000)
	}
}

// --- server sampling ---

type sample struct {
	source                                         string
	at                                             time.Time
	cpu, rssMB, heapMB                             float64
	goroutines                                     int
	haveCounters                                   bool
	relayed, relayedBytes, relayDropped, malformed uint64
}

func sampleServer(cfg config) *sample {
	switch {
	case cfg.admin != "":
		return sampleAdmin(cfg)
	case cfg.pid > 0:
		return samplePS(cfg.pid)
	}
	return nil
}

func sampleAdmin(cfg config) *sample {
	tok, err := os.ReadFile(cfg.adminToken)
	if err != nil {
		return nil
	}
	req, _ := http.NewRequest(http.MethodGet, strings.TrimRight(cfg.admin, "/")+"/admin/v1/metrics", nil)
	req.Header.Set("Authorization", "Bearer "+strings.TrimSpace(string(tok)))
	resp, err := (&http.Client{Timeout: 3 * time.Second}).Do(req)
	if err != nil {
		return nil
	}
	defer resp.Body.Close()
	var m struct {
		Server struct {
			Relayed      uint64 `json:"relayed"`
			RelayedBytes uint64 `json:"relayed_bytes"`
			RelayDropped uint64 `json:"relay_dropped"`
			Malformed    uint64 `json:"malformed"`
		} `json:"server"`
		Process struct {
			CPUSeconds float64 `json:"cpu_seconds"`
			RSSBytes   uint64  `json:"rss_bytes"`
			HeapBytes  uint64  `json:"heap_bytes"`
			Goroutines int     `json:"goroutines"`
		} `json:"process"`
	}
	if resp.StatusCode != http.StatusOK || json.NewDecoder(resp.Body).Decode(&m) != nil {
		return nil
	}
	s := &sample{source: "admin metrics", at: time.Now(), cpu: m.Process.CPUSeconds,
		rssMB: float64(m.Process.RSSBytes) / (1 << 20), heapMB: float64(m.Process.HeapBytes) / (1 << 20),
		goroutines: m.Process.Goroutines, haveCounters: true, relayed: m.Server.Relayed,
		relayedBytes: m.Server.RelayedBytes, relayDropped: m.Server.RelayDropped, malformed: m.Server.Malformed}
	if pid := cfg.pid; pid > 0 { // ps gives the current RSS on macOS, where rusage has only the peak
		if p := samplePS(pid); p != nil {
			s.rssMB = p.rssMB
		}
	}
	return s
}

// samplePS reads cumulative CPU time and RSS with ps (macOS and Linux).
func samplePS(pid int) *sample {
	out, err := exec.Command("ps", "-o", "time=,rss=", "-p", strconv.Itoa(pid)).Output()
	if err != nil {
		return nil
	}
	f := strings.Fields(string(out))
	if len(f) != 2 {
		return nil
	}
	cpu, ok := parsePSTime(f[0])
	rss, err := strconv.ParseFloat(f[1], 64)
	if !ok || err != nil {
		return nil
	}
	return &sample{source: "ps", at: time.Now(), cpu: cpu, rssMB: rss / 1024}
}

// parsePSTime parses [[dd-]hh:]mm:ss[.ff].
func parsePSTime(s string) (float64, bool) {
	days := 0.0
	if d, rest, ok := strings.Cut(s, "-"); ok {
		v, err := strconv.ParseFloat(d, 64)
		if err != nil {
			return 0, false
		}
		days, s = v, rest
	}
	parts := strings.Split(s, ":")
	total := 0.0
	for _, p := range parts {
		v, err := strconv.ParseFloat(p, 64)
		if err != nil {
			return 0, false
		}
		total = total*60 + v
	}
	return days*86400 + total, true
}
