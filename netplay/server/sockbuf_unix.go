//go:build unix

package main

import (
	"net"
	"syscall"
)

// sockBufs returns the kernel's effective receive and send buffer sizes of conn (0 if unknown).
// Linux reports twice the requested value (its bookkeeping overhead) and caps requests at
// net.core.rmem_max / wmem_max.
func sockBufs(conn *net.UDPConn) (rcv, snd int) {
	raw, err := conn.SyscallConn()
	if err != nil {
		return 0, 0
	}
	raw.Control(func(fd uintptr) {
		rcv, _ = syscall.GetsockoptInt(int(fd), syscall.SOL_SOCKET, syscall.SO_RCVBUF)
		snd, _ = syscall.GetsockoptInt(int(fd), syscall.SOL_SOCKET, syscall.SO_SNDBUF)
	})
	return rcv, snd
}
