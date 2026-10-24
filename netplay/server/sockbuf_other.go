//go:build !unix

package main

import "net"

func sockBufs(conn *net.UDPConn) (rcv, snd int) { return 0, 0 }
