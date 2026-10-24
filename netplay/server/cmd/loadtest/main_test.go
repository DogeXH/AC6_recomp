package main

import (
	"encoding/hex"
	"fmt"
	"io"
	"log/slog"
	"net"
	"net/netip"
	"os"
	"testing"
	"time"

	"ac6lobby/internal/client"
	"ac6lobby/internal/lobby"
)

// testPort is the second-to-last port of AC6LOBBY_TEST_PORTS (default 47828).
func testPort() int {
	last := 47829
	if v := os.Getenv("AC6LOBBY_TEST_PORTS"); v != "" {
		var a, b int
		if n, _ := fmt.Sscanf(v, "%d-%d", &a, &b); n == 2 && b > a {
			last = b
		}
	}
	return last - 1
}

func TestLoadtestSmoke(t *testing.T) {
	log := slog.New(slog.NewTextHandler(io.Discard, nil))
	srv, err := lobby.New(lobby.Config{SecretKey: client.NewKey(), Logger: log,
		Limits: lobby.Limits{HelloPerSecond: 1000, HelloBurst: 1000, AccountsPerIPPerHour: 1000}})
	if err != nil {
		t.Fatal(err)
	}
	addr := fmt.Sprintf("127.0.0.1:%d", testPort())
	conn, err := net.ListenUDP("udp", net.UDPAddrFromAddrPort(netip.MustParseAddrPort(addr)))
	if err != nil {
		t.Fatal(err)
	}
	done := make(chan struct{})
	go func() { srv.Serve(conn); close(done) }()
	defer func() { srv.Close(); conn.Close(); <-done }()

	pk := srv.PublicKey()
	ids := t.TempDir() + "/ids.json"
	cfg := config{server: addr, key: hex.EncodeToString(pk[:]), identities: ids, clients: 6, group: 3,
		rateMin: 20, rateMax: 40, sizeMin: 100, sizeMax: 300, duration: 1500 * time.Millisecond,
		timeout: 10 * time.Second, helloConc: 8, tagPrefix: "T"}
	sum, err := loadtest(cfg)
	if err != nil {
		t.Fatal(err)
	}
	if sum.Failures != 0 || sum.Registered != 6 || sum.Groups != 2 || sum.Flows != 12 {
		t.Fatalf("summary %+v", sum)
	}
	if sum.Traffic.SentPkts < 12*20 || sum.Traffic.LossPct > 5 {
		t.Fatalf("traffic %+v", sum.Traffic)
	}
	if srv.Status().Accounts != 6 {
		t.Fatalf("accounts %d", srv.Status().Accounts)
	}
	// A second run reuses the identities: no new accounts.
	cfg.duration = 300 * time.Millisecond
	if _, err := loadtest(cfg); err != nil {
		t.Fatal(err)
	}
	if srv.Status().Accounts != 6 {
		t.Fatalf("identities not reused: %d accounts", srv.Status().Accounts)
	}
	if st := srv.Status(); st.Sessions != 0 {
		t.Fatalf("sessions left behind: %+v", st)
	}
}

func TestParsePSTime(t *testing.T) {
	for in, want := range map[string]float64{"0:01.50": 1.5, "01:02:03": 3723, "1-00:00:01": 86401, "12.25": 12.25} {
		if got, ok := parsePSTime(in); !ok || got != want {
			t.Errorf("%s: %v %v", in, got, ok)
		}
	}
}
