/*
SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
SPDX-License-Identifier: MIT
*/

package device

import (
	"context"
	"errors"
	"stackChan/internal/dao"
	"stackChan/internal/model"
	"stackChan/internal/model/do"
	"stackChan/internal/service"

	"github.com/gogf/gf/v2/database/gdb"
	"github.com/gogf/gf/v2/errors/gcode"
	"github.com/gogf/gf/v2/errors/gerror"
	"github.com/gogf/gf/v2/frame/g"
	"github.com/gogf/gf/v2/os/gtime"

	"stackChan/api/device/v2"
)

var errDeviceAlreadyBound = errors.New("device is already bound to another user")

// BindDevice Device binding interface
func (c *ControllerV2) BindDevice(ctx context.Context, req *v2.BindDeviceReq) (res *v2.BindDeviceRes, err error) {
	uid := g.RequestFromCtx(ctx).GetCtxVar(model.Uid).Int64()
	if uid == 0 {
		return nil, gerror.NewCode(gcode.CodeMissingParameter, "User UID cannot be empty")
	}
	if req.Mac == "" {
		return nil, gerror.NewCode(gcode.CodeMissingParameter, "Device MAC address cannot be empty")
	}

	err = g.DB().Transaction(ctx, func(ctx context.Context, tx gdb.TX) error {
		if err = service.CreateMacIfNotExistsWithTx(ctx, tx, req.Mac); err != nil {
			return err
		}

		result, updateErr := dao.Device.Ctx(ctx).TX(tx).
			Where("mac = ?", req.Mac).
			Where("(uid IS NULL OR uid = 0 OR uid = ?)", uid).
			Data(do.Device{
				Uid:      uid,
				BindTime: gtime.Now().Format("Y-m-d H:i:s"),
			}).
			Update()
		if updateErr != nil {
			return updateErr
		}
		affected, updateErr := result.RowsAffected()
		if updateErr != nil {
			return updateErr
		}
		if affected == 0 {
			return errDeviceAlreadyBound
		}
		return nil
	})
	if err != nil {
		if errors.Is(err, errDeviceAlreadyBound) {
			return nil, gerror.NewCode(gcode.CodeNotAuthorized, errDeviceAlreadyBound.Error())
		}
		return nil, gerror.WrapCode(gcode.CodeDbOperationError, err, "Device binding failed")
	}
	return new(v2.BindDeviceRes(true)), nil
}
