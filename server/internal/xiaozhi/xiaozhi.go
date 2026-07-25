/*
SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
SPDX-License-Identifier: MIT
*/

package xiaozhi

import (
	"context"
	"errors"
	"fmt"
	"io"
	"net/url"
	"regexp"
	"stackChan/internal/model"
	"stackChan/internal/model/xiaozhi"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/gogf/gf/v2/container/gvar"
	"github.com/gogf/gf/v2/encoding/gjson"
	"github.com/gogf/gf/v2/frame/g"
	"github.com/gogf/gf/v2/net/gclient"
	"github.com/gogf/gf/v2/os/gctx"
)

var (
	ctx                  = gctx.New()
	token                string
	tokenExpire          time.Time
	tokenVersion         uint64
	tokenMu              sync.Mutex
	tokenRefreshDone     chan struct{}
	tokenRefreshErr      error
	tokenRefreshFailedAt time.Time
	macSeparatorRegex    = regexp.MustCompile(`[^0-9a-fA-F]`)
	validMacRegex        = regexp.MustCompile(`^([0-9A-Fa-f]{2}[:-]){5}([0-9A-Fa-f]{2})$`)
	globalClient         *gclient.Client
	tokenFetcher         = refreshToken
)

const (
	baseUrl             = "https://xiaozhi.me/"
	tokenPath           = "api/developers/token"
	agentTemplatesList  = "api/developers/agent-templates/list"
	devices             = "api/developers/devices"
	deviceUnbind        = "api/developers/unbind-device"
	agentsDelete        = "api/agents/delete"
	createAgent         = "api/agents"
	chats               = "api/chats/list"
	tokenExpiry         = 24 * time.Hour // Token valid for 24 hours
	refreshFailCooldown = 2 * time.Second
	maxAgentUpdatePages = 1000
	responseBodyLimit   = 4 << 20
	agents              = "api/agents"
)

func init() {
	globalClient = g.Client()
	globalClient.SetTimeout(10 * time.Second)
	globalClient.SetHeader("Content-Type", "application/json")
}

// Unified request processing method, auto handle Session expiration
func doRequest(method string, path string, data interface{}, resp interface{}) error {
	return doRequestContext(ctx, method, path, data, resp)
}

func doRequestContext(requestCtx context.Context, method string, path string, data interface{}, resp interface{}) error {
	requestCtx = normalizeContext(requestCtx)

	for attempt := 0; attempt < 2; attempt++ {
		tokenString, version, err := getToken(requestCtx, false)
		if err != nil {
			return err
		}

		client := globalClient.Clone()
		client.SetHeader("Authorization", "Bearer "+tokenString)

		requestURL := baseUrl + path
		switch strings.ToUpper(method) {
		case "GET":
			if params, ok := data.(g.Map); ok && len(params) > 0 {
				query := url.Values{}
				for key, value := range params {
					query.Set(key, fmt.Sprint(value))
				}
				separator := "?"
				if strings.Contains(requestURL, "?") {
					separator = "&"
				}
				requestURL += separator + query.Encode()
			}
		case "POST":
		case "PUT":
		case "DELETE":
		default:
			return fmt.Errorf("unsupported request method: %s", method)
		}

		response, err := requestVarContext(requestCtx, client, method, requestURL, data)
		if err != nil {
			return err
		}
		if err = response.Scan(resp); err != nil {
			return err
		}

		json := gjson.New(response.Val())
		if json.Get("success").Bool() {
			return nil
		}

		message := json.Get("message").String()
		if message == "Session expired or logged out" && attempt == 0 {
			g.Log().Info(requestCtx, "session expired or logged out, auto refresh token")
			invalidateTokenIfCurrent(tokenString, version)
			continue
		}
		if message == "" {
			message = "upstream request was unsuccessful"
		}
		return fmt.Errorf("xiaozhi request failed: %s", message)
	}
	return errors.New("xiaozhi request failed after token refresh")
}

func requestVarContext(
	requestCtx context.Context,
	client *gclient.Client,
	method string,
	requestURL string,
	data interface{},
) (*gvar.Var, error) {
	if client == nil {
		return nil, errors.New("xiaozhi HTTP client is unavailable")
	}
	method = strings.ToUpper(method)

	var (
		response *gclient.Response
		err      error
	)
	if method == "GET" {
		response, err = client.DoRequest(requestCtx, method, requestURL)
	} else {
		response, err = client.DoRequest(requestCtx, method, requestURL, data)
	}
	if err != nil {
		return nil, fmt.Errorf("xiaozhi request failed: %w", err)
	}
	if response == nil || response.Response == nil || response.Body == nil {
		return nil, errors.New("xiaozhi request returned no response")
	}
	defer response.Close()

	body, err := io.ReadAll(io.LimitReader(response.Body, responseBodyLimit+1))
	if err != nil {
		return nil, fmt.Errorf("read xiaozhi response: %w", err)
	}
	if len(body) > responseBodyLimit {
		return nil, fmt.Errorf("xiaozhi response exceeds %d bytes", responseBodyLimit)
	}
	return gvar.New(body), nil
}

// GetToken Get Token (thread-safe, 24-hour auto-expiration)
func GetToken() (string, error) {
	return GetTokenContext(ctx)
}

func GetTokenContext(requestCtx context.Context) (string, error) {
	tokenString, _, err := getToken(requestCtx, false)
	return tokenString, err
}

func GetNewToken() (string, error) {
	return GetNewTokenContext(ctx)
}

func GetNewTokenContext(requestCtx context.Context) (string, error) {
	tokenString, _, err := getToken(requestCtx, true)
	return tokenString, err
}

func getToken(requestCtx context.Context, forceRefresh bool) (string, uint64, error) {
	requestCtx = normalizeContext(requestCtx)
	forcePending := forceRefresh

	for {
		if err := requestCtx.Err(); err != nil {
			return "", 0, err
		}

		tokenMu.Lock()
		if forcePending {
			if tokenRefreshDone != nil {
				done := tokenRefreshDone
				tokenMu.Unlock()
				select {
				case <-requestCtx.Done():
					return "", 0, requestCtx.Err()
				case <-done:
					forcePending = false
					continue
				}
			}
			invalidateTokenLocked()
			tokenRefreshErr = nil
			tokenRefreshFailedAt = time.Time{}
			forcePending = false
		}

		if token != "" && time.Now().Before(tokenExpire) {
			tokenString := token
			version := tokenVersion
			tokenMu.Unlock()
			return tokenString, version, nil
		}

		if tokenRefreshDone != nil {
			done := tokenRefreshDone
			tokenMu.Unlock()
			select {
			case <-requestCtx.Done():
				return "", 0, requestCtx.Err()
			case <-done:
				continue
			}
		}

		if tokenRefreshErr != nil && time.Since(tokenRefreshFailedAt) < refreshFailCooldown {
			err := tokenRefreshErr
			tokenMu.Unlock()
			return "", 0, err
		}

		done := make(chan struct{})
		tokenRefreshDone = done
		tokenMu.Unlock()

		newToken, err := fetchTokenSafely(requestCtx)
		now := time.Now()

		tokenMu.Lock()
		if err == nil {
			token = newToken
			tokenExpire = now.Add(tokenExpiry)
			tokenVersion++
			tokenRefreshErr = nil
			tokenRefreshFailedAt = time.Time{}
		} else if !errors.Is(err, context.Canceled) && !errors.Is(err, context.DeadlineExceeded) {
			tokenRefreshErr = err
			tokenRefreshFailedAt = now
		}
		version := tokenVersion
		tokenRefreshDone = nil
		close(done)
		tokenMu.Unlock()

		if err != nil {
			g.Log().Error(requestCtx, "refresh token failed: %v", err)
			return "", 0, err
		}
		g.Log().Info(requestCtx, "refresh token success, expire at: %s", now.Add(tokenExpiry).Format("2006-01-02 15:04:05"))
		return newToken, version, nil
	}
}

func fetchTokenSafely(requestCtx context.Context) (newToken string, err error) {
	defer func() {
		if recovered := recover(); recovered != nil {
			err = fmt.Errorf("refresh token panic: %v", recovered)
		}
	}()
	return tokenFetcher(requestCtx)
}

func invalidateTokenIfCurrent(tokenString string, version uint64) {
	tokenMu.Lock()
	defer tokenMu.Unlock()

	if token == tokenString && tokenVersion == version {
		invalidateTokenLocked()
	}
}

func invalidateTokenLocked() {
	token = ""
	tokenExpire = time.Time{}
	tokenVersion++
}

func normalizeContext(requestCtx context.Context) context.Context {
	if requestCtx == nil {
		return context.Background()
	}
	return requestCtx
}

// DeleteAgent Delete agent
func DeleteAgent(agentId int) (bool, error) {
	params := g.Map{
		"id": agentId,
	}

	var resp xiaozhi.XiaoZhiResponse[model.Empty]
	err := doRequest("POST", agentsDelete, params, &resp)
	if err != nil {
		g.Log().Error(ctx, "delete agent failed: %v", err)
		return false, err
	}
	return true, nil
}

func CreateAgent(params g.Map) (*int, error) {
	var resp xiaozhi.XiaoZhiResponse[xiaozhi.CreateAgentResponse]
	err := doRequest("POST", createAgent, params, &resp)
	if err != nil {
		g.Log().Error(ctx, "create agent failed: %v", err)
		return nil, err
	}
	if resp.Data == nil {
		return nil, errors.New("create agent returned no data")
	}
	return &resp.Data.Id, nil
}

// GetAgentTemplate Get agent template
func GetAgentTemplate(page int, pageSize int) (*xiaozhi.ListData[xiaozhi.AgentTemplate], error) {
	return GetAgentTemplateContext(ctx, page, pageSize)
}

func GetAgentTemplateContext(requestCtx context.Context, page int, pageSize int) (*xiaozhi.ListData[xiaozhi.AgentTemplate], error) {
	requestCtx = normalizeContext(requestCtx)
	g.Log().Debug(requestCtx, "Get agent template, page: ", page, " pageSize: ", pageSize)
	queryMap := g.Map{
		"page":     page,
		"pageSize": pageSize,
	}

	var resp xiaozhi.XiaoZhiResponse[xiaozhi.ListData[xiaozhi.AgentTemplate]]
	err := doRequestContext(requestCtx, "GET", agentTemplatesList, queryMap, &resp)
	if err != nil {
		g.Log().Error(requestCtx, "Get agent template failed: %v", err)
		return nil, err
	}
	if resp.Data == nil {
		return nil, errors.New("Get agent template returned no data")
	}

	g.Log().Info(requestCtx,
		"Get agent template success, list length: ", len(resp.Data.List),
		" total count: ", resp.Pagination.Total,
	)

	return resp.Data, nil
}

// SetAgentSetting Update agent settings
func SetAgentSetting(agentId int, parameters xiaozhi.AgentConfig) (bool, error) {
	return SetAgentSettingContext(ctx, agentId, parameters)
}

func SetAgentSettingContext(requestCtx context.Context, agentId int, parameters xiaozhi.AgentConfig) (bool, error) {
	requestCtx = normalizeContext(requestCtx)
	path := "api/agents/" + strconv.Itoa(agentId) + "/config"
	url := baseUrl + path
	g.Log().Info(requestCtx, "Update agent setting, agentId:", agentId, "url: ", url)
	g.Log().Info(requestCtx, "Request body parameters: ", gjson.MustEncodeString(parameters))

	var resp xiaozhi.XiaoZhiResponse[model.Empty]
	err := doRequestContext(requestCtx, "POST", path, parameters, &resp)
	if err != nil {
		g.Log().Error(requestCtx, "Update agent setting failed: %v", err)
		return false, err
	}

	g.Log().Info(requestCtx, "Update agent setting success, agentId:", agentId)
	return true, nil
}

// GetDevices Get device list
func GetDevices(
	page *int,
	pageSize *int,
	macAddress *string,
	serialNumber *string,
	productID *int,
	DeviceID *int,
) (*[]xiaozhi.Device, error) {
	return GetDevicesContext(ctx, page, pageSize, macAddress, serialNumber, productID, DeviceID)
}

func GetDevicesContext(
	requestCtx context.Context,
	page *int,
	pageSize *int,
	macAddress *string,
	serialNumber *string,
	productID *int,
	DeviceID *int,
) (*[]xiaozhi.Device, error) {
	requestCtx = normalizeContext(requestCtx)
	newMacAddress := formatMac(macAddress)

	// Added: Request parameter logging

	queryMap := g.Map{}
	if page != nil {
		queryMap["page"] = *page
	}
	if pageSize != nil {
		queryMap["pageSize"] = *pageSize
	}
	if macAddress != nil {
		if newMacAddress == nil {
			return nil, errors.New("invalid MAC address")
		}
		queryMap["mac_address"] = *newMacAddress
	}
	if serialNumber != nil {
		queryMap["serial_number"] = *serialNumber
	}
	if productID != nil {
		queryMap["product_id"] = *productID
	}
	if DeviceID != nil {
		queryMap["device_id"] = *DeviceID
	}

	g.Log().Debug(requestCtx,
		"Get device list, data:", queryMap,
	)

	var resp xiaozhi.XiaoZhiResponse[xiaozhi.ListData[xiaozhi.Device]]
	err := doRequestContext(requestCtx, "GET", devices, queryMap, &resp)
	if err != nil {
		g.Log().Error(requestCtx, "Get device list failed: %v", err)
		return nil, err
	}
	if resp.Data == nil {
		return nil, errors.New("Get device list returned no data")
	}

	g.Log().Info(requestCtx, "Get device list success, list length:", len(resp.Data.List), " total count:", resp.Pagination.Total)

	if len(resp.Data.List) > 0 {
		g.Log().Info(requestCtx, "Get device list success, first device:", resp.Data.List[0])
	}

	return &resp.Data.List, nil
}

func GetAgents(page *int, pageSize *int, keyword *string) (*[]xiaozhi.Agent, error) {
	// Added: Request parameter logging
	g.Log().Debug(ctx,
		"Get agent list, page:", page,
		" pageSize:", pageSize,
		" keyword:", keyword,
	)
	queryMap := g.Map{}
	if keyword != nil {
		queryMap["keyword"] = *keyword
	}
	if page != nil {
		queryMap["page"] = *page
	}
	if pageSize != nil {
		queryMap["pageSize"] = *pageSize
	}
	var resp xiaozhi.XiaoZhiResponse[[]xiaozhi.Agent]
	err := doRequest("GET", agents, queryMap, &resp)
	if err != nil {
		g.Log().Error(ctx, "Get agent list failed: %v", err)
		return nil, err
	}
	if resp.Data == nil {
		return nil, errors.New("Get agent list returned no data")
	}
	g.Log().Info(ctx,
		"Get agent list success, list length:", len(*resp.Data),
		" total count:", resp.Pagination.Total,
	)

	return resp.Data, nil
}

func formatMac(mac *string) *string {
	if mac == nil {
		return nil
	}
	cleanMac := macSeparatorRegex.ReplaceAllString(*mac, "")
	if !isValidMac(cleanMac) {
		return nil
	}
	cleanMac = strings.ToLower(cleanMac)
	var parts []string
	for i := 0; i < 12; i += 2 {
		parts = append(parts, cleanMac[i:i+2])
	}
	return new(strings.Join(parts, ":"))
}

func isValidMac(cleanMac string) bool {
	return len(cleanMac) == 12
}

type ZhiGetToken struct {
	Token string `json:"token"`
}

// refreshToken refreshes the token without holding tokenMu. Callers coordinate
// concurrent refreshes through getToken.
func refreshToken(requestCtx context.Context) (string, error) {
	requestCtx = normalizeContext(requestCtx)
	secretKey := g.Cfg().MustGet(requestCtx, "xiaozhi.secret_key").String()
	if secretKey == "" {
		g.Log().Error(requestCtx, "xiaozhi.secret_key is empty, please check config file")
		return "", errors.New("xiaozhi.secret_key is empty")
	}

	g.Log().Debug(requestCtx, "refresh token")
	requestData := g.Map{
		"secret_key": secretKey,
	}

	client := globalClient.Clone()

	var resp xiaozhi.XiaoZhiResponse[ZhiGetToken]
	response, err := requestVarContext(requestCtx, client, "POST", baseUrl+tokenPath, requestData)
	if err != nil {
		return "", fmt.Errorf("refresh token request failed: %w", err)
	}
	err = response.Scan(&resp)
	if err != nil {
		g.Log().Error(requestCtx, "refresh token failed: %v", err)
		return "", fmt.Errorf("refresh token failed: %w", err)
	}

	if !resp.Success {
		g.Log().Error(requestCtx, "refresh token failed: %s", resp.Message)
		return "", fmt.Errorf("refresh token failed: %s", resp.Message)
	}

	if resp.Data == nil {
		g.Log().Error(requestCtx, "refresh token failed: response data is empty")
		return "", errors.New("refresh token response data is empty")
	}
	if resp.Data.Token == "" {
		g.Log().Error(requestCtx, "refresh token failed: token is empty")
		return "", fmt.Errorf("token is empty")
	}

	g.Log().Debug(requestCtx, "refresh token success")
	return resp.Data.Token, nil
}

// UnbindDevice Unbind device from XiaoZhi side
// @param macAddress Device MAC address
func UnbindDevice(macAddress *string) (bool, error) {
	return UnbindDeviceContext(ctx, macAddress)
}

func UnbindDeviceContext(requestCtx context.Context, macAddress *string) (bool, error) {
	requestCtx = normalizeContext(requestCtx)
	g.Log().Debug(requestCtx, "unbind device")
	if formatMac(macAddress) == nil {
		return false, errors.New("invalid MAC address")
	}

	// First query device ID
	devices, err := GetDevicesContext(requestCtx, new(1), new(10), macAddress, nil, nil, nil)
	if err != nil {
		g.Log().Error(requestCtx, err.Error())
		return false, err
	}

	if len(*devices) == 0 {
		g.Log().Error(requestCtx, "unbind device failed: device not found, mac=%s", *macAddress)
		/// Device not found, return true
		return true, nil
	}
	deviceID := (*devices)[0].DeviceID

	requestData := g.Map{
		"device_id": deviceID,
	}

	g.Log().Info(requestCtx, "unbind device, device_id: ", (*devices)[0])
	g.Log().Info(requestCtx, "request data: ", gjson.MustEncodeString(requestData))

	var resp xiaozhi.XiaoZhiResponse[model.Empty]
	err = doRequestContext(requestCtx, "POST", deviceUnbind, requestData, &resp)
	if err != nil {
		g.Log().Error(requestCtx, "unbind device failed: %v", err)
		return false, err
	}
	if !resp.Success {
		if resp.Message == "device not found" {
			g.Log().Info(requestCtx, "unbind device failed: device not found")
			return true, nil
		}
		g.Log().Error(requestCtx, "unbind device failed: %s", resp.Message)
		return false, nil
	}
	g.Log().Info(requestCtx, "unbind device success, device_id: ", deviceID)
	g.Log().Info(requestCtx, resp.Message)

	return true, nil
}

// UpdateAllDevices / Temporary script code, update mcp tools for all devices
func UpdateAllDevices() (bool, error) {
	// Initial pagination values
	page := 1
	pageSize := 100

	// Loop through pages until no more data. The hard ceiling protects this
	// maintenance helper from an upstream that repeats the same page forever.
	for page <= maxAgentUpdatePages {
		// Get current page device list
		agents, err := GetAgents(&page, &pageSize, nil)
		if err != nil {
			return false, err
		}

		// If no data on current page, all pages processed, exit loop
		if agents == nil || len(*agents) == 0 {
			g.Log().Info(ctx, "update all devices success")
			return true, nil
		}

		g.Log().Info(ctx, "update all devices, page: ", page, ", total: ", len(*agents))

		for _, agent := range *agents {
			agentId := agent.ID

			parameters := xiaozhi.AgentConfig{
				AgentName:           agent.AgentName,
				AssistantName:       agent.AssistantName,
				LlmModel:            agent.LlmModel,
				TtsVoice:            agent.TtsVoice,
				TtsSpeechSpeed:      agent.TtsSpeechSpeed,
				TtsPitch:            agent.TtsPitch,
				AsrSpeed:            agent.AsrSpeed,
				Language:            agent.Language,
				Character:           agent.Character,
				Memory:              agent.Memory,
				MemoryType:          agent.MemoryType,
				KnowledgeBaseIds:    []int{},
				McpEndpoints:        nil,
				ProductMcpEndpoints: nil,
			}

			path := "api/agents/" + strconv.FormatInt(agentId, 10) + "/config"
			url := baseUrl + path
			g.Log().Info(ctx, "update agent config, agentId: ", agentId, "url: ", url)
			g.Log().Info(ctx, "request data: ", gjson.MustEncodeString(parameters))

			var resp xiaozhi.XiaoZhiResponse[model.Empty]
			err := doRequest("POST", path, parameters, &resp)
			if err != nil {
				g.Log().Error(ctx, "update agent config failed: %v", err)
				return false, err
			}

			if !resp.Success {
				g.Log().Info(ctx, "update agent config failed: %s", resp.Message)
				continue
			}

			g.Log().Info(ctx, "update agent config success, agentId: ", agentId)
		}

		if len(*agents) < pageSize {
			g.Log().Info(ctx, "update all devices success")
			return true, nil
		}

		// Page number +1, continue requesting next page
		page++
	}

	return false, fmt.Errorf("update all devices exceeded %d pages", maxAgentUpdatePages)
}

func DeleteChats() {
	page := 1
	pageSize := 100
	requestData := g.Map{
		"page": page,
		"size": pageSize,
	}

	var resp xiaozhi.XiaoZhiResponse[xiaozhi.ListData[xiaozhi.Conversation]]
	err := doRequest("GET", chats, requestData, &resp)
	if err != nil {
		return
	}

	if !resp.Success {
		return
	}
	if resp.Data == nil {
		g.Log().Error(ctx, "delete chats returned no data")
		return
	}

	list := resp.Data.List

	for _, item := range list {

		url := "api/agents/" + strconv.Itoa(item.AgentId) + "/chats/" + strconv.Itoa(item.Id)

		var deleResp xiaozhi.XiaoZhiResponse[model.Empty]
		err := doRequest("DELETE", url, nil, &deleResp)
		if err != nil {
			g.Log().Error(ctx, "delete chat failed, agentId:", item.AgentId, " chatId:", item.Id, " error:", err)
			continue
		}

		g.Log().Info(ctx, "delete chat success, agentId:", item.AgentId, " chatId:", item.Id)

	}

}
