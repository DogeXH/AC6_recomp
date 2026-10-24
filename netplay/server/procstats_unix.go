//go:build unix

package main

import (
	"runtime"
	"syscall"
)

// rusage returns the CPU seconds used and the peak RSS in bytes.
func rusage() (float64, uint64) {
	var ru syscall.Rusage
	if syscall.Getrusage(syscall.RUSAGE_SELF, &ru) != nil {
		return 0, 0
	}
	cpu := float64(ru.Utime.Sec+ru.Stime.Sec) + float64(ru.Utime.Usec+ru.Stime.Usec)/1e6
	maxrss := uint64(ru.Maxrss)
	if runtime.GOOS != "darwin" {
		maxrss *= 1024 // Linux and the BSDs report KiB; macOS reports bytes
	}
	return cpu, maxrss
}
