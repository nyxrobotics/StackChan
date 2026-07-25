/*
SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
SPDX-License-Identifier: MIT
*/

package device

import (
	"context"
	"stackChan/api/device/v2"
	"stackChan/internal/dao"
	"stackChan/internal/model"
	"stackChan/internal/service"

	"github.com/gogf/gf/v2/errors/gcode"
	"github.com/gogf/gf/v2/errors/gerror"
	"github.com/gogf/gf/v2/frame/g"
)

func (c *ControllerV2) AgentRestoreDefault(ctx context.Context, req *v2.AgentRestoreDefaultReq) (res *v2.AgentRestoreDefaultRes, err error) {
	if req.Mac == "" {
		return nil, gerror.NewCode(gcode.CodeMissingParameter, "Device MAC address cannot be empty")
	}

	uid := g.RequestFromCtx(ctx).GetCtxVar(model.Uid).Int64()
	if uid == 0 {
		return nil, gerror.NewCode(gcode.CodeMissingParameter, "User UID cannot be empty")
	}
	owned, err := dao.Device.Ctx(ctx).
		Where("mac = ?", req.Mac).
		Where("uid = ?", uid).
		Count()
	if err != nil {
		return nil, gerror.WrapCode(gcode.CodeDbOperationError, err, "Failed to query device ownership")
	}
	if owned == 0 {
		return nil, gerror.NewCode(gcode.CodeNotAuthorized, "device not found or not owned by current user")
	}

	restoreResponse, err := service.RestoreDefaultAgent(req.Mac)
	if err != nil {
		return nil, err
	}
	if !restoreResponse {
		return nil, gerror.NewCode(gcode.CodeInternalError, "Failed to restore default configuration")
	}
	return new(v2.AgentRestoreDefaultRes(true)), nil
}
