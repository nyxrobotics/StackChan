/*
SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
SPDX-License-Identifier: MIT
*/

package filesafe

import (
	"crypto/rand"
	"encoding/hex"
	"errors"
	"fmt"
	"io/fs"
	"os"
	"path"
	"path/filepath"
	"strings"
)

var ErrInvalidPath = errors.New("path must stay within the file root")

const DefaultRootDirectory = "file"

// Root wraps os.Root so all filesystem operations remain beneath one directory.
type Root struct {
	root *os.Root
}

// Open opens an existing filesystem root.
func Open(name string) (*Root, error) {
	root, err := os.OpenRoot(name)
	if err != nil {
		return nil, err
	}
	return &Root{root: root}, nil
}

// OpenOrCreate creates a filesystem root if needed and then opens it.
func OpenOrCreate(name string, perm os.FileMode) (*Root, error) {
	if err := os.MkdirAll(name, perm); err != nil {
		return nil, err
	}
	return Open(name)
}

func (r *Root) Close() error {
	return r.root.Close()
}

// CleanRelativePath joins untrusted path fragments and rejects traversal and
// absolute paths before they reach os.Root.
func CleanRelativePath(parts ...string) (string, error) {
	cleanParts := make([]string, 0, len(parts))
	for _, part := range parts {
		if part == "" {
			continue
		}

		normalized := strings.ReplaceAll(part, `\`, "/")
		if strings.HasPrefix(normalized, "/") ||
			filepath.IsAbs(part) ||
			filepath.VolumeName(part) != "" ||
			hasDrivePrefix(normalized) ||
			strings.IndexByte(normalized, 0) >= 0 {
			return "", ErrInvalidPath
		}

		for _, component := range strings.Split(normalized, "/") {
			if component == ".." {
				return "", ErrInvalidPath
			}
		}

		cleaned := path.Clean(normalized)
		if cleaned == "." {
			continue
		}
		if cleaned == ".." || strings.HasPrefix(cleaned, "../") {
			return "", ErrInvalidPath
		}
		cleanParts = append(cleanParts, cleaned)
	}

	if len(cleanParts) == 0 {
		return ".", nil
	}
	return filepath.FromSlash(path.Join(cleanParts...)), nil
}

func (r *Root) OpenFile(name string) (*os.File, error) {
	cleaned, err := CleanRelativePath(name)
	if err != nil {
		return nil, err
	}
	return r.root.Open(cleaned)
}

func (r *Root) MkdirAll(name string, perm os.FileMode) error {
	cleaned, err := CleanRelativePath(name)
	if err != nil {
		return err
	}
	return r.root.MkdirAll(cleaned, perm)
}

func (r *Root) Remove(name string) error {
	cleaned, err := CleanRelativePath(name)
	if err != nil {
		return err
	}
	return r.root.Remove(cleaned)
}

func (r *Root) Rename(oldName, newName string) error {
	cleanedOld, err := CleanRelativePath(oldName)
	if err != nil {
		return err
	}
	cleanedNew, err := CleanRelativePath(newName)
	if err != nil {
		return err
	}
	return r.root.Rename(cleanedOld, cleanedNew)
}

// CreateTemp creates an exclusive temporary file beneath directory. The caller
// is responsible for closing and removing it.
func (r *Root) CreateTemp(directory string) (*os.File, string, error) {
	cleanedDir, err := CleanRelativePath(directory)
	if err != nil {
		return nil, "", err
	}

	for range 100 {
		var randomBytes [16]byte
		if _, err = rand.Read(randomBytes[:]); err != nil {
			return nil, "", err
		}
		name := ".upload-" + hex.EncodeToString(randomBytes[:]) + ".tmp"
		relativeName := filepath.Join(cleanedDir, name)
		file, openErr := r.root.OpenFile(relativeName, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0o600)
		if openErr == nil {
			return file, relativeName, nil
		}
		if !errors.Is(openErr, fs.ErrExist) {
			return nil, "", openErr
		}
	}
	return nil, "", fmt.Errorf("create temporary upload file: %w", fs.ErrExist)
}

func hasDrivePrefix(name string) bool {
	return len(name) >= 2 &&
		((name[0] >= 'a' && name[0] <= 'z') || (name[0] >= 'A' && name[0] <= 'Z')) &&
		name[1] == ':'
}
