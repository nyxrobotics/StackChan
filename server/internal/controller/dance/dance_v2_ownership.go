/*
SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
SPDX-License-Identifier: MIT
*/

package dance

import (
	"context"
	"stackChan/internal/dao"
	"stackChan/internal/model"
	"stackChan/internal/model/entity"

	"github.com/gogf/gf/v2/errors/gcode"
	"github.com/gogf/gf/v2/errors/gerror"
	"github.com/gogf/gf/v2/frame/g"
)

func requireOwnedDevice(ctx context.Context, mac string) error {
	uid := g.RequestFromCtx(ctx).GetCtxVar(model.Uid).Int64()
	if uid == 0 {
		return gerror.NewCode(gcode.CodeNotAuthorized, "user is not authenticated")
	}

	count, err := dao.Device.Ctx(ctx).
		Where("mac = ?", mac).
		Where("uid = ?", uid).
		Count()
	if err != nil {
		return gerror.WrapCode(gcode.CodeDbOperationError, err, "failed to query device ownership")
	}
	if count == 0 {
		return gerror.NewCode(gcode.CodeNotAuthorized, "device not found or not owned by current user")
	}
	return nil
}

func requireOwnedDance(ctx context.Context, id int64) (string, error) {
	var dance entity.DeviceDance
	if err := dao.DeviceDance.Ctx(ctx).Where("id = ?", id).Scan(&dance); err != nil {
		return "", gerror.WrapCode(gcode.CodeDbOperationError, err, "failed to query dance")
	}
	if dance.Id == 0 {
		return "", gerror.NewCode(gcode.CodeNotFound, "dance not found")
	}
	if err := requireOwnedDevice(ctx, dance.Mac); err != nil {
		return "", err
	}
	return dance.Mac, nil
}
