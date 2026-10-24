package wire

import (
	"encoding/binary"
	"errors"
)

// Magic starts every datagram.
const Magic = 0xA6

// MaxDatagram bounds every datagram, relay framing included.
const MaxDatagram = 1400

// Packet types (PROTOCOL.md section 2).
const (
	TypeHello    = 0x01
	TypeHelloAck = 0x02
	TypeC2S      = 0x03
	TypeS2C      = 0x04
	TypeRehello  = 0x05
	TypeRelay    = 0x06
	TypeRelayed  = 0x07
	TypeP2P      = 0x10
)

// Header sizes; each header is also the AEAD associated data of its packet.
const (
	HelloHeader    = 50
	HelloAckHeader = 18
	C2SHeader      = 26
	S2CHeader      = 18
	P2PHeader      = 18
	RelayHeader    = 18
	RelayedHeader  = 10
)

// C2S inner kinds.
const (
	KindRPCReq    = 0x10
	KindKeepalive = 0x11
	KindIntroReq  = 0x12
	KindBye       = 0x13
)

// S2C inner kinds.
const (
	KindRPCResp      = 0x20
	KindKeepaliveAck = 0x21
	KindIntro        = 0x22
	KindIntroFail    = 0x23
	KindNotice       = 0x24
)

// P2P inner kinds.
const (
	P2PProbe     = 0x01
	P2PProbeAck  = 0x02
	P2PKeepalive = 0x03
	P2PData      = 0x04
	P2PBye       = 0x05
)

// INTRO_REQ selectors and INTRO_FAIL reasons.
const (
	IntroByVIP    = 1
	IntroByPeerID = 2

	IntroFailUnknown = 1
	IntroFailOffline = 2
	IntroFailRefused = 3
)

// RPC fragmentation limits (PROTOCOL.md 2.3 and 3).
const (
	MaxFragment  = 1100
	MaxFragments = 32
	RPCFragHead  = 7 // kind u8, req_id u32, frag_idx u8, frag_cnt u8
)

var errShort = errors.New("wire: packet too short")

// Rehello is the clear REHELLO datagram.
var Rehello = []byte{Magic, TypeRehello}

// --- HELLO ---

// HelloFields are the clear fields of a HELLO.
type HelloFields struct {
	ClientPK Key
	Boot     Boot
	Ctr      uint64
}

// BuildHello seals plaintext (the JSON hello) under c2sKey.
func BuildHello(f HelloFields, c2sKey Key, plaintext []byte) []byte {
	b := make([]byte, HelloHeader, HelloHeader+len(plaintext)+TagSize)
	b[0], b[1] = Magic, TypeHello
	copy(b[2:34], f.ClientPK[:])
	copy(b[34:42], f.Boot[:])
	binary.LittleEndian.PutUint64(b[42:50], f.Ctr)
	return Seal(b, c2sKey, f.Ctr, b[:HelloHeader], plaintext)
}

// ParseHello splits a HELLO into its clear fields and the sealed part.
func ParseHello(b []byte) (HelloFields, []byte, error) {
	var f HelloFields
	if len(b) < HelloHeader+TagSize || b[0] != Magic || b[1] != TypeHello {
		return f, nil, errShort
	}
	copy(f.ClientPK[:], b[2:34])
	copy(f.Boot[:], b[34:42])
	f.Ctr = binary.LittleEndian.Uint64(b[42:50])
	return f, b[HelloHeader:], nil
}

// --- HELLO_ACK and S2C (same header layout) ---

// BuildServerPacket builds a HELLO_ACK (typ = TypeHelloAck) or S2C (typ = TypeS2C) packet.
func BuildServerPacket(typ byte, serverBoot Boot, ctr uint64, s2cKey Key, plaintext []byte) []byte {
	b := make([]byte, S2CHeader, S2CHeader+len(plaintext)+TagSize)
	b[0], b[1] = Magic, typ
	copy(b[2:10], serverBoot[:])
	binary.LittleEndian.PutUint64(b[10:18], ctr)
	return Seal(b, s2cKey, ctr, b[:S2CHeader], plaintext)
}

// ParseServerPacket returns the server boot, counter and sealed part of a HELLO_ACK or S2C.
func ParseServerPacket(b []byte) (typ byte, serverBoot Boot, ctr uint64, sealed []byte, err error) {
	if len(b) < S2CHeader+TagSize || b[0] != Magic || (b[1] != TypeHelloAck && b[1] != TypeS2C) {
		return 0, serverBoot, 0, nil, errShort
	}
	copy(serverBoot[:], b[2:10])
	return b[1], serverBoot, binary.LittleEndian.Uint64(b[10:18]), b[S2CHeader:], nil
}

// --- C2S ---

// C2SFields are the clear fields of a C2S packet.
type C2SFields struct {
	PeerID uint64
	Boot   Boot
	Ctr    uint64
}

// BuildC2S seals inner under c2sKey.
func BuildC2S(f C2SFields, c2sKey Key, inner []byte) []byte {
	b := make([]byte, C2SHeader, C2SHeader+len(inner)+TagSize)
	b[0], b[1] = Magic, TypeC2S
	binary.LittleEndian.PutUint64(b[2:10], f.PeerID)
	copy(b[10:18], f.Boot[:])
	binary.LittleEndian.PutUint64(b[18:26], f.Ctr)
	return Seal(b, c2sKey, f.Ctr, b[:C2SHeader], inner)
}

// ParseC2S splits a C2S packet into its clear fields and the sealed part.
func ParseC2S(b []byte) (C2SFields, []byte, error) {
	var f C2SFields
	if len(b) < C2SHeader+TagSize || b[0] != Magic || b[1] != TypeC2S {
		return f, nil, errShort
	}
	f.PeerID = binary.LittleEndian.Uint64(b[2:10])
	copy(f.Boot[:], b[10:18])
	f.Ctr = binary.LittleEndian.Uint64(b[18:26])
	return f, b[C2SHeader:], nil
}

// --- P2P ---

// BuildP2P seals inner under the sender's P2P key.
func BuildP2P(srcPeerID, ctr uint64, sendKey Key, inner []byte) []byte {
	b := make([]byte, P2PHeader, P2PHeader+len(inner)+TagSize)
	b[0], b[1] = Magic, TypeP2P
	binary.LittleEndian.PutUint64(b[2:10], srcPeerID)
	binary.LittleEndian.PutUint64(b[10:18], ctr)
	return Seal(b, sendKey, ctr, b[:P2PHeader], inner)
}

// ParseP2P returns the source peer, counter and sealed part of a P2P packet.
func ParseP2P(b []byte) (srcPeerID, ctr uint64, sealed []byte, err error) {
	if len(b) < P2PHeader+TagSize || b[0] != Magic || b[1] != TypeP2P {
		return 0, 0, nil, errShort
	}
	return binary.LittleEndian.Uint64(b[2:10]), binary.LittleEndian.Uint64(b[10:18]), b[P2PHeader:], nil
}

// --- RELAY / RELAYED ---

// BuildRelay frames an inner P2P packet for the server to forward to dst.
func BuildRelay(src, dst uint64, innerP2P []byte) []byte {
	b := make([]byte, RelayHeader, RelayHeader+len(innerP2P))
	b[0], b[1] = Magic, TypeRelay
	binary.LittleEndian.PutUint64(b[2:10], src)
	binary.LittleEndian.PutUint64(b[10:18], dst)
	return append(b, innerP2P...)
}

// ParseRelay returns the RELAY addressing and the inner P2P packet. The inner packet must
// look like a P2P packet (magic, type, full header and tag) whose source is src.
func ParseRelay(b []byte) (src, dst uint64, inner []byte, err error) {
	if len(b) < RelayHeader+P2PHeader+TagSize || b[0] != Magic || b[1] != TypeRelay {
		return 0, 0, nil, errShort
	}
	src = binary.LittleEndian.Uint64(b[2:10])
	dst = binary.LittleEndian.Uint64(b[10:18])
	inner = b[RelayHeader:]
	if inner[0] != Magic || inner[1] != TypeP2P || binary.LittleEndian.Uint64(inner[2:10]) != src {
		return 0, 0, nil, errors.New("wire: relay inner packet is not a P2P packet from src")
	}
	return src, dst, inner, nil
}

// BuildRelayed frames a forwarded P2P packet for its receiver.
func BuildRelayed(src uint64, innerP2P []byte) []byte {
	b := make([]byte, RelayedHeader, RelayedHeader+len(innerP2P))
	b[0], b[1] = Magic, TypeRelayed
	binary.LittleEndian.PutUint64(b[2:10], src)
	return append(b, innerP2P...)
}

// ParseRelayed returns the source peer and the inner P2P packet of a RELAYED datagram.
func ParseRelayed(b []byte) (src uint64, inner []byte, err error) {
	if len(b) < RelayedHeader+P2PHeader+TagSize || b[0] != Magic || b[1] != TypeRelayed {
		return 0, nil, errShort
	}
	return binary.LittleEndian.Uint64(b[2:10]), b[RelayedHeader:], nil
}

// --- RPC fragments (RPC_REQ and RPC_RESP inner messages) ---

// RPCFrag is one fragment of an RPC request or response.
type RPCFrag struct {
	ReqID uint32
	Idx   uint8
	Cnt   uint8
	Data  []byte
}

// AppendRPCFrag appends kind, req_id, frag_idx, frag_cnt and data.
func AppendRPCFrag(dst []byte, kind byte, f RPCFrag) []byte {
	dst = append(dst, kind)
	dst = binary.LittleEndian.AppendUint32(dst, f.ReqID)
	dst = append(dst, f.Idx, f.Cnt)
	return append(dst, f.Data...)
}

// ParseRPCFrag parses the body that follows the kind byte. It enforces the fragment limits.
func ParseRPCFrag(body []byte) (RPCFrag, error) {
	var f RPCFrag
	if len(body) < RPCFragHead-1 {
		return f, errShort
	}
	f.ReqID = binary.LittleEndian.Uint32(body[0:4])
	f.Idx, f.Cnt = body[4], body[5]
	f.Data = body[6:]
	if f.Cnt == 0 || f.Cnt > MaxFragments || f.Idx >= f.Cnt || len(f.Data) > MaxFragment {
		return f, errors.New("wire: bad rpc fragment")
	}
	return f, nil
}

// SplitRPC splits a JSON message into fragments of at most MaxFragment bytes. It returns nil
// when the message needs more than MaxFragments fragments. An empty message is one empty
// fragment.
func SplitRPC(reqID uint32, msg []byte) []RPCFrag {
	n := (len(msg) + MaxFragment - 1) / MaxFragment
	if n == 0 {
		n = 1
	}
	if n > MaxFragments {
		return nil
	}
	frags := make([]RPCFrag, 0, n)
	for i := 0; i < n; i++ {
		lo := i * MaxFragment
		hi := min(lo+MaxFragment, len(msg))
		frags = append(frags, RPCFrag{ReqID: reqID, Idx: uint8(i), Cnt: uint8(n), Data: msg[lo:hi]})
	}
	return frags
}

// --- INTRO_REQ / INTRO / INTRO_FAIL ---

// AppendIntroReq appends an INTRO_REQ inner message. For by = IntroByVIP, value is the IPv4
// address as a host-order u32 (written in network order); for IntroByPeerID, the peer ID.
func AppendIntroReq(dst []byte, reqID uint32, by uint8, value uint64) []byte {
	dst = append(dst, KindIntroReq)
	dst = binary.LittleEndian.AppendUint32(dst, reqID)
	dst = append(dst, by)
	if by == IntroByVIP {
		return binary.BigEndian.AppendUint32(dst, uint32(value))
	}
	return binary.LittleEndian.AppendUint64(dst, value)
}

// ParseIntroReq parses the body after the kind byte.
func ParseIntroReq(body []byte) (reqID uint32, by uint8, value uint64, err error) {
	if len(body) < 5 {
		return 0, 0, 0, errShort
	}
	reqID, by = binary.LittleEndian.Uint32(body[0:4]), body[4]
	switch by {
	case IntroByVIP:
		if len(body) != 9 {
			return 0, 0, 0, errShort
		}
		return reqID, by, uint64(binary.BigEndian.Uint32(body[5:9])), nil
	case IntroByPeerID:
		if len(body) != 13 {
			return 0, 0, 0, errShort
		}
		return reqID, by, binary.LittleEndian.Uint64(body[5:13]), nil
	}
	return 0, 0, 0, errors.New("wire: bad intro selector")
}

// AppendIntro appends an INTRO inner message: req_id then the JSON peer record.
func AppendIntro(dst []byte, reqID uint32, record []byte) []byte {
	dst = append(dst, KindIntro)
	dst = binary.LittleEndian.AppendUint32(dst, reqID)
	return append(dst, record...)
}

// AppendIntroFail appends an INTRO_FAIL inner message.
func AppendIntroFail(dst []byte, reqID uint32, reason uint8) []byte {
	dst = append(dst, KindIntroFail)
	dst = binary.LittleEndian.AppendUint32(dst, reqID)
	return append(dst, reason)
}
