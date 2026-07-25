/*
SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
SPDX-License-Identifier: MIT
*/

package filesafe

import (
	"errors"
	"os"
	"path/filepath"
	"testing"
)

func TestCleanRelativePathRejectsEscape(t *testing.T) {
	t.Parallel()

	invalid := []string{
		"../secret",
		`..\secret`,
		"/absolute",
		`C:\absolute`,
		"safe/../../secret",
	}
	for _, name := range invalid {
		name := name
		t.Run(name, func(t *testing.T) {
			t.Parallel()
			if _, err := CleanRelativePath(name); !errors.Is(err, ErrInvalidPath) {
				t.Fatalf("CleanRelativePath(%q) error = %v, want ErrInvalidPath", name, err)
			}
		})
	}

	got, err := CleanRelativePath("avatars", "cat.png")
	if err != nil {
		t.Fatalf("CleanRelativePath returned error: %v", err)
	}
	if want := filepath.Join("avatars", "cat.png"); got != want {
		t.Fatalf("CleanRelativePath result = %q, want %q", got, want)
	}
}

func TestRootRejectsSymlinkEscape(t *testing.T) {
	t.Parallel()

	parent := t.TempDir()
	rootDir := filepath.Join(parent, "root")
	if err := os.Mkdir(rootDir, 0o755); err != nil {
		t.Fatal(err)
	}
	outside := filepath.Join(parent, "outside.txt")
	if err := os.WriteFile(outside, []byte("secret"), 0o600); err != nil {
		t.Fatal(err)
	}
	if err := os.Symlink(outside, filepath.Join(rootDir, "escape")); err != nil {
		t.Skipf("symlink is unavailable: %v", err)
	}

	root, err := Open(rootDir)
	if err != nil {
		t.Fatal(err)
	}
	defer root.Close()

	if file, err := root.OpenFile("escape"); err == nil {
		file.Close()
		t.Fatal("Root.OpenFile followed a symlink outside the root")
	}
}
