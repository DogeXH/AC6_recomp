package lobby

import (
	"os"
	"path/filepath"
	"testing"
	"time"
)

// A stats write that fails (here: the data dir is read-only, like a full disk) is retried on a
// later persist tick instead of leaving the stats only in memory (seen 2026-10-23: the Mac's
// disk filled up during a ranked match and stats.json kept the old rows).
func TestStatsPersistRetriedAfterFailedWrite(t *testing.T) {
	if os.Geteuid() == 0 {
		t.Skip("root ignores directory permissions")
	}
	dir := t.TempDir()
	ts := startServer(t, withData(dir))
	c := ts.newClient("P")
	waitForFile(t, filepath.Join(dir, "accounts.json"))
	if err := os.Chmod(dir, 0o500); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { os.Chmod(dir, 0o700) })
	createSessionAndWrite(t, c, 100)
	time.Sleep(statsDebounce + 2*time.Second) // the first attempt has failed by now
	if _, err := os.Stat(filepath.Join(dir, "stats.json")); err == nil {
		t.Fatal("stats.json written into a read-only dir")
	}
	if err := os.Chmod(dir, 0o700); err != nil {
		t.Fatal(err)
	}
	waitForFile(t, filepath.Join(dir, "stats.json"))
}
