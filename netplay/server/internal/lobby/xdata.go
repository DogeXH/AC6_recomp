package lobby

import (
	"bytes"
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"math"
	"strconv"
)

// XUSER_DATA_TYPE values (PROTOCOL.md section 3).
const (
	TypeContext  = 0
	TypeInt32    = 1
	TypeInt64    = 2
	TypeDouble   = 3
	TypeUnicode  = 4
	TypeFloat    = 5
	TypeBinary   = 6
	TypeDateTime = 7
	TypeNull     = 255 // only produced by the server for a missing stats column
)

// Bounds on XUSER data accepted from clients.
const (
	maxDataBytes  = 1024 // decoded unicode/binary property value
	maxProps      = 64
	maxContexts   = 64
	maxStatsViews = 16
)

// Prop is an XUSER_PROPERTY: {"id":u32,"t":type,"v":value}.
type Prop struct {
	ID uint32
	T  uint8
	I  int64   // context, int32 (sign-extended), int64, datetime
	F  float64 // double, float
	B  []byte  // unicode (UTF-16BE incl. terminator), binary
}

// Number returns the property value as an int64 (doubles are truncated, toward zero).
func (p Prop) Number() int64 {
	switch p.T {
	case TypeDouble, TypeFloat:
		if math.IsNaN(p.F) {
			return 0
		}
		if p.F >= math.MaxInt64 {
			return math.MaxInt64
		}
		if p.F <= math.MinInt64 {
			return math.MinInt64
		}
		return int64(p.F)
	case TypeUnicode, TypeBinary, TypeNull:
		return 0
	}
	return p.I
}

// MarshalJSON writes the wire form.
func (p Prop) MarshalJSON() ([]byte, error) {
	var b bytes.Buffer
	fmt.Fprintf(&b, `{"id":%d,"t":%d,"v":`, p.ID, p.T)
	switch p.T {
	case TypeDouble, TypeFloat:
		v, err := json.Marshal(p.F)
		if err != nil {
			return nil, err
		}
		b.Write(v)
	case TypeUnicode, TypeBinary:
		v, _ := json.Marshal(base64.StdEncoding.EncodeToString(p.B))
		b.Write(v)
	case TypeNull:
		b.WriteString("0")
	default:
		b.WriteString(strconv.FormatInt(p.I, 10))
	}
	b.WriteByte('}')
	return b.Bytes(), nil
}

// UnmarshalJSON parses and validates the wire form. Numbers are decoded exactly.
func (p *Prop) UnmarshalJSON(data []byte) error {
	var raw struct {
		ID *json.Number    `json:"id"`
		T  *json.Number    `json:"t"`
		V  json.RawMessage `json:"v"`
	}
	if err := decodeStrict(data, &raw); err != nil {
		return err
	}
	if raw.ID == nil || raw.T == nil || len(raw.V) == 0 {
		return errors.New("property needs id, t and v")
	}
	id, err := parseU32(*raw.ID)
	if err != nil {
		return fmt.Errorf("property id: %w", err)
	}
	t, err := strconv.ParseUint(raw.T.String(), 10, 8)
	if err != nil || t > TypeDateTime {
		return errors.New("property type out of range")
	}
	out := Prop{ID: id, T: uint8(t)}
	switch out.T {
	case TypeUnicode, TypeBinary:
		var s string
		if err := json.Unmarshal(raw.V, &s); err != nil {
			return errors.New("property value must be base64")
		}
		if base64.StdEncoding.DecodedLen(len(s)) > maxDataBytes+3 {
			return errors.New("property value too large")
		}
		b, err := base64.StdEncoding.DecodeString(s)
		if err != nil || len(b) > maxDataBytes {
			return errors.New("property value must be base64 of at most 1024 bytes")
		}
		out.B = b
	default:
		var n json.Number
		if err := decodeStrict(raw.V, &n); err != nil {
			return errors.New("property value must be a number")
		}
		switch out.T {
		case TypeDouble, TypeFloat:
			f, err := strconv.ParseFloat(n.String(), 64)
			if err != nil || math.IsInf(f, 0) {
				return errors.New("property value is not a finite number")
			}
			if out.T == TypeFloat {
				f = float64(float32(f))
			}
			out.F = f
		case TypeInt32:
			v, err := parseInt(n, 32)
			if err != nil {
				return err
			}
			out.I = int64(int32(uint32(v))) // accepts -2^31..2^32-1, stores sign-extended
		default: // context, int64, datetime
			v, err := parseInt(n, 64)
			if err != nil {
				return err
			}
			out.I = v
		}
	}
	*p = out
	return nil
}

// parseInt accepts a signed or unsigned integer of the given width and returns its bits as
// int64 (an unsigned value above the signed range wraps, as in guest memory).
func parseInt(n json.Number, bits int) (int64, error) {
	s := n.String()
	if v, err := strconv.ParseInt(s, 10, bits); err == nil {
		return v, nil
	}
	if u, err := strconv.ParseUint(s, 10, bits); err == nil {
		return int64(u), nil
	}
	return 0, fmt.Errorf("integer %q out of range", s)
}

func parseU32(n json.Number) (uint32, error) {
	u, err := strconv.ParseUint(n.String(), 10, 32)
	if err != nil {
		return 0, fmt.Errorf("%q is not a u32", n.String())
	}
	return uint32(u), nil
}

// Context is an XUSER_CONTEXT: [id, value].
type Context struct {
	ID    uint32
	Value uint32
}

// MarshalJSON writes [id, value].
func (c Context) MarshalJSON() ([]byte, error) {
	return []byte(fmt.Sprintf("[%d,%d]", c.ID, c.Value)), nil
}

// UnmarshalJSON parses [id, value] with both values u32.
func (c *Context) UnmarshalJSON(data []byte) error {
	var pair []json.Number
	if err := decodeStrict(data, &pair); err != nil || len(pair) != 2 {
		return errors.New("context must be [id, value]")
	}
	id, err := parseU32(pair[0])
	if err != nil {
		return err
	}
	v, err := parseInt(pair[1], 32)
	if err != nil {
		return err
	}
	*c = Context{ID: id, Value: uint32(v)}
	return nil
}

// decodeStrict decodes one JSON value with UseNumber and rejects trailing data.
func decodeStrict(data []byte, v any) error {
	dec := json.NewDecoder(bytes.NewReader(data))
	dec.UseNumber()
	if err := dec.Decode(v); err != nil {
		return err
	}
	if dec.More() {
		return errors.New("trailing data")
	}
	return nil
}

func validateProps(ps []Prop) error {
	if len(ps) > maxProps {
		return fmt.Errorf("at most %d properties", maxProps)
	}
	return nil
}

func validateContexts(cs []Context) error {
	if len(cs) > maxContexts {
		return fmt.Errorf("at most %d contexts", maxContexts)
	}
	return nil
}
