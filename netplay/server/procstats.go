package main

import (
	"os"
	"runtime"
	"strconv"
	"strings"
)

// procStats are process figures for the admin metrics and the stats log line.
type procStats struct {
	CPUSeconds  float64 `json:"cpu_seconds"`   // user + system CPU time used so far
	RSSBytes    uint64  `json:"rss_bytes"`     // resident set size now (Linux), else the peak
	MaxRSSBytes uint64  `json:"max_rss_bytes"` // peak resident set size
	HeapBytes   uint64  `json:"heap_bytes"`    // Go heap in use
	SysBytes    uint64  `json:"sys_bytes"`     // memory obtained from the OS by the Go runtime
	Goroutines  int     `json:"goroutines"`
	NumGC       uint32  `json:"num_gc"`
	GoVersion   string  `json:"go_version"`
	GOMAXPROCS  int     `json:"gomaxprocs"`
}

func readProcStats() procStats {
	var ms runtime.MemStats
	runtime.ReadMemStats(&ms)
	p := procStats{HeapBytes: ms.HeapInuse, SysBytes: ms.Sys, Goroutines: runtime.NumGoroutine(),
		NumGC: ms.NumGC, GoVersion: runtime.Version(), GOMAXPROCS: runtime.GOMAXPROCS(0)}
	p.CPUSeconds, p.MaxRSSBytes = rusage()
	p.RSSBytes = currentRSS()
	if p.RSSBytes == 0 {
		p.RSSBytes = p.MaxRSSBytes
	}
	return p
}

// currentRSS reads /proc/self/statm (Linux); 0 elsewhere.
func currentRSS() uint64 {
	b, err := os.ReadFile("/proc/self/statm")
	if err != nil {
		return 0
	}
	f := strings.Fields(string(b))
	if len(f) < 2 {
		return 0
	}
	pages, err := strconv.ParseUint(f[1], 10, 64)
	if err != nil {
		return 0
	}
	return pages * uint64(os.Getpagesize())
}
