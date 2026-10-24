package lobby

import (
	"time"
)

// bucket is a token bucket: `rate` tokens per second, at most `burst` stored.
type bucket struct {
	tokens float64
	last   time.Time
	rate   float64
	burst  float64
}

func newBucket(rate, burst float64, now time.Time) *bucket {
	return &bucket{tokens: burst, last: now, rate: rate, burst: burst}
}

func (b *bucket) refill(now time.Time) {
	if dt := now.Sub(b.last).Seconds(); dt > 0 {
		b.tokens = min(b.burst, b.tokens+dt*b.rate)
	}
	b.last = now
}

// allow takes n tokens if available.
func (b *bucket) allow(n float64, now time.Time) bool {
	b.refill(now)
	if b.tokens < n {
		return false
	}
	b.tokens -= n
	return true
}

// has reports whether n tokens are available without taking them.
func (b *bucket) has(n float64, now time.Time) bool {
	b.refill(now)
	return b.tokens >= n
}

// full reports whether the bucket has refilled completely (the key has been idle).
func (b *bucket) full(now time.Time) bool {
	b.refill(now)
	return b.tokens >= b.burst
}

// keyedLimiter holds one bucket per key with a bound on the number of keys. When the map is
// full and idle keys cannot be swept, new keys are refused (fail closed).
type keyedLimiter[K comparable] struct {
	rate, burst float64
	maxKeys     int
	m           map[K]*bucket
}

func newKeyedLimiter[K comparable](rate, burst float64, maxKeys int) *keyedLimiter[K] {
	return &keyedLimiter[K]{rate: rate, burst: burst, maxKeys: maxKeys, m: map[K]*bucket{}}
}

func (l *keyedLimiter[K]) allow(k K, n float64, now time.Time) bool {
	b := l.m[k]
	if b == nil {
		if len(l.m) >= l.maxKeys {
			l.sweep(now)
			if len(l.m) >= l.maxKeys {
				return false
			}
		}
		b = newBucket(l.rate, l.burst, now)
		l.m[k] = b
	}
	return b.allow(n, now)
}

// sweep drops buckets that are full again (their key has been idle long enough).
func (l *keyedLimiter[K]) sweep(now time.Time) {
	for k, b := range l.m {
		if b.full(now) {
			delete(l.m, k)
		}
	}
}
