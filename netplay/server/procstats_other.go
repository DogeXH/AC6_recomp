//go:build !unix

package main

func rusage() (float64, uint64) { return 0, 0 }
