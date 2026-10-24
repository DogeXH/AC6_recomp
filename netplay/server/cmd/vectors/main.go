// Command vectors writes the deterministic ac6net crypto and packet test vectors.
//
//	go run ./cmd/vectors                 # writes the default path
//	go run ./cmd/vectors -o -            # prints to stdout
package main

import (
	"flag"
	"fmt"
	"os"
	"path/filepath"

	"ac6lobby/internal/vectors"
)

const defaultOut = "/Users/cole/ACE6-Recomp/ac6-online-20261019/notes/vectors.json"

func main() {
	out := flag.String("o", defaultOut, "output path, or - for stdout")
	flag.Parse()

	data := vectors.Marshal(vectors.Generate())
	if *out == "-" {
		os.Stdout.Write(data)
		return
	}
	tmp := *out + ".tmp"
	if err := os.WriteFile(tmp, data, 0o644); err != nil {
		fmt.Fprintln(os.Stderr, "vectors:", err)
		os.Exit(1)
	}
	if err := os.Rename(tmp, *out); err != nil {
		fmt.Fprintln(os.Stderr, "vectors:", err)
		os.Exit(1)
	}
	abs, _ := filepath.Abs(*out)
	fmt.Println("wrote", abs)
}
