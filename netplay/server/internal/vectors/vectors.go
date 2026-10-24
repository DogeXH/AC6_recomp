// Package vectors builds the deterministic cross-implementation test vectors written by
// cmd/vectors. Every input is fixed, so the output is byte-for-byte reproducible.
package vectors

import (
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"fmt"

	"ac6lobby/internal/wire"
)

// File is the top-level vectors.json document.
type File struct {
	Version   int          `json:"version"`
	Generator string       `json:"generator"`
	Notes     []string     `json:"notes"`
	Keys      []KeyVector  `json:"x25519"`
	DH        []DHVector   `json:"dh"`
	KDF       []KDFVector  `json:"kdf"`
	AEAD      []AEADVector `json:"aead"`
	Packets   Packets      `json:"packets"`
	Replay    ReplayVector `json:"replay"`
	Compat    []CompatVec  `json:"compat"`
}

// KeyVector: pk = X25519(sk, basepoint).
type KeyVector struct {
	Name string `json:"name"`
	SK   string `json:"sk"`
	PK   string `json:"pk"`
}

// DHVector: shared = X25519(a.sk, b.pk) = X25519(b.sk, a.pk).
type DHVector struct {
	A      string `json:"a"`
	B      string `json:"b"`
	Shared string `json:"shared"`
}

// KDFVector: out = BLAKE2b-256(key, label || parts...). Message is label || parts, in hex.
type KDFVector struct {
	Name    string   `json:"name"`
	Label   string   `json:"label"`
	Key     string   `json:"key"`
	Parts   []string `json:"parts"`
	Message string   `json:"message"`
	Out     string   `json:"out"`
}

// AEADVector: sealed = XChaCha20-Poly1305(key, nonce, ad, plaintext) = ciphertext || tag.
type AEADVector struct {
	Name      string `json:"name"`
	Key       string `json:"key"`
	Ctr       uint64 `json:"ctr"`
	Nonce     string `json:"nonce"`
	AD        string `json:"ad"`
	Plaintext string `json:"plaintext"`
	Sealed    string `json:"sealed"`
}

// PacketVector is one complete datagram plus the inputs that built it.
type PacketVector struct {
	Description string            `json:"description"`
	Key         string            `json:"key,omitempty"`
	KeyName     string            `json:"key_name,omitempty"`
	Ctr         uint64            `json:"ctr,omitempty"`
	Header      string            `json:"header"`
	Plaintext   string            `json:"plaintext,omitempty"`
	PlainText   string            `json:"plaintext_text,omitempty"`
	Fields      map[string]string `json:"fields"`
	Bytes       string            `json:"bytes"`
}

// Packets holds the complete-packet vectors.
type Packets struct {
	Hello      PacketVector `json:"hello"`
	HelloAck   PacketVector `json:"hello_ack"`
	C2SRPCReq  PacketVector `json:"c2s_rpc_req"`
	S2CRPCResp PacketVector `json:"s2c_rpc_resp"`
	P2PData    PacketVector `json:"p2p_data"`
	Relay      PacketVector `json:"relay"`
	Relayed    PacketVector `json:"relayed"`
}

// ReplayVector: feed Sequence in order to an empty 1024-bit window; Accepted[i] says whether
// counter i passes (a passing counter is then recorded as seen).
type ReplayVector struct {
	Sequence []uint64 `json:"sequence"`
	Accepted []bool   `json:"accepted"`
}

// CompatVec: key = BLAKE2b-128(key = 32 zero bytes, message).
type CompatVec struct {
	Title    string `json:"title"`
	Media    string `json:"media"`
	Version  uint32 `json:"version"`
	Protocol uint32 `json:"protocol"`
	FPSMode  string `json:"fps_mode"`
	Message  string `json:"message"`
	Key      string `json:"key"`
}

// Fixed inputs.
var (
	ClientSK   = seq(0x01)
	ServerSK   = seq(0x21)
	PeerBSK    = seq(0x41)
	ClientBoot = wire.Boot{0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef}
	ServerBoot = wire.Boot{0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54, 0x32, 0x10}
	PeerBBoot  = wire.Boot{0x0f, 0x1e, 0x2d, 0x3c, 0x4b, 0x5a, 0x69, 0x78}
)

const (
	PeerA uint64 = 0x9f3c5a7b11223344
	PeerB uint64 = 0x5e6f708192a3b4c5
)

func seq(start byte) wire.Key {
	var k wire.Key
	for i := range k {
		k[i] = start + byte(i)
	}
	return k
}

func hx(b []byte) string { return hex.EncodeToString(b) }

func mustDH(sk, pk wire.Key) wire.Key {
	k, err := wire.DH(sk, pk)
	if err != nil {
		panic(err)
	}
	return k
}

func kdfVector(name string, dh wire.Key, label string, parts ...[]byte) (KDFVector, wire.Key) {
	out := wire.KDF(dh, label, parts...)
	msg := []byte(label)
	hp := make([]string, 0, len(parts))
	for _, p := range parts {
		msg = append(msg, p...)
		hp = append(hp, hx(p))
	}
	return KDFVector{Name: name, Label: label, Key: hx(dh[:]), Parts: hp, Message: hx(msg), Out: hx(out[:])}, out
}

func aeadVector(name string, key wire.Key, ctr uint64, ad, pt []byte) AEADVector {
	n := wire.Nonce(ctr)
	return AEADVector{Name: name, Key: hx(key[:]), Ctr: ctr, Nonce: hx(n[:]), AD: hx(ad),
		Plaintext: hx(pt), Sealed: hx(wire.Seal(nil, key, ctr, ad, pt))}
}

// CompatInputs are the fixed compat-key inputs.
func compatVector(fps string) CompatVec {
	const title, media, version, protocol = 0x4E4D07D1, 0x531C30BE, 8, 1
	key := wire.CompatKey(title, media, version, protocol, fps)
	return CompatVec{Title: fmt.Sprintf("%08X", title), Media: fmt.Sprintf("%08X", media),
		Version: version, Protocol: protocol, FPSMode: fps,
		Message: hx(wire.CompatMessage(title, media, version, protocol, fps)), Key: hx(key[:])}
}

func u64le(v uint64) []byte { return binary.LittleEndian.AppendUint64(nil, v) }

// Generate builds the vectors document.
func Generate() File {
	cPK, sPK, bPK := wire.PublicKey(ClientSK), wire.PublicKey(ServerSK), wire.PublicKey(PeerBSK)
	dhCS := mustDH(ClientSK, sPK)
	if mustDH(ServerSK, cPK) != dhCS {
		panic("vectors: DH is not symmetric")
	}
	dhAB := mustDH(ClientSK, bPK)
	if mustDH(PeerBSK, cPK) != dhAB {
		panic("vectors: DH is not symmetric")
	}

	kC2S, c2s := kdfVector("c2s", dhCS, wire.LabelC2S, cPK[:], sPK[:], ClientBoot[:])
	kS2C, s2c := kdfVector("s2c", dhCS, wire.LabelS2C, cPK[:], sPK[:], ClientBoot[:], ServerBoot[:])
	kAB, p2pAB := kdfVector("p2p_a_to_b", dhAB, wire.LabelP2P, cPK[:], bPK[:], ClientBoot[:], PeerBBoot[:])
	kBA, _ := kdfVector("p2p_b_to_a", dhAB, wire.LabelP2P, bPK[:], cPK[:], PeerBBoot[:], ClientBoot[:])
	if c2s != wire.C2SKey(dhCS, cPK, sPK, ClientBoot) || s2c != wire.S2CKey(dhCS, cPK, sPK, ClientBoot, ServerBoot) ||
		p2pAB != wire.P2PKey(dhAB, cPK, bPK, ClientBoot, PeerBBoot) {
		panic("vectors: key helpers disagree with KDF")
	}

	fixedKey := seq(0x80)
	long := make([]byte, 200)
	for i := range long {
		long[i] = byte(i * 7)
	}
	aead := []AEADVector{
		aeadVector("empty", fixedKey, 1, nil, nil),
		aeadVector("short", fixedKey, 2, []byte{0xA6, 0x03}, []byte("ac6net")),
		aeadVector("multi_block", fixedKey, 0x0102030405060708, []byte("associated data"), long),
		aeadVector("c2s_key", c2s, 0xFFFFFFFFFFFFFFFE, []byte{0xA6}, []byte(`{"ping":true}`)),
	}

	compat60 := compatVector("60")
	f := File{
		Version:   1,
		Generator: "netplay/server/cmd/vectors (go run ./cmd/vectors)",
		Notes: []string{
			"All binary values are lower-case hex. u64/u32 fields inside packets are little-endian unless noted.",
			"nonce = ctr (u64 LE) || 16 zero bytes; sealed = ciphertext || 16-byte Poly1305 tag (XChaCha20-Poly1305, IETF construction).",
			"Packet 'header' is the AEAD associated data: bytes[0..50) for HELLO, [0..26) for C2S, [0..18) for HELLO_ACK, S2C and P2P.",
			"The client (peer A) sends HELLO with ctr 1 and the RPC with ctr 2 under the same c2s key; the server sends HELLO_ACK with ctr 1 and the RPC response with ctr 2 under the s2c key.",
			"p2p_data carries a vnet datagram on channel 0x01: proto 254, src/dst port 999 (BE), msg_id 1 (u16 LE), frag 0 of 1.",
		},
		Keys: []KeyVector{
			{Name: "client", SK: hx(ClientSK[:]), PK: hx(cPK[:])},
			{Name: "server", SK: hx(ServerSK[:]), PK: hx(sPK[:])},
			{Name: "peer_b", SK: hx(PeerBSK[:]), PK: hx(bPK[:])},
		},
		DH: []DHVector{
			{A: "client", B: "server", Shared: hx(dhCS[:])},
			{A: "client", B: "peer_b", Shared: hx(dhAB[:])},
		},
		KDF:    []KDFVector{kC2S, kS2C, kAB, kBA},
		AEAD:   aead,
		Compat: []CompatVec{compat60, compatVector("30")},
	}

	// HELLO.
	helloJSON := `{"v":1,"client":"ac6recomp-netplay/0.1","gamertag":"Talisman","title":"4E4D07D1",` +
		`"compat":"` + compat60.Key + `","local":[["192.168.1.20",51234]],"privacy":false}`
	hello := wire.BuildHello(wire.HelloFields{ClientPK: cPK, Boot: ClientBoot, Ctr: 1}, c2s, []byte(helloJSON))
	f.Packets.Hello = PacketVector{
		Description: "HELLO (0x01), client to server",
		Key:         hx(c2s[:]), KeyName: "c2s", Ctr: 1,
		Header: hx(hello[:wire.HelloHeader]), Plaintext: hx([]byte(helloJSON)), PlainText: helloJSON,
		Fields: map[string]string{"c_pk": hx(cPK[:]), "c_boot": hx(ClientBoot[:]), "ctr": "1"},
		Bytes:  hx(hello),
	}

	// HELLO_ACK.
	ackJSON := `{"v":1,"peer_id":"9f3c5a7b11223344","xuid":"0009a1b2c3d4e5f6","vip":"10.64.0.7",` +
		`"gamertag":"Talisman","public":["203.0.113.9",40123],"server_time_ms":1760000000000,"motd":""}`
	ack := wire.BuildServerPacket(wire.TypeHelloAck, ServerBoot, 1, s2c, []byte(ackJSON))
	f.Packets.HelloAck = PacketVector{
		Description: "HELLO_ACK (0x02), server to client",
		Key:         hx(s2c[:]), KeyName: "s2c", Ctr: 1,
		Header: hx(ack[:wire.S2CHeader]), Plaintext: hx([]byte(ackJSON)), PlainText: ackJSON,
		Fields: map[string]string{"s_boot": hx(ServerBoot[:]), "ctr": "1"},
		Bytes:  hx(ack),
	}

	// C2S RPC_REQ.
	reqJSON := `{"id":1,"m":"whoami","a":{}}`
	reqInner := wire.AppendRPCFrag(nil, wire.KindRPCReq, wire.RPCFrag{ReqID: 1, Idx: 0, Cnt: 1, Data: []byte(reqJSON)})
	req := wire.BuildC2S(wire.C2SFields{PeerID: PeerA, Boot: ClientBoot, Ctr: 2}, c2s, reqInner)
	f.Packets.C2SRPCReq = PacketVector{
		Description: "C2S (0x03) carrying RPC_REQ (0x10): req_id 1, fragment 0 of 1",
		Key:         hx(c2s[:]), KeyName: "c2s", Ctr: 2,
		Header: hx(req[:wire.C2SHeader]), Plaintext: hx(reqInner), PlainText: reqJSON,
		Fields: map[string]string{"peer_id": fmt.Sprintf("%016x", PeerA), "peer_id_le": hx(u64le(PeerA)),
			"c_boot": hx(ClientBoot[:]), "ctr": "2", "kind": "10", "req_id": "1", "frag_idx": "0", "frag_cnt": "1"},
		Bytes: hx(req),
	}

	// S2C RPC_RESP.
	respJSON := `{"id":1,"ok":true,"r":{"peer_id":"9f3c5a7b11223344","xuid":"0009a1b2c3d4e5f6",` +
		`"vip":"10.64.0.7","gamertag":"Talisman","public":["203.0.113.9",40123]}}`
	respInner := wire.AppendRPCFrag(nil, wire.KindRPCResp, wire.RPCFrag{ReqID: 1, Idx: 0, Cnt: 1, Data: []byte(respJSON)})
	resp := wire.BuildServerPacket(wire.TypeS2C, ServerBoot, 2, s2c, respInner)
	f.Packets.S2CRPCResp = PacketVector{
		Description: "S2C (0x04) carrying RPC_RESP (0x20): req_id 1, fragment 0 of 1",
		Key:         hx(s2c[:]), KeyName: "s2c", Ctr: 2,
		Header: hx(resp[:wire.S2CHeader]), Plaintext: hx(respInner), PlainText: respJSON,
		Fields: map[string]string{"s_boot": hx(ServerBoot[:]), "ctr": "2", "kind": "20", "req_id": "1",
			"frag_idx": "0", "frag_cnt": "1"},
		Bytes: hx(resp),
	}

	// P2P DATA, A to B, vnet datagram channel.
	payload := []byte("AC6 datagram")
	inner := []byte{wire.P2PData, 0x01, 254, 0x03, 0xE7, 0x03, 0xE7, 0x01, 0x00, 0x00, 0x01}
	inner = append(inner, payload...)
	p2p := wire.BuildP2P(PeerA, 1, p2pAB, inner)
	f.Packets.P2PData = PacketVector{
		Description: "P2P (0x10) DATA (0x04) from peer A (client) to peer B, channel 0x01",
		Key:         hx(p2pAB[:]), KeyName: "p2p_a_to_b", Ctr: 1,
		Header: hx(p2p[:wire.P2PHeader]), Plaintext: hx(inner),
		Fields: map[string]string{"src_peer_id": fmt.Sprintf("%016x", PeerA), "ctr": "1", "kind": "04",
			"channel": "01", "payload_text": string(payload)},
		Bytes: hx(p2p),
	}

	relay := wire.BuildRelay(PeerA, PeerB, p2p)
	f.Packets.Relay = PacketVector{
		Description: "RELAY (0x06), clear framing around packets.p2p_data, from A to B",
		Header:      hx(relay[:wire.RelayHeader]),
		Fields: map[string]string{"src_peer_id": fmt.Sprintf("%016x", PeerA),
			"dst_peer_id": fmt.Sprintf("%016x", PeerB)},
		Bytes: hx(relay),
	}
	relayed := wire.BuildRelayed(PeerA, p2p)
	f.Packets.Relayed = PacketVector{
		Description: "RELAYED (0x07), what B receives for packets.relay",
		Header:      hx(relayed[:wire.RelayedHeader]),
		Fields:      map[string]string{"src_peer_id": fmt.Sprintf("%016x", PeerA)},
		Bytes:       hx(relayed),
	}

	// Replay window.
	f.Replay.Sequence = []uint64{0, 1, 1, 3, 2, 2, 1030, 6, 7, 5, 1031, 1030, 2000, 976, 977, 3000}
	var w wire.ReplayWindow
	for _, c := range f.Replay.Sequence {
		ok := w.Check(c)
		if ok {
			w.Accept(c)
		}
		f.Replay.Accepted = append(f.Replay.Accepted, ok)
	}
	return f
}

// Marshal renders the document as indented JSON with a trailing newline.
func Marshal(f File) []byte {
	b, err := json.MarshalIndent(f, "", "  ")
	if err != nil {
		panic(err)
	}
	return append(b, '\n')
}
