package main

import (
	"crypto/rand"
	"encoding/binary"
	"encoding/hex"
	"errors"
	"flag"
	"fmt"
	"io"
	"log/slog"
	"net"
	"os"
	"path/filepath"
	"strings"
	"time"

	"ac6lobby/internal/lobby"
	"ac6lobby/internal/wire"
)

const keyUsage = `usage: ac6lobby key [-data DIR] show | rotate [-no-grace] | retire

  show     print the server public key (the value clients pin as server_key)
  rotate   generate a new server key. The current one is kept as server.key.<UTC time>.old
           and, unless -no-grace, also as server.prev.key, which the server keeps accepting so
           players have time to update their server_key. Stop the server first; start it
           again afterwards.
  retire   end the grace period: delete server.prev.key (its backup .old file stays), then
           restart the server. Players still on the old key can no longer sign in.`

func runKey(args []string, stdout io.Writer) error {
	fs := flag.NewFlagSet("ac6lobby key", flag.ContinueOnError)
	fs.Usage = func() { fmt.Fprintln(fs.Output(), keyUsage) }
	dataDir := fs.String("data", defaultDataDir(), "server data directory")
	if err := fs.Parse(args); err != nil {
		return err
	}
	if fs.NArg() < 1 {
		fs.Usage()
		return flag.ErrHelp
	}
	sub := flag.NewFlagSet("ac6lobby key "+fs.Arg(0), flag.ContinueOnError)
	noGrace := sub.Bool("no-grace", false, "rotate: stop accepting the old key at once")
	if err := sub.Parse(fs.Args()[1:]); err != nil {
		return err
	}
	if sub.NArg() > 0 {
		return fmt.Errorf("unexpected argument %q", sub.Arg(0))
	}
	prevPath := filepath.Join(*dataDir, lobby.PrevKeyFile)
	keyPath := filepath.Join(*dataDir, "server.key")
	quiet := slog.New(slog.NewTextHandler(io.Discard, nil))
	switch fs.Arg(0) {
	case "show":
		if _, err := os.Stat(keyPath); err != nil {
			return fmt.Errorf("%s: %w (the server creates it on first start)", keyPath, err)
		}
		sk, err := lobby.LoadOrCreateKey(*dataDir, quiet)
		if err != nil {
			return err
		}
		pk := wire.PublicKey(sk)
		fmt.Fprintf(stdout, "%x\n", pk[:])
		fmt.Fprintf(stdout, "# client config (ac6recomp.toml, [netplay]): server_key = \"%x\"\n", pk[:])
		prev, err := lobby.LoadPrevKeys(*dataDir)
		if err != nil {
			return err
		}
		for _, k := range prev {
			p := wire.PublicKey(k)
			fmt.Fprintf(stdout, "# also accepted until \"key retire\": previous key %x\n", p[:])
		}
		return nil
	case "rotate":
		old, err := os.ReadFile(keyPath)
		if err != nil {
			return fmt.Errorf("%s: %w", keyPath, err)
		}
		oldPK := "unknown"
		if b, err := hex.DecodeString(strings.TrimSpace(string(old))); err == nil && len(b) == wire.KeySize {
			var k wire.Key
			copy(k[:], b)
			pk := wire.PublicKey(k)
			oldPK = hex.EncodeToString(pk[:])
		}
		backup := filepath.Join(*dataDir, "server.key."+time.Now().UTC().Format("20060102T150405Z")+".old")
		if err := lobby.WriteFileAtomic(backup, old, 0o600); err != nil {
			return err
		}
		if *noGrace {
			if err := os.Remove(prevPath); err != nil && !errors.Is(err, os.ErrNotExist) {
				return err
			}
		} else if err := lobby.WriteFileAtomic(prevPath, old, 0o600); err != nil {
			return err
		}
		if err := os.Remove(keyPath); err != nil {
			return err
		}
		sk, err := lobby.LoadOrCreateKey(*dataDir, quiet)
		if err != nil {
			return fmt.Errorf("creating the new key: %w (the old key is in %s)", err, backup)
		}
		pk := wire.PublicKey(sk)
		fmt.Fprintf(stdout, "old public key: %s (secret kept in %s)\n", oldPK, backup)
		fmt.Fprintf(stdout, "new public key: %x\n", pk[:])
		if *noGrace {
			fmt.Fprintln(stdout, "Restart the server, then publish the new key. Clients on the old key cannot sign in until they update.")
		} else {
			fmt.Fprintln(stdout, "Restart the server, then publish the new key. The old key keeps working until you run")
			fmt.Fprintln(stdout, "\"ac6lobby key retire\" (and restart); \"admin metrics\" shows online_old_key.")
		}
		return nil
	case "retire":
		if _, err := os.Stat(prevPath); errors.Is(err, os.ErrNotExist) {
			return fmt.Errorf("no %s: no previous key is being accepted", prevPath)
		}
		if err := os.Remove(prevPath); err != nil {
			return err
		}
		fmt.Fprintf(stdout, "removed %s. Restart the server to stop accepting the previous key.\n", prevPath)
		return nil
	}
	fs.Usage()
	return fmt.Errorf("unknown key command %q", fs.Arg(0))
}

func defaultDataDir() string {
	if env := os.Getenv("AC6LOBBY_DATA"); env != "" {
		return env
	}
	for _, d := range []string{"data", "/var/lib/ac6lobby", "/data"} {
		if _, err := os.Stat(filepath.Join(d, "server.key")); err == nil {
			return d
		}
	}
	return "data"
}

// runHealthcheck sends a C2S packet for an unknown registration; a live server answers with
// the 2-byte REHELLO. This creates no account and no state on the server.
func runHealthcheck(args []string, stdout io.Writer) error {
	fs := flag.NewFlagSet("ac6lobby healthcheck", flag.ContinueOnError)
	target := fs.String("udp", "127.0.0.1:47800", "server UDP address")
	timeout := fs.Duration("timeout", 2*time.Second, "how long to wait for the answer")
	tries := fs.Int("tries", 3, "probes to send")
	quiet := fs.Bool("q", false, "print nothing on success")
	if err := fs.Parse(args); err != nil {
		return err
	}
	rtt, err := probeUDP(*target, *timeout, *tries)
	if err != nil {
		return err
	}
	if !*quiet {
		fmt.Fprintf(stdout, "ok: %s answered in %s\n", *target, rtt.Round(10*time.Microsecond))
	}
	return nil
}

func probeUDP(target string, timeout time.Duration, tries int) (time.Duration, error) {
	raddr, err := net.ResolveUDPAddr("udp", target)
	if err != nil {
		return 0, err
	}
	conn, err := net.DialUDP("udp", nil, raddr)
	if err != nil {
		return 0, err
	}
	defer conn.Close()
	pkt := make([]byte, wire.C2SHeader+wire.TagSize)
	pkt[0], pkt[1] = wire.Magic, wire.TypeC2S
	// peer_id 0 is never assigned; a random boot and tag.
	if _, err := rand.Read(pkt[10:]); err != nil {
		return 0, err
	}
	binary.LittleEndian.PutUint64(pkt[18:26], 1)
	deadline := time.Now().Add(timeout)
	buf := make([]byte, 64)
	per := timeout / time.Duration(max(tries, 1))
	for i := 0; i < max(tries, 1); i++ {
		start := time.Now()
		if _, err := conn.Write(pkt); err != nil {
			return 0, err
		}
		conn.SetReadDeadline(minTime(start.Add(per), deadline))
		for {
			n, err := conn.Read(buf)
			if err != nil {
				break // timeout or ICMP refusal: try again
			}
			if n == 2 && buf[0] == wire.Magic && buf[1] == wire.TypeRehello {
				return time.Since(start), nil
			}
		}
	}
	return 0, errors.New("no answer from " + target)
}

func minTime(a, b time.Time) time.Time {
	if a.Before(b) {
		return a
	}
	return b
}
