//go:build !unix

package main

func selfCPU() float64 { return 0 }
