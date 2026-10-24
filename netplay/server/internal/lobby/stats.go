package lobby

import (
	"cmp"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"slices"
	"time"
)

// Stats limits.
const (
	maxBoards        = 4096 // distinct (title, view) leaderboards
	maxColumnsPerRow = 64
)

// System column ids (XLast attribute ids).
const (
	columnRank      uint16 = 65535
	columnRating    uint16 = 65534
	columnGamername uint16 = 65533
)

type aggregation uint8

const (
	aggLast   aggregation = iota // replace
	aggMax                       // keep the larger value
	aggSystem                    // computed by the server; writes are ignored
)

type columnSpec struct {
	col uint16
	agg aggregation
}

// titleColumns maps a title's stats property ids to leaderboard columns. AC6's 102 views share
// one layout (from its SPA; see notes/ac6_spa_summary.json). Properties that are not listed
// fall back to column = property id & 0xFFFF with aggregation "last".
var titleColumns = map[string]map[uint32]columnSpec{
	"4E4D07D1": {
		0x10008001: {columnRank, aggSystem},
		0x40008002: {columnGamername, aggSystem},
		0x20000008: {columnRating, aggMax}, // "Ranked Points"
		0x10000009: {2, aggLast},           // nation ID
		0x20000006: {3, aggMax},            // score
		0x2000000B: {1, aggLast},           // xuid
	},
}

func columnFor(title string, propID uint32) columnSpec {
	if spec, ok := titleColumns[title][propID]; ok {
		return spec
	}
	return columnSpec{uint16(propID), aggLast}
}

type boardKey struct {
	Title string
	View  uint32
}

type statsRow struct {
	XUID    uint64
	Cols    map[uint16]Prop // by column id; Prop.ID keeps the written property id
	Updated time.Time
	Seq     uint64 // write sequence of the last change to a stored value (rank tie-break)
	rating  int64  // valid while the board is sorted
}

type statsBoard struct {
	rows    map[uint64]*statsRow
	sorted  []*statsRow // rank order, rebuilt lazily
	rankCol uint16
	stale   bool
}

type statsStore struct {
	boards map[boardKey]*statsBoard
	seq    uint64
	dirty  bool
}

func newStatsStore() *statsStore { return &statsStore{boards: map[boardKey]*statsBoard{}} }

var errTooManyBoards = errors.New("too many leaderboards")

func (s *statsStore) row(title string, view uint32, xuid uint64) (*statsBoard, *statsRow, error) {
	k := boardKey{title, view}
	b := s.boards[k]
	if b == nil {
		if len(s.boards) >= maxBoards {
			return nil, nil, errTooManyBoards
		}
		b = &statsBoard{rows: map[uint64]*statsRow{}}
		s.boards[k] = b
	}
	r := b.rows[xuid]
	if r == nil {
		r = &statsRow{XUID: xuid, Cols: map[uint16]Prop{}}
		b.rows[xuid] = r
	}
	return b, r, nil
}

// write applies one XSessionWriteStats view for xuid, mapping properties to columns and
// aggregating per column (last or max).
func (s *statsStore) write(title string, view uint32, xuid uint64, props []Prop, now time.Time) error {
	b, r, err := s.row(title, view, xuid)
	if err != nil {
		return err
	}
	changed := false
	for _, p := range props {
		spec := columnFor(title, p.ID)
		if spec.agg == aggSystem {
			continue
		}
		old, has := r.Cols[spec.col]
		if !has && len(r.Cols) >= maxColumnsPerRow {
			continue
		}
		if has && spec.agg == aggMax && !greater(p, old) {
			continue
		}
		if has && propEqual(p, old) {
			continue
		}
		r.Cols[spec.col] = p
		changed = true
	}
	r.Updated = now.UTC().Truncate(time.Second)
	if changed {
		s.seq++
		r.Seq = s.seq
		b.stale = true
	}
	s.dirty = true
	return nil
}

// greater compares numeric values (doubles exactly, others as int64).
func greater(a, b Prop) bool {
	if (a.T == TypeDouble || a.T == TypeFloat) && (b.T == TypeDouble || b.T == TypeFloat) {
		return a.F > b.F
	}
	return a.Number() > b.Number()
}

func propEqual(a, b Prop) bool {
	return a.T == b.T && a.I == b.I && a.F == b.F && string(a.B) == string(b.B)
}

// ranked returns the board's rows in rank order: rating descending, then the earlier change
// first, then XUID. The rating is column 65534 when the board has it, otherwise the lowest
// column id ("first column").
func (b *statsBoard) ranked() []*statsRow {
	if !b.stale && b.sorted != nil {
		return b.sorted
	}
	hasRating, lowest := false, uint16(0xFFFF)
	for _, r := range b.rows {
		for col := range r.Cols {
			hasRating = hasRating || col == columnRating
			lowest = min(lowest, col)
		}
	}
	b.rankCol = lowest
	if hasRating {
		b.rankCol = columnRating
	}
	b.sorted = b.sorted[:0]
	for _, r := range b.rows {
		r.rating = 0
		if p, ok := r.Cols[b.rankCol]; ok {
			r.rating = p.Number()
		}
		b.sorted = append(b.sorted, r)
	}
	slices.SortFunc(b.sorted, func(x, y *statsRow) int {
		if c := cmp.Compare(y.rating, x.rating); c != 0 {
			return c
		}
		if c := cmp.Compare(x.Seq, y.Seq); c != 0 {
			return c
		}
		return cmp.Compare(x.XUID, y.XUID)
	})
	b.stale = false
	return b.sorted
}

// statsRowDoc and statsViewDoc are the wire forms (PROTOCOL.md 3.1 StatsView).
type statsRowDoc struct {
	XUID     string `json:"xuid"`
	Gamertag string `json:"gamertag"`
	Rank     int    `json:"rank"`
	Rating   int64  `json:"rating"`
	Columns  []Prop `json:"columns"`
}

type statsViewDoc struct {
	ViewID    uint32            `json:"view_id"`
	TotalRows int               `json:"total_rows"`
	Rows      []json.RawMessage `json:"rows"`
}

// rowDoc renders a row with exactly the requested columns, in order. Column 65535 is the
// rank, 65533 the gamertag (UTF-16BE with terminator); a missing column is
// {"id":c,"t":255,"v":0}. The board must be sorted (rank and rating valid).
func rowDoc(r *statsRow, rank int, gamertag string, columns []uint16) statsRowDoc {
	d := statsRowDoc{XUID: hex16(r.XUID), Gamertag: gamertag, Rank: rank, Rating: r.rating,
		Columns: make([]Prop, 0, len(columns))}
	for _, c := range columns {
		p, ok := r.Cols[c]
		switch {
		case ok:
		case c == columnRank:
			p = Prop{T: TypeInt32, I: int64(rank)}
		case c == columnGamername:
			p = Prop{T: TypeUnicode, B: utf16BE(gamertag)}
		default:
			p = Prop{T: TypeNull}
		}
		p.ID = uint32(c)
		d.Columns = append(d.Columns, p)
	}
	return d
}

// utf16BE encodes an ASCII gamertag as UTF-16BE plus a terminator.
func utf16BE(s string) []byte {
	out := make([]byte, 0, 2*len(s)+2)
	for _, r := range s {
		if r > 0xFFFF {
			r = '?'
		}
		out = append(out, byte(r>>8), byte(r))
	}
	return append(out, 0, 0)
}

// --- persistence ---

type statsFileCol struct {
	Col  uint16 `json:"col"`
	Prop Prop   `json:"prop"`
}

type statsFileRow struct {
	XUID    string         `json:"xuid"`
	Updated int64          `json:"updated_unix"`
	Seq     uint64         `json:"seq"`
	Columns []statsFileCol `json:"columns"`
}

type statsFileBoard struct {
	Title string         `json:"title"`
	View  uint32         `json:"view_id"`
	Rows  []statsFileRow `json:"rows"`
}

type statsFile struct {
	Version int              `json:"version"`
	Boards  []statsFileBoard `json:"boards"`
}

func (s *statsStore) snapshot() statsFile {
	f := statsFile{Version: 2, Boards: make([]statsFileBoard, 0, len(s.boards))}
	for k, b := range s.boards {
		fb := statsFileBoard{Title: k.Title, View: k.View, Rows: make([]statsFileRow, 0, len(b.rows))}
		for _, r := range b.rows {
			row := statsFileRow{XUID: hex16(r.XUID), Updated: r.Updated.Unix(), Seq: r.Seq,
				Columns: make([]statsFileCol, 0, len(r.Cols))}
			for c, p := range r.Cols {
				row.Columns = append(row.Columns, statsFileCol{c, p})
			}
			slices.SortFunc(row.Columns, func(x, y statsFileCol) int { return cmp.Compare(x.Col, y.Col) })
			fb.Rows = append(fb.Rows, row)
		}
		slices.SortFunc(fb.Rows, func(x, y statsFileRow) int { return cmp.Compare(x.XUID, y.XUID) })
		f.Boards = append(f.Boards, fb)
	}
	slices.SortFunc(f.Boards, func(x, y statsFileBoard) int {
		if c := cmp.Compare(x.Title, y.Title); c != 0 {
			return c
		}
		return cmp.Compare(x.View, y.View)
	})
	return f
}

func (s *statsStore) load(path string) error {
	data, err := os.ReadFile(path)
	if errors.Is(err, os.ErrNotExist) {
		return nil
	}
	if err != nil {
		return err
	}
	var f statsFile
	if err := json.Unmarshal(data, &f); err != nil {
		return fmt.Errorf("%s: %w", path, err)
	}
	if f.Version != 2 {
		return fmt.Errorf("%s: unsupported version %d", path, f.Version)
	}
	for _, fb := range f.Boards {
		if !validTitle(fb.Title) {
			return fmt.Errorf("%s: bad title %q", path, fb.Title)
		}
		for _, fr := range fb.Rows {
			xuid, ok := parseHex16(fr.XUID)
			if !ok || len(fr.Columns) > maxColumnsPerRow {
				return fmt.Errorf("%s: bad row %q", path, fr.XUID)
			}
			b, r, err := s.row(fb.Title, fb.View, xuid)
			if err != nil {
				return fmt.Errorf("%s: %w", path, err)
			}
			for _, c := range fr.Columns {
				r.Cols[c.Col] = c.Prop
			}
			r.Updated, r.Seq = time.Unix(fr.Updated, 0).UTC(), fr.Seq
			s.seq = max(s.seq, fr.Seq)
			b.stale = true
		}
	}
	s.dirty = false
	return nil
}

// removeXUID deletes every leaderboard row of xuid and returns how many it removed.
func (s *statsStore) removeXUID(xuid uint64) int {
	n := 0
	for _, b := range s.boards {
		if _, ok := b.rows[xuid]; ok {
			delete(b.rows, xuid)
			b.stale = true
			n++
		}
	}
	if n > 0 {
		s.dirty = true
	}
	return n
}
