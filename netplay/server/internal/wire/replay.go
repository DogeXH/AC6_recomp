package wire

// ReplayWindowBits is the size of the sliding window behind the highest counter seen.
const ReplayWindowBits = 1024

// ReplayWindow tracks the highest counter received with one key plus a 1024-bit window of
// older counters. Check before decrypting; Accept only after the packet authenticated.
// The zero value is ready to use.
type ReplayWindow struct {
	highest uint64
	bits    [ReplayWindowBits / 64]uint64 // bit i set: counter (highest - i) was seen
}

// Highest returns the highest accepted counter (0 before the first packet).
func (w *ReplayWindow) Highest() uint64 { return w.highest }

// Reset makes the window reject every counter up to and including highest, as if all of them
// had been seen. A new registration uses it so packets older than its HELLO never replay.
func (w *ReplayWindow) Reset(highest uint64) {
	w.highest = highest
	for i := range w.bits {
		w.bits[i] = ^uint64(0)
	}
}

// Check reports whether ctr is new: non-zero, not seen before and not older than the window.
func (w *ReplayWindow) Check(ctr uint64) bool {
	if ctr == 0 {
		return false
	}
	if ctr > w.highest {
		return true
	}
	age := w.highest - ctr
	if age >= ReplayWindowBits {
		return false
	}
	return w.bits[age/64]&(1<<(age%64)) == 0
}

// Accept records ctr as seen. It returns true when ctr became the new highest counter.
// Call it only for a counter that passed Check and whose packet authenticated.
func (w *ReplayWindow) Accept(ctr uint64) bool {
	if ctr > w.highest {
		shift := ctr - w.highest
		if w.highest == 0 || shift >= ReplayWindowBits {
			w.bits = [ReplayWindowBits / 64]uint64{}
		} else {
			w.shift(shift)
		}
		w.highest = ctr
		w.bits[0] |= 1
		return true
	}
	age := w.highest - ctr
	if age < ReplayWindowBits {
		w.bits[age/64] |= 1 << (age % 64)
	}
	return false
}

// shift moves every bit n places towards older ages (0 < n < ReplayWindowBits).
func (w *ReplayWindow) shift(n uint64) {
	words := int(n / 64)
	bits := n % 64
	const count = ReplayWindowBits / 64
	for i := count - 1; i >= 0; i-- {
		var v uint64
		if src := i - words; src >= 0 {
			v = w.bits[src] << bits
			if bits != 0 && src-1 >= 0 {
				v |= w.bits[src-1] >> (64 - bits)
			}
		}
		w.bits[i] = v
	}
}
