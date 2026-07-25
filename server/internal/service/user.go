/*
SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
SPDX-License-Identifier: MIT
*/

package service

import (
	"context"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"strings"
	"time"

	v2 "stackChan/api/user/v2"
	"stackChan/internal/dao"
	"stackChan/internal/model"
	"stackChan/internal/model/entity"

	"github.com/gogf/gf/v2/container/gvar"
	"github.com/gogf/gf/v2/errors/gcode"
	"github.com/gogf/gf/v2/errors/gerror"
	"github.com/gogf/gf/v2/frame/g"
	"github.com/gogf/gf/v2/util/guid"
	"github.com/golang-jwt/jwt/v5"
)

const (
	TokenExpire                 = 365 * 24 * time.Hour
	remoteUserRequestTimeout    = 10 * time.Second
	remoteUserResponseBodyLimit = 1 << 20
)

var (
	errRemoteUserResponseTooLarge = errors.New("remote user response exceeds size limit")
	remoteUserHTTPClient          = newRemoteUserHTTPClient(remoteUserRequestTimeout)
)

func newRemoteUserHTTPClient(timeout time.Duration) *http.Client {
	return &http.Client{
		Timeout: timeout,
		CheckRedirect: func(_ *http.Request, _ []*http.Request) error {
			return http.ErrUseLastResponse
		},
	}
}

type remoteUserHTTPStatusError struct {
	statusCode int
	status     string
}

func (e *remoteUserHTTPStatusError) Error() string {
	return fmt.Sprintf("remote user service returned unexpected HTTP status %s", e.status)
}

func postRemoteUserForm(
	ctx context.Context,
	client *http.Client,
	endpoint string,
	headers http.Header,
	form url.Values,
) ([]byte, error) {
	request, err := http.NewRequestWithContext(ctx, http.MethodPost, endpoint, strings.NewReader(form.Encode()))
	if err != nil {
		return nil, fmt.Errorf("create remote user request: %w", err)
	}
	request.Header.Set("Content-Type", "application/x-www-form-urlencoded")
	request.Header.Set("Accept", "application/json")
	for name, values := range headers {
		for _, value := range values {
			request.Header.Add(name, value)
		}
	}

	response, err := client.Do(request)
	if err != nil {
		if response != nil && response.Body != nil {
			_ = response.Body.Close()
		}
		return nil, fmt.Errorf("send remote user request: %w", err)
	}
	defer response.Body.Close()

	body, err := io.ReadAll(io.LimitReader(response.Body, remoteUserResponseBodyLimit+1))
	if err != nil {
		return nil, fmt.Errorf("read remote user response: %w", err)
	}
	if len(body) > remoteUserResponseBodyLimit {
		return nil, errRemoteUserResponseTooLarge
	}
	if response.StatusCode < http.StatusOK || response.StatusCode >= http.StatusMultipleChoices {
		return nil, &remoteUserHTTPStatusError{
			statusCode: response.StatusCode,
			status:     response.Status,
		}
	}
	return body, nil
}

// Login User login
func Login(ctx context.Context, req *v2.LoginReq) (res *v2.LoginRes, err error) {

	if req.Username == "" || req.Password == "" {
		return nil, gerror.NewCode(gcode.CodeMissingParameter, "Username / Password cannot be left blank.")
	}

	remoteResp, err := callRemoteLogin(ctx, req)
	if err != nil {
		return nil, err
	}
	if remoteResp == nil {
		return nil, gerror.NewCode(gcode.CodeInvalidParameter, "invalid parameter")
	}
	if err = saveUserToLocal(ctx, remoteResp); err != nil {
		return nil, err
	}
	token, err := generateToken(ctx, remoteResp.Response.Uid)
	if err != nil {
		return nil, err
	}
	return &v2.LoginRes{
		Token: token,
	}, nil
}

// callRemoteLogin Call remote login interface
func callRemoteLogin(ctx context.Context, req *v2.LoginReq) (*model.RemoteLoginResp, error) {
	remoteLoginResp := &model.RemoteLoginResp{}

	loginUrl := g.Cfg().MustGet(ctx, "m5stack.loginUrl").String()

	respBody, err := postRemoteUserForm(
		ctx,
		remoteUserHTTPClient,
		loginUrl,
		nil,
		url.Values{
			"username": {req.Username},
			"password": {req.Password},
		},
	)
	if err != nil {
		g.Log().Errorf(ctx, "Remote login request failed, username=%s: %+v", req.Username, err)
		return nil, gerror.WrapCode(gcode.CodeInternalError, err, "remote service unavailable")
	}
	respBodyText := string(respBody)
	g.Log().Debugf(ctx, "Remote login raw response: %s", respBodyText)
	if strings.Contains(respBodyText, "[[error:") {
		g.Log().Errorf(ctx, "Remote login failed: %s", respBodyText)
		return nil, gerror.NewCode(gcode.CodeBusinessValidationFailed, respBodyText)
	}
	err = gvar.New(respBody).Scan(&remoteLoginResp)
	if err != nil {
		g.Log().Errorf(ctx, "Login response parsing failed: %+v, raw response: %s", err, respBodyText)
		return nil, gerror.WrapCode(gcode.CodeInternalError, err, respBodyText)
	}
	if remoteLoginResp.Status.Code != "ok" {
		errMsg := remoteLoginResp.Status.Message
		g.Log().Errorf(ctx, "Remote login failed: %s", errMsg)
		return nil, gerror.NewCode(gcode.CodeBusinessValidationFailed, remoteLoginResp.Status.Message, errMsg)
	}
	return remoteLoginResp, nil
}

// saveUserToLocal Save user to local database
func saveUserToLocal(ctx context.Context, resp *model.RemoteLoginResp) error {
	data := entity.User{
		Uid:            resp.Response.Uid,
		Username:       resp.Response.Username,
		Userslug:       resp.Response.Userslug,
		DisplayName:    resp.Response.Displayname,
		IconText:       resp.Response.IconText,
		IconBgColor:    resp.Response.IconBgColor,
		EmailConfirmed: resp.Response.EmailConfirmed,
		JoinDate:       resp.Response.Joindate,
		LastOnline:     resp.Response.Lastonline,
		UserStatus:     resp.Response.Status,
	}
	_, err := dao.User.Ctx(ctx).Save(data)
	if err != nil {
		return gerror.WrapCode(gcode.CodeDbOperationError, err, "Failed to write user to local database")
	}
	return nil
}

// generateToken Generate JWT token, includes user UID, issuer, audience, issued time, expiration time
func generateToken(ctx context.Context, uid int64) (string, error) {
	now := time.Now()

	Issuer := g.Cfg().MustGet(ctx, "m5stack.issuer").String()
	Audience := g.Cfg().MustGet(ctx, "m5stack.audience").String()

	claims := jwt.MapClaims{
		"jti": guid.S(),                    // Unique token ID (for revocation/blacklisting)
		"id":  uid,                         // User UID
		"iss": Issuer,                      // Issuer (for verification and anti-forgery)
		"aud": Audience,                    // Audience (to limit scope of use)
		"iat": now.Unix(),                  // Issued at time
		"exp": now.Add(TokenExpire).Unix(), // Expiration time
	}
	tokenObj := jwt.NewWithClaims(jwt.SigningMethodHS256, claims)
	jwtSecret := GetJwtSecret()
	if jwtSecret == "" || len(jwtSecret) < 16 {
		return "", gerror.NewCode(gcode.CodeInternalError, "JWT secret is empty or too weak")
	}
	token, err := tokenObj.SignedString([]byte(jwtSecret))
	if err != nil {
		return "", gerror.WrapCode(gcode.CodeInternalError, err, "Failed to generate token")
	}
	return token, nil
}

// Registration User registration
func Registration(ctx context.Context, req *v2.RegistrationReq) (res *v2.RegistrationRes, err error) {
	if req.UserName == "" || req.Password == "" || req.Email == "" {
		return nil, gerror.NewCode(gcode.CodeMissingParameter, "Username/Email/Password cannot be empty")
	}
	remoteResp, err := callRemoteRegister(ctx, req)
	if err != nil {
		return nil, err
	}
	return new(v2.RegistrationRes(remoteResp)), nil
}

// callRemoteRegister Call remote registration interface
func callRemoteRegister(ctx context.Context, req *v2.RegistrationReq) (res *model.RegistrationResponse, err error) {
	resp := &model.RemoteRegisterResp{}
	g.Log().Infof(ctx, "Remote registration request parameters: username=%s, email=%s", req.UserName, req.Email)

	RegistrationToken := g.Cfg().MustGet(ctx, "m5stack.registrationToken").String()
	RegistrationUrl := g.Cfg().MustGet(ctx, "m5stack.registrationUrl").String()

	respBody, err := postRemoteUserForm(
		ctx,
		remoteUserHTTPClient,
		RegistrationUrl,
		http.Header{"Authorization": {RegistrationToken}},
		url.Values{
			"username": {req.UserName},
			"email":    {req.Email},
			"password": {req.Password},
		},
	)
	if err != nil {
		g.Log().Errorf(ctx, "Remote registration request failed, username=%s: %+v", req.UserName, err)
		return nil, gerror.WrapCode(gcode.CodeInternalError, err, "remote service unavailable")
	}

	respBodyText := string(respBody)
	g.Log().Debugf(ctx, "Remote registration raw response: %s", respBodyText)

	if strings.Contains(respBodyText, "[[error:") {
		g.Log().Errorf(ctx, "Remote registration failed: %s", respBodyText)
		return nil, gerror.NewCode(gcode.CodeBusinessValidationFailed, respBodyText)
	}

	err = gvar.New(respBody).Scan(&resp)
	if err != nil {
		g.Log().Errorf(ctx, "Registration response parsing failed: %+v, raw response: %s", err, respBodyText)
		return nil, gerror.WrapCode(gcode.CodeInternalError, err, respBodyText)
	}

	if resp.Status.Code != "ok" {
		g.Log().Errorf(ctx, "Remote registration business failed: code=%s, message=%s", resp.Status.Code, resp.Status.Message)
		return nil, gerror.NewCodef(gcode.CodeBusinessValidationFailed, resp.Status.Message)
	}
	return &resp.RegistrationResponse, nil
}
