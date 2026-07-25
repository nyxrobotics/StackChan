/*
SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
SPDX-License-Identifier: MIT
*/

package service

import (
	"context"
	"fmt"
	"io"
	"path/filepath"
	v1 "stackChan/api/file/v1"
	"stackChan/internal/filesafe"

	"github.com/gogf/gf/v2/errors/gcode"
	"github.com/gogf/gf/v2/errors/gerror"
)

const maxUploadFileSize int64 = 100 * 1024 * 1024

func AddFile(ctx context.Context, req *v1.FileReq) (res *v1.FileRes, err error) {
	if req == nil || req.File == nil || req.Name == "" {
		return nil, gerror.NewCode(gcode.CodeInvalidParameter, "file or filename is empty")
	}
	if req.File.Size <= 0 || req.File.Size > maxUploadFileSize {
		return nil, gerror.NewCodef(
			gcode.CodeInvalidParameter,
			"file size must be between 1 and %d bytes",
			maxUploadFileSize,
		)
	}

	fileName, err := filesafe.CleanRelativePath(req.Name)
	if err != nil || fileName == "." || filepath.Base(fileName) != fileName {
		return nil, gerror.NewCode(gcode.CodeInvalidParameter, "invalid filename")
	}
	directory, err := filesafe.CleanRelativePath(req.Directory)
	if err != nil {
		return nil, gerror.NewCode(gcode.CodeInvalidParameter, "invalid upload directory")
	}
	targetPath, err := filesafe.CleanRelativePath(directory, fileName)
	if err != nil {
		return nil, gerror.NewCode(gcode.CodeInvalidParameter, "invalid upload path")
	}

	fileRoot, err := filesafe.OpenOrCreate(filesafe.DefaultRootDirectory, 0o755)
	if err != nil {
		return nil, err
	}
	defer fileRoot.Close()

	if err = fileRoot.MkdirAll(directory, 0o755); err != nil {
		return nil, err
	}

	source, err := req.File.Open()
	if err != nil {
		return nil, err
	}
	defer source.Close()

	temporaryFile, temporaryPath, err := fileRoot.CreateTemp(directory)
	if err != nil {
		return nil, err
	}
	temporaryClosed := false
	renamed := false
	defer func() {
		if !temporaryClosed {
			_ = temporaryFile.Close()
		}
		if !renamed {
			_ = fileRoot.Remove(temporaryPath)
		}
	}()

	written, err := io.Copy(temporaryFile, io.LimitReader(source, maxUploadFileSize+1))
	if err != nil {
		return nil, err
	}
	if written <= 0 || written > maxUploadFileSize {
		return nil, gerror.NewCode(gcode.CodeInvalidParameter, "uploaded file exceeds the size limit")
	}
	if written != req.File.Size {
		return nil, gerror.NewCode(gcode.CodeInvalidParameter, "uploaded file size does not match its metadata")
	}
	if err = temporaryFile.Chmod(0o644); err != nil {
		return nil, err
	}
	if err = temporaryFile.Sync(); err != nil {
		return nil, err
	}
	if err = temporaryFile.Close(); err != nil {
		return nil, err
	}
	temporaryClosed = true

	if err = fileRoot.Rename(temporaryPath, targetPath); err != nil {
		return nil, fmt.Errorf("publish uploaded file: %w", err)
	}
	renamed = true

	return &v1.FileRes{
		Path: filepath.ToSlash(filepath.Join(filesafe.DefaultRootDirectory, targetPath)),
	}, nil
}
