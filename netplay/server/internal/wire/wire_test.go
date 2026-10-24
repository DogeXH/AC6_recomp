package wire_test

import (
	"bytes"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"math/rand/v2"
	"os"
	"testing"

	"ac6lobby/internal/vectors"
	"ac6lobby/internal/wire"
)

func unhex(t *testing.T, s string) []byte {
	t.Helper()
	b, err := hex.DecodeString(s)
	if err != nil {
		t.Fatalf("bad hex %q: %v", s, err)
	}
	return b
}

func key(t *testing.T, s string) wire.Key {
	var k wire.Key
	b := unhex(t, s)
	if len(b) != 32 {
		t.Fatalf("key %q is %d bytes", s, len(b))
	}
	copy(k[:], b)
	return k
}

func loadVectors(t *testing.T) vectors.File {
	t.Helper()
	data, err := os.ReadFile("testdata/vectors.json")
	if err != nil {
		t.Fatal(err)
	}
	var f vectors.File
	if err := json.Unmarshal(data, &f); err != nil {
		t.Fatal(err)
	}
	return f
}

// The committed vectors must be exactly what the generator produces.
func TestVectorsFileIsCurrent(t *testing.T) {
	data, err := os.ReadFile("testdata/vectors.json")
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(data, vectors.Marshal(vectors.Generate())) {
		t.Fatal("testdata/vectors.json is stale: run go run ./cmd/vectors -o internal/wire/testdata/vectors.json")
	}
}

// Recompute every vector from its inputs with the wire primitives.
func TestVectorsSelfCheck(t *testing.T) {
	f := loadVectors(t)
	sks := map[string]wire.Key{}
	pks := map[string]wire.Key{}
	for _, k := range f.Keys {
		sk := key(t, k.SK)
		if pk := wire.PublicKey(sk); hex.EncodeToString(pk[:]) != k.PK {
			t.Errorf("x25519 %s: pk mismatch", k.Name)
		}
		sks[k.Name], pks[k.Name] = sk, key(t, k.PK)
	}
	for _, d := range f.DH {
		ab, err := wire.DH(sks[d.A], pks[d.B])
		if err != nil {
			t.Fatal(err)
		}
		ba, _ := wire.DH(sks[d.B], pks[d.A])
		if hex.EncodeToString(ab[:]) != d.Shared || ab != ba {
			t.Errorf("dh %s/%s mismatch", d.A, d.B)
		}
	}
	for _, k := range f.KDF {
		var parts [][]byte
		msg := []byte(k.Label)
		for _, p := range k.Parts {
			parts = append(parts, unhex(t, p))
			msg = append(msg, unhex(t, p)...)
		}
		out := wire.KDF(key(t, k.Key), k.Label, parts...)
		if hex.EncodeToString(out[:]) != k.Out || hex.EncodeToString(msg) != k.Message {
			t.Errorf("kdf %s mismatch", k.Name)
		}
	}
	for _, a := range f.AEAD {
		n := wire.Nonce(a.Ctr)
		if hex.EncodeToString(n[:]) != a.Nonce {
			t.Errorf("aead %s: nonce mismatch", a.Name)
		}
		sealed := wire.Seal(nil, key(t, a.Key), a.Ctr, unhex(t, a.AD), unhex(t, a.Plaintext))
		if hex.EncodeToString(sealed) != a.Sealed {
			t.Errorf("aead %s: sealed mismatch", a.Name)
		}
		pt, err := wire.Open(key(t, a.Key), a.Ctr, unhex(t, a.AD), sealed)
		if err != nil || !bytes.Equal(pt, unhex(t, a.Plaintext)) {
			t.Errorf("aead %s: open failed", a.Name)
		}
		sealed[0] ^= 1
		if _, err := wire.Open(key(t, a.Key), a.Ctr, unhex(t, a.AD), sealed); err == nil {
			t.Errorf("aead %s: tampered data opened", a.Name)
		}
	}
	for name, p := range map[string]vectors.PacketVector{"hello": f.Packets.Hello, "hello_ack": f.Packets.HelloAck,
		"c2s_rpc_req": f.Packets.C2SRPCReq, "s2c_rpc_resp": f.Packets.S2CRPCResp, "p2p_data": f.Packets.P2PData} {
		b := unhex(t, p.Bytes)
		hdr := unhex(t, p.Header)
		if !bytes.HasPrefix(b, hdr) {
			t.Errorf("%s: header is not a prefix", name)
			continue
		}
		pt, err := wire.Open(key(t, p.Key), p.Ctr, hdr, b[len(hdr):])
		if err != nil || !bytes.Equal(pt, unhex(t, p.Plaintext)) {
			t.Errorf("%s: does not decrypt to the plaintext", name)
		}
	}
	// Structural checks with the parsers.
	hf, _, err := wire.ParseHello(unhex(t, f.Packets.Hello.Bytes))
	if err != nil || hf.Ctr != 1 || hex.EncodeToString(hf.ClientPK[:]) != f.Keys[0].PK {
		t.Errorf("hello parse: %+v %v", hf, err)
	}
	cf, _, err := wire.ParseC2S(unhex(t, f.Packets.C2SRPCReq.Bytes))
	if err != nil || cf.PeerID != vectors.PeerA || cf.Ctr != 2 {
		t.Errorf("c2s parse: %+v %v", cf, err)
	}
	src, dst, inner, err := wire.ParseRelay(unhex(t, f.Packets.Relay.Bytes))
	if err != nil || src != vectors.PeerA || dst != vectors.PeerB || hex.EncodeToString(inner) != f.Packets.P2PData.Bytes {
		t.Errorf("relay parse: %v", err)
	}
	rsrc, rinner, err := wire.ParseRelayed(unhex(t, f.Packets.Relayed.Bytes))
	if err != nil || rsrc != vectors.PeerA || hex.EncodeToString(rinner) != f.Packets.P2PData.Bytes {
		t.Errorf("relayed parse: %v", err)
	}
	// Replay sequence.
	var w wire.ReplayWindow
	for i, c := range f.Replay.Sequence {
		ok := w.Check(c)
		if ok {
			w.Accept(c)
		}
		if ok != f.Replay.Accepted[i] {
			t.Errorf("replay step %d (ctr %d): got %v", i, c, ok)
		}
	}
	// Compat keys.
	for _, c := range f.Compat {
		k := wire.CompatKey(0x4E4D07D1, 0x531C30BE, c.Version, c.Protocol, c.FPSMode)
		if hex.EncodeToString(k[:]) != c.Key {
			t.Errorf("compat %s mismatch", c.FPSMode)
		}
	}
	if f.Compat[0].Key != "a51a535851e632111a59c1dcd86a42ec" {
		t.Errorf("compat 60 key changed: %s", f.Compat[0].Key)
	}
}

func TestReplayWindow(t *testing.T) {
	var w wire.ReplayWindow
	accept := func(c uint64) bool {
		if !w.Check(c) {
			return false
		}
		w.Accept(c)
		return true
	}
	for c := uint64(1); c <= 3000; c++ {
		if !accept(c) {
			t.Fatalf("in-order ctr %d rejected", c)
		}
	}
	for c := uint64(1977); c <= 3000; c++ {
		if accept(c) {
			t.Fatalf("duplicate ctr %d accepted", c)
		}
	}
	if accept(1976) {
		t.Fatal("ctr older than the window accepted")
	}
	// Out-of-order inside the window, across word boundaries.
	w = wire.ReplayWindow{}
	order := []uint64{100, 37, 99, 1, 64, 65, 63, 128, 129, 2000, 1000, 977, 976}
	want := []bool{true, true, true, true, true, true, true, true, true, true, true, true, false}
	for i, c := range order {
		if got := accept(c); got != want[i] {
			t.Errorf("ctr %d: got %v want %v", c, got, want[i])
		}
	}
	for _, c := range order[:len(order)-1] {
		if c >= 977 && accept(c) {
			t.Errorf("ctr %d accepted twice", c)
		}
	}
	// Random shuffles inside the window behave like a set.
	r := rand.New(rand.NewPCG(1, 2))
	for trial := 0; trial < 50; trial++ {
		w = wire.ReplayWindow{}
		seen := map[uint64]bool{}
		var hi uint64
		for i := 0; i < 2000; i++ {
			c := uint64(r.IntN(3000) + 1)
			fresh := !seen[c] && (c > hi || hi-c < wire.ReplayWindowBits)
			if got := accept(c); got != fresh {
				t.Fatalf("trial %d ctr %d (hi %d): got %v want %v", trial, c, hi, got, fresh)
			}
			if fresh {
				seen[c] = true
				hi = max(hi, c)
			}
		}
	}
}

// Parsers must reject truncated and corrupted input without panicking.
func TestParsersBounds(t *testing.T) {
	f := vectors.Generate()
	samples := [][]byte{}
	for _, p := range []vectors.PacketVector{f.Packets.Hello, f.Packets.HelloAck, f.Packets.C2SRPCReq,
		f.Packets.S2CRPCResp, f.Packets.P2PData, f.Packets.Relay, f.Packets.Relayed} {
		b, _ := hex.DecodeString(p.Bytes)
		samples = append(samples, b)
	}
	r := rand.New(rand.NewPCG(3, 4))
	for _, s := range samples {
		for n := 0; n <= len(s); n++ {
			b := append([]byte{}, s[:n]...)
			if n > 0 && r.IntN(2) == 0 {
				b[r.IntN(n)] ^= byte(r.IntN(255) + 1)
			}
			wire.ParseHello(b)
			wire.ParseServerPacket(b)
			wire.ParseC2S(b)
			wire.ParseP2P(b)
			wire.ParseRelay(b)
			wire.ParseRelayed(b)
			if len(b) > 1 {
				wire.ParseRPCFrag(b[1:])
				wire.ParseIntroReq(b[1:])
			}
		}
	}
	if _, err := wire.ParseRPCFrag([]byte{1, 0, 0, 0, 0, 0}); err == nil {
		t.Error("frag_cnt 0 accepted")
	}
	if _, err := wire.ParseRPCFrag([]byte{1, 0, 0, 0, 2, 2}); err == nil {
		t.Error("frag_idx >= frag_cnt accepted")
	}
	if _, err := wire.ParseRPCFrag([]byte{1, 0, 0, 0, 0, 33}); err == nil {
		t.Error("33 fragments accepted")
	}
	big := make([]byte, 6+wire.MaxFragment+1)
	big[5] = 1
	if _, err := wire.ParseRPCFrag(big); err == nil {
		t.Error("oversized fragment accepted")
	}
	// INTRO_REQ round trip.
	b := wire.AppendIntroReq(nil, 7, wire.IntroByVIP, 0x0A400009)
	if !bytes.Equal(b[6:], []byte{10, 64, 0, 9}) {
		t.Errorf("vip not in network order: %x", b)
	}
	id, by, v, err := wire.ParseIntroReq(b[1:])
	if err != nil || id != 7 || by != wire.IntroByVIP || v != 0x0A400009 {
		t.Errorf("intro_req by vip: %d %d %x %v", id, by, v, err)
	}
	b = wire.AppendIntroReq(nil, 8, wire.IntroByPeerID, 0x1122334455667788)
	if binary.LittleEndian.Uint64(b[6:]) != 0x1122334455667788 {
		t.Errorf("peer id not LE: %x", b)
	}
	if _, _, _, err := wire.ParseIntroReq(append(b[1:], 0)); err == nil {
		t.Error("intro_req with trailing byte accepted")
	}
}

func TestSplitRPC(t *testing.T) {
	msg := bytes.Repeat([]byte("x"), wire.MaxFragment*3+5)
	frags := wire.SplitRPC(9, msg)
	if len(frags) != 4 || frags[3].Cnt != 4 || len(frags[3].Data) != 5 {
		t.Fatalf("split: %d fragments", len(frags))
	}
	var joined []byte
	for i, f := range frags {
		if int(f.Idx) != i || f.ReqID != 9 {
			t.Fatalf("fragment %d header %+v", i, f)
		}
		joined = append(joined, f.Data...)
	}
	if !bytes.Equal(joined, msg) {
		t.Fatal("join mismatch")
	}
	if wire.SplitRPC(1, make([]byte, wire.MaxFragment*wire.MaxFragments+1)) != nil {
		t.Fatal("33-fragment message split")
	}
	if f := wire.SplitRPC(1, nil); len(f) != 1 || f[0].Cnt != 1 {
		t.Fatal("empty message must be one fragment")
	}
	if _, err := wire.DH(wire.Key{}, wire.Key{}); err == nil {
		t.Fatal("low-order DH accepted")
	}
}
