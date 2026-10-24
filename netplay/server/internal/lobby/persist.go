package lobby

import (
	"encoding/json"
	"log/slog"
	"os"
	"path/filepath"
	"sync"
)

// WriteFileAtomic writes data to path through a temporary file, fsync and rename, so a crash
// leaves either the old or the new file.
func WriteFileAtomic(path string, data []byte, perm os.FileMode) error {
	dir := filepath.Dir(path)
	tmp, err := os.CreateTemp(dir, "."+filepath.Base(path)+".tmp*")
	if err != nil {
		return err
	}
	tmpName := tmp.Name()
	ok := false
	defer func() {
		if !ok {
			tmp.Close()
			os.Remove(tmpName)
		}
	}()
	if err := tmp.Chmod(perm); err != nil {
		return err
	}
	if _, err := tmp.Write(data); err != nil {
		return err
	}
	if err := tmp.Sync(); err != nil {
		return err
	}
	if err := tmp.Close(); err != nil {
		return err
	}
	if err := os.Rename(tmpName, path); err != nil {
		return err
	}
	ok = true
	if d, err := os.Open(dir); err == nil {
		d.Sync() // best effort: persist the rename
		d.Close()
	}
	return nil
}

// jsonWriter writes snapshots of one file on its own goroutine. Only the latest pending
// snapshot is written; older ones are skipped. After each attempt it calls onWritten (if set)
// with the snapshot's generation and the write error.
type jsonWriter struct {
	path      string
	log       *slog.Logger
	onWritten func(gen uint64, err error)

	mu      sync.Mutex
	pending any
	gen     uint64
	has     bool
	wake    chan struct{}
	done    chan struct{}
	closed  bool
}

func newJSONWriter(path string, log *slog.Logger, onWritten func(uint64, error)) *jsonWriter {
	w := &jsonWriter{path: path, log: log, onWritten: onWritten, wake: make(chan struct{}, 1),
		done: make(chan struct{})}
	go w.run()
	return w
}

// submit queues v (a value that json.Marshal can encode and that nobody mutates afterwards)
// as generation gen.
func (w *jsonWriter) submit(v any, gen uint64) {
	w.mu.Lock()
	defer w.mu.Unlock()
	if w.closed {
		return
	}
	w.pending, w.gen, w.has = v, gen, true
	select { // under mu, so close() cannot close wake concurrently
	case w.wake <- struct{}{}:
	default:
	}
}

func (w *jsonWriter) run() {
	defer close(w.done)
	for range w.wake {
		w.flushPending()
	}
	w.flushPending()
}

func (w *jsonWriter) flushPending() {
	w.mu.Lock()
	v, gen, has := w.pending, w.gen, w.has
	w.pending, w.has = nil, false
	w.mu.Unlock()
	if !has {
		return
	}
	data, err := json.MarshalIndent(v, "", " ")
	if err == nil {
		err = WriteFileAtomic(w.path, append(data, '\n'), 0o600)
	}
	if err != nil {
		w.log.Error("persist failed", "file", filepath.Base(w.path), "err", err)
	}
	if w.onWritten != nil {
		w.onWritten(gen, err)
	}
}

// close writes any pending snapshot and stops the goroutine.
func (w *jsonWriter) close() {
	w.mu.Lock()
	if w.closed {
		w.mu.Unlock()
		return
	}
	w.closed = true
	w.mu.Unlock()
	close(w.wake)
	<-w.done
}
