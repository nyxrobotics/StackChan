/*
SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
SPDX-License-Identifier: MIT
*/

package cmd

import (
	"context"
	"net/http"
	"stackChan/internal/boot"
	"stackChan/internal/controller/admin"
	"stackChan/internal/controller/appstore"
	"stackChan/internal/controller/dance"
	"stackChan/internal/controller/device"
	"stackChan/internal/controller/file"
	"stackChan/internal/controller/friend"
	"stackChan/internal/controller/pano"
	"stackChan/internal/controller/post"
	"stackChan/internal/controller/stackchandevice"
	"stackChan/internal/controller/user"
	"stackChan/internal/controller/xiaozhi"
	"stackChan/internal/filesafe"
	"stackChan/internal/middleware"
	"stackChan/internal/web_socket"

	"github.com/gogf/gf/v2/frame/g"
	"github.com/gogf/gf/v2/net/ghttp"
	"github.com/gogf/gf/v2/os/gcmd"
)

var (
	Main = gcmd.Command{
		Name:  "main",
		Usage: "main",
		Brief: "start http server",
		Func: func(ctx context.Context, parser *gcmd.Parser) (err error) {
			s := g.Server()
			s.SetClientMaxBodySize(100 * 1024 * 1024)

			s.Use(middleware.CORS)

			s.BindHandler("/stackChan/ws", web_socket.Handler)

			// heartBeat
			boot.InitCron()

			///Configuration file access
			s.Group("/file", func(group *ghttp.RouterGroup) {
				group.GET("/*filepath", func(r *ghttp.Request) {
					writeNotFound := func() {
						r.Response.WriteHeader(http.StatusNotFound)
						r.Response.Write("File not found")
					}

					relativePath := r.Get("filepath").String()
					if relativePath == "" {
						writeNotFound()
						return
					}

					fileRoot, err := filesafe.Open(filesafe.DefaultRootDirectory)
					if err != nil {
						writeNotFound()
						return
					}
					defer fileRoot.Close()

					servedFile, err := fileRoot.OpenFile(relativePath)
					if err != nil {
						writeNotFound()
						return
					}
					defer servedFile.Close()

					info, err := servedFile.Stat()
					if err != nil || info.IsDir() {
						writeNotFound()
						return
					}
					r.Response.ServeContent(info.Name(), info.ModTime(), servedFile)
				})
			})

			s.Group("/stackChan/v2", func(group *ghttp.RouterGroup) {
				group.Middleware(middleware.V2TokenAuthMiddleware, ghttp.MiddlewareHandlerResponse)
				group.Bind(user.NewV2(), dance.NewV2(), device.NewV2())
			})

			s.Group("/stackChan", func(group *ghttp.RouterGroup) {
				group.Middleware(middleware.TokenAuthMiddleware, ghttp.MiddlewareHandlerResponse)
				group.Bind(device.NewV1(), friend.NewV1(), dance.NewV1(), file.NewV1(), post.NewV1(), pano.NewV1(), appstore.NewV1(), xiaozhi.NewV1(), stackchandevice.NewV2())
			})

			s.Group("/admin/stackChan", func(group *ghttp.RouterGroup) {
				group.Middleware(middleware.AdminTokenAuthMiddleware, ghttp.MiddlewareHandlerResponse)
				group.Bind(admin.NewV1(), file.NewV1())
			})

			// Do not use SetServerRoot, globally only provide frontend entry via /web
			s.SetServerRoot("web/management")

			s.SetPort(12800)
			s.Run()
			return nil
		},
	}
)
