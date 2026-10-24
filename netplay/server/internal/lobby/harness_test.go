package lobby

import (
	"encoding/json"
	"fmt"
	"io"
	"log/slog"
	"net"
	"net/netip"
	"os"
	"sync"
	"testing"
	"time"

	"ac6lobby/internal/client"
	"ac6lobby/internal/wire"
)

// Test ports: 47820-47829 by default, or the range in AC6LOBBY_TEST_PORTS ("first-last", for
// example "47860-47869"). This package uses all but the last two ports of the range (the last
// two belong to the cmd/loadtest and main package tests, which go test may run in parallel).
// Tests in this package run sequentially; each test server takes the next port.
var (
	portMu              sync.Mutex
	portFirst, portLast = testPortRange()
	nextPort            = portFirst
)

func testPortRange() (int, int) {
	first, last := 47820, 47829
	if v := os.Getenv("AC6LOBBY_TEST_PORTS"); v != "" {
		var a, b int
		if n, _ := fmt.Sscanf(v, "%d-%d", &a, &b); n == 2 && a > 0 && b >= a && b < 65536 {
			first, last = a, b
		}
	}
	if last-first >= 4 {
		last -= 2
	}
	return first, last
}

func testPort() int {
	portMu.Lock()
	defer portMu.Unlock()
	p := nextPort
	nextPort++
	if nextPort > portLast {
		nextPort = portFirst
	}
	return p
}

type fakeClock struct {
	mu sync.Mutex
	t  time.Time
}

func newFakeClock() *fakeClock { return &fakeClock{t: time.Unix(1_760_000_000, 0)} }

func (f *fakeClock) Now() time.Time {
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.t
}

func (f *fakeClock) Advance(d time.Duration) {
	f.mu.Lock()
	f.t = f.t.Add(d)
	f.mu.Unlock()
}

type testServer struct {
	*Server
	t     *testing.T
	addr  string
	conn  *net.UDPConn
	clock *fakeClock
	done  chan struct{}
	data  string
	sk    wire.Key
	keys  map[*client.Client]wire.Key // secret keys recorded by tests that restart a client
}

type serverOpt func(*Config)

func withClock(c *fakeClock) serverOpt { return func(cfg *Config) { cfg.Now = c.Now } }
func withData(dir string) serverOpt    { return func(cfg *Config) { cfg.DataDir = dir } }
func withKey(k wire.Key) serverOpt     { return func(cfg *Config) { cfg.SecretKey = k } }
func withLimits(l Limits) serverOpt    { return func(cfg *Config) { cfg.Limits = l } }

func testLogger() *slog.Logger {
	if os.Getenv("AC6LOBBY_TEST_LOG") != "" {
		return slog.New(slog.NewTextHandler(os.Stderr, &slog.HandlerOptions{Level: slog.LevelDebug}))
	}
	return slog.New(slog.NewTextHandler(io.Discard, nil))
}

// startServer runs a server on the next lane port. It stops when the test ends.
func startServer(t *testing.T, opts ...serverOpt) *testServer {
	t.Helper()
	cfg := Config{SecretKey: client.NewKey(), Logger: testLogger(),
		Limits: Limits{HelloPerSecond: 1000, HelloBurst: 1000, AccountsPerIPPerHour: 1000}}
	for _, o := range opts {
		o(&cfg)
	}
	srv, err := New(cfg)
	if err != nil {
		t.Fatal(err)
	}
	port := testPort()
	conn, err := net.ListenUDP("udp", net.UDPAddrFromAddrPort(netip.AddrPortFrom(netip.MustParseAddr("127.0.0.1"), uint16(port))))
	if err != nil {
		t.Fatalf("listen on lane port %d: %v", port, err)
	}
	ts := &testServer{Server: srv, t: t, addr: fmt.Sprintf("127.0.0.1:%d", port), conn: conn,
		done: make(chan struct{}), data: cfg.DataDir, sk: cfg.SecretKey, keys: map[*client.Client]wire.Key{}}
	go func() {
		srv.Serve(conn)
		close(ts.done)
	}()
	t.Cleanup(ts.stop)
	return ts
}

func (ts *testServer) stop() {
	select {
	case <-ts.done:
		return
	default:
	}
	ts.Close()
	ts.conn.Close()
	<-ts.done
}

// sweepNow runs the liveness sweep and intro resends at the server's current clock.
func (ts *testServer) sweepNow() {
	ts.mu.Lock()
	now := ts.now()
	ts.resendIntros(now)
	ts.sweep(now)
	ts.mu.Unlock()
}

func (ts *testServer) locked(f func()) {
	ts.mu.Lock()
	defer ts.mu.Unlock()
	f()
}

const testCompat = "a51a535851e632111a59c1dcd86a42ec"

// newClient connects and registers a test client.
func (ts *testServer) newClient(gamertag string) *client.Client {
	ts.t.Helper()
	return ts.newClientKey(gamertag, client.NewKey())
}

func (ts *testServer) newClientKey(gamertag string, sk wire.Key) *client.Client {
	ts.t.Helper()
	c, err := client.New(ts.addr, ts.PublicKey(), sk)
	if err != nil {
		ts.t.Fatal(err)
	}
	ts.t.Cleanup(c.Close)
	if _, err := c.Hello(client.HelloInfo{Gamertag: gamertag, Title: "4E4D07D1", Compat: testCompat,
		Local: [][]any{{"192.168.1.20", c.LocalAddr().Port()}, {"127.0.0.1", c.LocalAddr().Port()}}}, 3*time.Second); err != nil {
		ts.t.Fatalf("hello %s: %v", gamertag, err)
	}
	return c
}

// call is Call with a test failure on error.
func call(t *testing.T, c *client.Client, method string, args, out any) {
	t.Helper()
	if err := c.Call(method, args, out); err != nil {
		t.Fatalf("%s: %v", method, err)
	}
}

// callErr expects an RPC error with the given code.
func callErr(t *testing.T, c *client.Client, method string, args any, code string) {
	t.Helper()
	err := c.Call(method, args, nil)
	re, ok := err.(*client.RPCError)
	if !ok || re.Code != code {
		t.Fatalf("%s: want error %q, got %v", method, code, err)
	}
}

// waitFor polls cond until it holds or 3 s pass.
func waitFor(t *testing.T, what string, cond func() bool) {
	t.Helper()
	deadline := time.Now().Add(3 * time.Second)
	for time.Now().Before(deadline) {
		if cond() {
			return
		}
		time.Sleep(5 * time.Millisecond)
	}
	t.Fatalf("timed out waiting for %s", what)
}

type createResult struct {
	SessionID string `json:"session_id"`
	Key       string `json:"key"`
	Nonce     string `json:"nonce"`
}

func createSession(t *testing.T, c *client.Client, flags uint32, pub, priv uint32, ctxs [][]uint32) createResult {
	t.Helper()
	var r createResult
	if ctxs == nil {
		ctxs = [][]uint32{}
	}
	call(t, c, "session.create", map[string]any{"title": "4E4D07D1", "compat": testCompat, "flags": flags,
		"public_slots": pub, "private_slots": priv, "contexts": ctxs, "properties": []any{}}, &r)
	return r
}

type docResult struct {
	SessionID     string            `json:"session_id"`
	Key           string            `json:"key"`
	Nonce         string            `json:"nonce"`
	Title         string            `json:"title"`
	Flags         uint32            `json:"flags"`
	State         string            `json:"state"`
	PublicSlots   uint32            `json:"public_slots"`
	PrivateSlots  uint32            `json:"private_slots"`
	FilledPublic  uint32            `json:"filled_public"`
	FilledPrivate uint32            `json:"filled_private"`
	Host          map[string]string `json:"host"`
	Contexts      [][]uint32        `json:"contexts"`
	Properties    []json.RawMessage `json:"properties"`
	Members       []struct {
		XUID    string `json:"xuid"`
		Private bool   `json:"private"`
	} `json:"members"`
}
