/*
SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
SPDX-License-Identifier: MIT
*/

package post

import (
	"context"
	"stackChan/api/post/v1"
	"stackChan/internal/dao"
	"stackChan/internal/model"

	"github.com/gogf/gf/v2/errors/gcode"
	"github.com/gogf/gf/v2/errors/gerror"
	"github.com/gogf/gf/v2/frame/g"
)

func (c *ControllerV1) DeletePost(ctx context.Context, req *v1.DeletePostReq) (res *v1.DeletePostRes, err error) {
	mac := g.RequestFromCtx(ctx).GetCtxVar(model.Mac).String()
	if mac == "" {
		return nil, gerror.NewCode(gcode.CodeNotAuthorized, "device is not authenticated")
	}
	result, err := dao.DevicePost.Ctx(ctx).
		Where("id = ?", req.Id).
		Where("mac = ?", mac).
		Delete()
	if err != nil {
		return nil, err
	}
	affected, err := result.RowsAffected()
	if err != nil {
		return nil, err
	}
	if affected == 0 {
		return nil, gerror.NewCode(gcode.CodeNotFound, "post not found")
	}
	response := v1.DeletePostRes("Deletion successful")
	return &response, nil
}
