/*
SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
SPDX-License-Identifier: MIT
*/

package web_socket

import (
	"context"
	"encoding/base64"
	"encoding/binary"
	"errors"
	"net"
	"net/http"
	"regexp"
	"stackChan/internal/model"
	"stackChan/internal/service"
	"stackChan/utility"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/gogf/gf/v2/frame/g"
	"github.com/gogf/gf/v2/net/ghttp"
	"github.com/gorilla/websocket"
)

const (
	Opus          byte = 0x01
	Jpeg          byte = 0x02
	ControlAvatar byte = 0x03
	ControlMotion byte = 0x04
	OnCamera      byte = 0x05
	OffCamera     byte = 0x06

	TextMessage byte = 0x07
	RequestCall byte = 0x09
	RefuseCall  byte = 0x0A
	AgreeCall   byte = 0x0B
	HangupCall  byte = 0x0C

	UpdateDeviceName byte = 0x0D
	GetDeviceName    byte = 0x0E

	inCall byte = 0x0F

	ping byte = 0x10
	pong byte = 0x11

	OnPhoneScreen    byte = 0x12
	OffPhoneScreen   byte = 0x13
	Dance            byte = 0x14
	GetAvatarPosture byte = 0x15

	DeviceOffline byte = 0x16
	DeviceOnline  byte = 0x17

	OnAudio  byte = 0x18
	OffAudio byte = 0x19

	AimedTakePhoto byte = 0x1A

	websocketReadLimit int64 = 8 * 1024 * 1024
	authTokenMaxSkew         = 10 * time.Second
)

var (
	wsUpGrader = websocket.Upgrader{
		CheckOrigin: func(r *http.Request) bool { return true },
		Error: func(w http.ResponseWriter, r *http.Request, status int, reason error) {
			logger.Errorf(r.Context(), "WebSocket Upgrade failed: %v", reason)
		},
	}
	logger              = g.Log()
	stackChanClientPool = sync.Map{}
	stackChanClientMu   sync.Mutex
	appClientPool       = sync.Map{}
	appClientMu         sync.RWMutex
	validMacPattern     = regexp.MustCompile(
		`^(?:[0-9A-Fa-f]{12}|(?:[0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}|(?:[0-9A-Fa-f]{2}-){5}[0-9A-Fa-f]{2})$`,
	)
)

// GetMac get MAC address from request header
func GetMac(r *ghttp.Request) (string, error) {
	if token := r.Header.Get(model.Authorization); token != "" {
		decodedToken, err := base64.StdEncoding.DecodeString(token)
		if err != nil {
			logger.Errorf(r.Context(), "Error base64 decoding token: %v", err)
			return "", err
		}
		decrypted, err := utility.RSADecrypt(decodedToken)
		if err != nil {
			logger.Errorf(r.Context(), "Error decrypting token: %v", err)
			return "", err
		}
		tokenStr := string(decrypted)
		parts := strings.Split(tokenStr, "|")
		if len(parts) != 3 {
			return "", errors.New("invalid token")
		}
		mac := parts[0]
		if !validMacPattern.MatchString(mac) {
			return "", errors.New("invalid MAC address")
		}
		tsStr := parts[2]
		ts, err := strconv.ParseInt(tsStr, 10, 64)
		if err != nil {
			return "", errors.New("invalid timestamp")
		}
		now := time.Now().Unix()
		maxSkewSeconds := int64(authTokenMaxSkew / time.Second)
		if ts < now-maxSkewSeconds || ts > now+maxSkewSeconds {
			return "", errors.New("token expired or not yet valid")
		}
		return mac, nil
	}
	return "", nil
}

// Handler WebSocket handler function
func Handler(r *ghttp.Request) {
	ctx := r.Context()
	mac, err := GetMac(r)
	if err != nil || mac == "" {
		r.Response.WriteHeader(http.StatusUnauthorized) // Return 401
		r.Response.Write("Unauthorized: invalid or missing MAC")
		return
	}
	deviceType := r.Get("deviceType").String()
	deviceId := ""
	switch deviceType {
	case "StackChan":
	case "App":
		deviceId = r.Get("deviceId").String()
		if deviceId == "" {
			r.Response.WriteHeader(http.StatusBadRequest)
			r.Response.Write("The deviceId parameter in the App end is empty.")
			return
		}
	default:
		r.Response.WriteHeader(http.StatusBadRequest)
		r.Response.Write("The deviceType parameter is invalid.")
		return
	}

	ws, err := wsUpGrader.Upgrade(r.Response.Writer, r.Request, nil)
	if err != nil {
		r.Response.Write(err.Error())
		return
	}
	ws.SetReadLimit(websocketReadLimit)

	if deviceType == "StackChan" {
		client, previousConn, created := registerStackChanClient(mac, ws)
		if previousConn != nil && previousConn != ws {
			_ = previousConn.Close()
		}
		if created {
			if _, createErr := service.CreateMacIfNotExists(ctx, mac); createErr != nil {
				// WebSocket forwarding does not depend on this metadata write.
				// Keep the device online; a later registration flow can retry.
				logger.Errorf(ctx, "Failed to persist newly connected StackChan %s: %v", mac, createErr)
			}
		}
		if !created {
			if client.GetCallAppClient() != nil {
				reconnectMsg := createStringMessage(TextMessage, "The equipment has been reconnected.")
				stackChanSendMessage(ctx, client, new(websocket.BinaryMessage), reconnectMsg)
			}
			if len(client.GetCameraSubscriptionList()) > 0 {
				onMsg := createMessage(OnCamera, nil)
				stackChanSendMessage(ctx, client, new(websocket.BinaryMessage), onMsg)
			}
			if len(client.GetAudioSubscriptionList()) > 0 {
				onMsg := createMessage(OnAudio, nil)
				stackChanSendMessage(ctx, client, new(websocket.BinaryMessage), onMsg)
			}
		}

		// send Online
		onlineMsg := createStringMessage(DeviceOnline, "Your StackChan has been launched.")
		msgType := websocket.BinaryMessage
		// Notify App
		appClients := getAppClients(client.GetMac())
		for _, appClient := range appClients {
			if appClient == nil {
				continue
			}
			appSendMessage(ctx, appClient, &msgType, onlineMsg)
		}

		logger.Info(ctx, "There is a StackChen connected to the service.", client.GetMac())
		defer func() {
			logger.Info(ctx, "There is a StackChan that has disconnected.", mac, deviceType)
			client.ClearConnIf(ws)
			_ = ws.Close()
		}()
		for {
			messageType, msg, err := ws.ReadMessage()
			if err != nil {
				if websocket.IsCloseError(err, websocket.CloseNormalClosure, websocket.CloseGoingAway) {
					logger.Infof(ctx, "StackChan Normal disconnection: mac=%s, deviceType=%s, Reason=%v", mac, deviceType, err)
					break
				}

				if ne, ok := errors.AsType[net.Error](err); ok && ne.Timeout() {
					logger.Infof(ctx, "StackChan Timeout disconnection: mac=%s, deviceType=%s", mac, deviceType)
					break
				}

				logger.Errorf(ctx, "StackChan Abnormal disconnection: mac=%s, deviceType=%s, Error=%v", mac, deviceType, err)
				break
			}
			if client.GetConn() != ws {
				logger.Debugf(ctx, "Ignore message from replaced StackChan connection: mac=%s", mac)
				break
			}
			client.SetLastTime(time.Now())
			readStackChanMessage(ctx, client, &messageType, &msg)
		}
	} else if deviceType == "App" {
		client, previousConn, _ := registerAppClient(mac, deviceId, ws)
		if previousConn != nil && previousConn != ws {
			_ = previousConn.Close()
		}
		logger.Info(ctx, "There is an App connected to the service.", client.GetMac())

		// check StackChan status
		stackChanClient := getStackChanClient(client.GetMac())
		if stackChanClient == nil || stackChanClient.GetConn() == nil {
			offlineMsg := createStringMessage(DeviceOffline, "Your StackChan is offline.")
			appSendMessage(ctx, client, new(websocket.BinaryMessage), offlineMsg)
		} else {
			onlineMsg := createStringMessage(DeviceOnline, "Your StackChan has been launched.")
			appSendMessage(ctx, client, new(websocket.BinaryMessage), onlineMsg)
		}

		defer func() {
			logger.Info(ctx, "There is an App that has disconnected.", mac, deviceType)
			client.ClearConnIf(ws)
			_ = ws.Close()
		}()
		for {
			messageType, msg, err := ws.ReadMessage()
			if err != nil {
				var ne net.Error
				if websocket.IsCloseError(err, websocket.CloseNormalClosure, websocket.CloseGoingAway) {
					logger.Infof(ctx, "App Normal disconnection: mac=%s, deviceType=%s, Error=%v", mac, deviceType, err)
					break
				}
				if errors.As(err, &ne) && ne.Timeout() {
					logger.Infof(ctx, "App Timeout disconnection: mac=%s, deviceType=%s", mac, deviceType)
					break
				}
				logger.Errorf(ctx, "App Abnormal disconnection: mac=%s, deviceType=%s, Error=%v", mac, deviceType, err)
				break
			}
			if client.GetConn() != ws {
				logger.Debugf(ctx, "Ignore message from replaced App connection: mac=%s, deviceId=%s", mac, deviceId)
				break
			}
			client.SetLastTime(time.Now())
			readAppClientMessage(ctx, client, &messageType, &msg)
		}
	}
}

func registerStackChanClient(mac string, conn *websocket.Conn) (
	client *model.StackChanClient,
	previousConn *websocket.Conn,
	created bool,
) {
	stackChanClientMu.Lock()
	defer stackChanClientMu.Unlock()

	if value, ok := stackChanClientPool.Load(mac); ok {
		if existing, valid := value.(*model.StackChanClient); valid && existing != nil {
			if !existing.IsStopped() {
				previousConn = existing.ReplaceConn(conn)
				existing.SetLastTime(time.Now())
				return existing, previousConn, false
			}
			previousConn = existing.GetConn()
		}
		stackChanClientPool.Delete(mac)
	}

	client = model.NewStackChanClient(mac, conn, make([]*model.AppClient, 0), nil, false)
	stackChanClientPool.Store(mac, client)
	return client, nil, true
}

func registerAppClient(mac string, deviceID string, conn *websocket.Conn) (
	client *model.AppClient,
	previousConn *websocket.Conn,
	created bool,
) {
	appClientMu.Lock()
	defer appClientMu.Unlock()

	var clients []*model.AppClient
	var matchingClient *model.AppClient
	var stoppedMatchingClient *model.AppClient
	if value, ok := appClientPool.Load(mac); ok {
		if existingClients, valid := value.([]*model.AppClient); valid {
			clients = make([]*model.AppClient, 0, len(existingClients)+1)
			for _, existing := range existingClients {
				if existing == nil {
					continue
				}
				if existing.GetDeviceId() == deviceID &&
					existing.GetMac() == mac &&
					existing.IsStopped() {
					if stoppedMatchingClient == nil {
						stoppedMatchingClient = existing
					}
					continue
				}
				clients = append(clients, existing)
				if matchingClient == nil &&
					existing.GetDeviceId() == deviceID &&
					existing.GetMac() == mac {
					matchingClient = existing
				}
			}
		}
	}
	if matchingClient != nil {
		previousConn = matchingClient.ReplaceConn(conn)
		matchingClient.SetLastTime(time.Now())
		appClientPool.Store(mac, clients)
		return matchingClient, previousConn, false
	}

	client = model.NewAppClient(mac, conn, deviceID)
	if stoppedMatchingClient != nil {
		previousConn = stoppedMatchingClient.GetConn()
	}
	clients = append(clients, client)
	appClientPool.Store(mac, clients)
	return client, previousConn, true
}

// Handle WebSocket connection requests from App devices
func addAppClient(c *model.AppClient) {
	mac := c.GetMac()

	appClientMu.Lock()
	defer appClientMu.Unlock()

	var clients []*model.AppClient
	if val, ok := appClientPool.Load(mac); ok {
		if existing, valid := val.([]*model.AppClient); valid {
			clients = make([]*model.AppClient, len(existing), len(existing)+1)
			copy(clients, existing)
		}
	}
	clients = append(clients, c)
	appClientPool.Store(mac, clients)
}

// Get all App clients with specified MAC address
func getAppClients(mac string) []*model.AppClient {
	appClientMu.RLock()
	defer appClientMu.RUnlock()

	if val, ok := appClientPool.Load(mac); ok {
		if clients, valid := val.([]*model.AppClient); valid {
			out := make([]*model.AppClient, len(clients))
			copy(out, clients)
			return out
		}
	}
	return nil
}

type appClientPoolEntry struct {
	key     any
	clients []*model.AppClient
	valid   bool
}

func getAppClientPoolSnapshot() []appClientPoolEntry {
	appClientMu.RLock()
	defer appClientMu.RUnlock()

	var entries []appClientPoolEntry
	appClientPool.Range(func(key, value any) bool {
		clients, valid := value.([]*model.AppClient)
		entry := appClientPoolEntry{key: key, valid: valid}
		if valid {
			entry.clients = make([]*model.AppClient, len(clients))
			copy(entry.clients, clients)
		}
		entries = append(entries, entry)
		return true
	})
	return entries
}

// Get StackChan client with specified MAC address
func getStackChanClient(mac string) *model.StackChanClient {
	if val, ok := stackChanClientPool.Load(mac); ok {
		return val.(*model.StackChanClient)
	}
	return nil
}

// Parse custom binary protocol messages, return message type, data length, payload and success status
func parseBinaryMessage(ctx context.Context, msg *[]byte) (byte, int, []byte, bool) {
	if msg == nil || len(*msg) < 1+4 {
		logger.Warning(ctx, "Message too short, cannot parse header, message not forwarded")
		return 0, 0, nil, false
	}

	msgType := (*msg)[0]
	declaredLength := binary.BigEndian.Uint32((*msg)[1:5])
	actualLength := len(*msg) - 5
	if uint64(declaredLength) != uint64(actualLength) {
		logger.Warningf(ctx, "Length mismatch: header says %d, actual is %d, message not forwarded", declaredLength, actualLength)
		return 0, 0, nil, false
	}

	return msgType, int(declaredLength), (*msg)[5:], true
}

// Handle WebSocket messages from StackChan devices
func readStackChanMessage(ctx context.Context, client *model.StackChanClient, messageType *int, msg *[]byte) {
	if *messageType == websocket.BinaryMessage {
		msgType, _, _, ok := parseBinaryMessage(ctx, msg)
		if !ok {
			return
		}
		switch msgType {
		case pong:
			break
		case ControlAvatar, ControlMotion, OnCamera, OffCamera:
			break
		case RefuseCall:
			// Reject call, remove and notify App client
			appClient := client.GetCallAppClient()
			if appClient != nil {
				appSendMessage(ctx, appClient, messageType, msg)
				client.SetCallAppClient(nil)
			}
			break
		case AgreeCall:
			// Accept call, add App client to subscription list
			appClient := client.GetCallAppClient()
			if appClient != nil {
				appSendMessage(ctx, appClient, messageType, msg)
				_, firstCameraSubscriber := client.SubscribeCamera(appClient)
				if firstCameraSubscriber {
					onMsg := createMessage(OnCamera, nil)
					onType := websocket.BinaryMessage
					stackChanSendMessage(ctx, client, &onType, onMsg)
				}
				_, firstAudioSubscriber := client.SubscribeAudio(appClient)
				if firstAudioSubscriber {
					onMsg := createMessage(OnAudio, nil)
					onType := websocket.BinaryMessage
					stackChanSendMessage(ctx, client, &onType, onMsg)
				}
			}
			break
		case HangupCall:
			// Hang up call, remove App client and update subscription list
			appClient := client.GetCallAppClient()
			if appClient != nil {
				appSendMessage(ctx, appClient, messageType, msg)
				// Remove the client from the subscription list
				_, cameraSubscriptionsEmpty := client.UnsubscribeCamera(appClient)
				// If the subscription list is empty, notify to turn off the camera
				if cameraSubscriptionsEmpty {
					offMsg := createMessage(OffCamera, nil)
					offType := websocket.BinaryMessage
					stackChanSendMessage(ctx, client, &offType, offMsg)
				}

				_, audioSubscriptionsEmpty := client.UnsubscribeAudio(appClient)
				if audioSubscriptionsEmpty {
					offMsg := createMessage(OffAudio, nil)
					offType := websocket.BinaryMessage
					stackChanSendMessage(ctx, client, &offType, offMsg)
				}
			}
			break
		case GetDeviceName:
			// Query device name
			name, err := service.GetDeviceName(ctx, client.GetMac())
			if err != nil {
				return
			}
			if name == "" {
				logger.Infof(ctx, "Queried device nickname is empty")
				return
			}
			newMsg := createStringMessage(GetDeviceName, name)
			stackChanSendMessage(ctx, client, messageType, newMsg)
			break
		case Opus:
			subscribers := client.GetAudioSubscriptionList()
			if len(subscribers) > 0 {
				var isAll = true
				for _, subClient := range subscribers {
					if subClient.GetConn() != nil {
						isAll = false
					}
					appSendMessage(ctx, subClient, messageType, msg)
				}
				if isAll {
					msg = createMessage(OffAudio, nil)
					stackChanSendMessage(ctx, client, messageType, msg)
				}
			} else {
				msg = createMessage(OffAudio, nil)
				stackChanSendMessage(ctx, client, messageType, msg)
			}
			break
		case Jpeg:
			subscribers := client.GetCameraSubscriptionList()
			if len(subscribers) > 0 {
				var isAll = true
				for _, subClient := range subscribers {
					if subClient.GetConn() != nil {
						isAll = false
					}
					appSendMessage(ctx, subClient, messageType, msg)
				}
				if isAll {
					msg = createMessage(OffCamera, nil)
					stackChanSendMessage(ctx, client, messageType, msg)
				}
			} else {
				msg = createMessage(OffCamera, nil)
				stackChanSendMessage(ctx, client, messageType, msg)
			}
			break
		case GetAvatarPosture:
			appClients := getAppClients(client.GetMac())
			for _, appClient := range appClients {
				appSendMessage(ctx, appClient, messageType, msg)
			}
			break
		case AimedTakePhoto:
			appClient := client.GetAimedTakePhotoAppClient()
			if appClient != nil {
				appSendMessage(ctx, appClient, messageType, msg)
			}
			break
		default:
			logger.Infof(ctx, "Unknown binary msgType: %d", msgType)
			appClients := getAppClients(client.GetMac())
			if appClients != nil {
				for _, appClient := range appClients {
					appSendMessage(ctx, appClient, messageType, msg)
				}
			}
		}
	} else if *messageType == websocket.TextMessage {
		appClients := getAppClients(client.GetMac())
		if appClients != nil {
			for _, appClient := range appClients {
				appSendMessage(ctx, appClient, messageType, msg)
			}
		}
	} else if *messageType == websocket.PingMessage {
		logger.Info(ctx, "Received ping message from StackChan side")
	}
}

// Handle WebSocket messages from App clients
func readAppClientMessage(ctx context.Context, client *model.AppClient, messageType *int, msg *[]byte) {
	if *messageType == websocket.BinaryMessage {
		msgType, _, payload, ok := parseBinaryMessage(ctx, msg)
		if !ok {
			return
		}
		switch msgType {
		case pong:
			break
		case GetDeviceName:
			// Query device name
			name, err := service.GetDeviceName(ctx, client.GetMac())
			if err != nil {
				logger.Errorf(ctx, "Failed to get device name: %v", err)
				return
			}
			if name == "" {
				logger.Infof(ctx, "Queried device nickname is empty")
				return
			}
			newMsg := createStringMessage(GetDeviceName, name)
			logger.Infof(ctx, "Device name found, returning: %s", name)
			appSendMessage(ctx, client, messageType, newMsg)
			break
		case UpdateDeviceName:
			stackChanClient := getStackChanClient(client.GetMac())
			if stackChanClient != nil {
				stackChanSendMessage(ctx, stackChanClient, messageType, msg)
			}
			appClients := getAppClients(client.GetMac())
			for _, appClient := range appClients {
				appSendMessage(ctx, appClient, messageType, msg)
			}
			break
		case Opus:
			if payload == nil || len(payload) < 12 {
				logger.Warningf(ctx, "Payload too short, cannot parse MAC address: %v", payload)
				return
			}
			macAddrBytes := payload[:12]
			data := payload[12:]
			macAddr := string(macAddrBytes)
			newMsg := createMessage(msgType, data)
			stackChanClient := getStackChanClient(macAddr)
			if stackChanClient != nil {
				stackChanSendMessage(ctx, stackChanClient, messageType, newMsg)
			}
			break
		case Jpeg:
			if payload == nil || len(payload) < 12 {
				logger.Warningf(ctx, "Payload too short, cannot parse MAC address: %v", payload)
				return
			}
			macAddrBytes := payload[:12]
			data := payload[12:]
			macAddr := string(macAddrBytes)
			newMsg := createMessage(msgType, data)
			stackChanClient := getStackChanClient(macAddr)
			if stackChanClient != nil {
				if stackChanClient.GetPhoneScreen() {
					stackChanSendMessage(ctx, stackChanClient, messageType, newMsg)
				}
			}
			break
		case ControlAvatar, ControlMotion:
			if payload == nil || len(payload) < 12 {
				logger.Warningf(ctx, "Payload too short, cannot parse MAC address: %v", payload)
				return
			}
			macAddrBytes := payload[:12]
			data := payload[12:]
			macAddr := string(macAddrBytes)
			newMsg := createMessage(msgType, data)
			stackChanClient := getStackChanClient(macAddr)
			if stackChanClient != nil {
				stackChanSendMessage(ctx, stackChanClient, messageType, newMsg)
			} else {
				logger.Infof(ctx, "StackChan is currently offline")
			}
			break
		case TextMessage:
			if payload == nil || len(payload) < 12 {
				logger.Warningf(ctx, "Payload too short, cannot parse MAC address: %v", payload)
				return
			}
			macAddr := string(payload[:12])
			data := payload[12:]
			newMsg := createMessage(msgType, data)
			stackChanClient := getStackChanClient(macAddr)
			if stackChanClient != nil {
				stackChanSendMessage(ctx, stackChanClient, messageType, newMsg)
			}
			appClients := getAppClients(macAddr)
			if appClients != nil {
				for _, appClient := range appClients {
					appSendMessage(ctx, appClient, messageType, newMsg)
				}
			}
			break
		case RequestCall:
			// Request call
			if payload == nil || len(payload) < 12 {
				logger.Warningf(ctx, "Payload too short, cannot parse MAC address: %v", payload)
				return
			}
			macAddr := string(payload[:12])
			data := payload[12:]
			stackChanClient := getStackChanClient(macAddr)
			if stackChanClient != nil {
				if stackChanClient.GetCallAppClient() == nil || stackChanClient.GetCallAppClient() == client {
					stackChanClient.SetCallAppClient(client)
					newMsg := createMessage(msgType, data)
					stackChanSendMessage(ctx, stackChanClient, messageType, newMsg)
				} else {
					// Notify App that the other side is already in a call
					newMsg := createStringMessage(inCall, "The other party is currently in a call")
					appSendMessage(ctx, client, messageType, newMsg)
				}
			}
			break
		case HangupCall:
			stackChanClientPool.Range(func(_, value any) bool {
				stackChanClient := value.(*model.StackChanClient)
				if stackChanClient.GetCallAppClient() == client {
					// Found corresponding call
					stackChanClient.SetCallAppClient(nil)
					stackChanSendMessage(ctx, stackChanClient, messageType, msg)

					_, cameraSubscriptionsEmpty := stackChanClient.UnsubscribeCamera(client)
					if cameraSubscriptionsEmpty {
						offMsg := createMessage(OffCamera, nil)
						offType := websocket.BinaryMessage
						stackChanSendMessage(ctx, stackChanClient, &offType, offMsg)
					}

					_, audioSubscriptionsEmpty := stackChanClient.UnsubscribeAudio(client)
					if audioSubscriptionsEmpty {
						offMsg := createMessage(OffAudio, nil)
						offType := websocket.BinaryMessage
						stackChanSendMessage(ctx, stackChanClient, &offType, offMsg)
					}

					return false
				}
				return true
			})
			break
		case OnAudio:
			macAddr := string(payload)
			stackChanClient := getStackChanClient(macAddr)
			if stackChanClient != nil {
				if added, _ := stackChanClient.SubscribeAudio(client); added {
					stackChanSendMessage(ctx, stackChanClient, messageType, msg)
				}
			}
			break
		case OffAudio:
			macAddr := string(payload)
			stackChanClient := getStackChanClient(macAddr)
			if stackChanClient != nil {
				existed, empty := stackChanClient.UnsubscribeAudio(client)
				shouldNotify := existed && empty
				if shouldNotify {
					stackChanSendMessage(ctx, stackChanClient, messageType, msg)
				}
			}
			break
		case OnCamera:
			macAddr := string(payload)
			stackChanClient := getStackChanClient(macAddr)
			if stackChanClient != nil {
				if added, _ := stackChanClient.SubscribeCamera(client); added {
					stackChanSendMessage(ctx, stackChanClient, messageType, msg)
				}
			}
			break
		case OffCamera:
			macAddr := string(payload)
			stackChanClient := getStackChanClient(macAddr)
			if stackChanClient != nil {
				existed, empty := stackChanClient.UnsubscribeCamera(client)
				shouldNotify := existed && empty
				if shouldNotify {
					stackChanSendMessage(ctx, stackChanClient, messageType, msg)
				}
			}
			break
		case OnPhoneScreen:
			// Show phone screen
			macAddr := string(payload)
			stackChanClient := getStackChanClient(macAddr)
			if stackChanClient != nil {
				if stackChanClient.GetPhoneScreen() == false {
					stackChanClient.SetPhoneScreen(true)
					stackChanSendMessage(ctx, stackChanClient, messageType, msg)
				}
			}
			break
		case OffPhoneScreen:
			// Hide phone screen
			macAddr := string(payload)
			stackChanClient := getStackChanClient(macAddr)
			if stackChanClient != nil {
				if stackChanClient.GetPhoneScreen() == true {
					stackChanClient.SetPhoneScreen(false)
					stackChanSendMessage(ctx, stackChanClient, messageType, msg)
				}
			}
			break
		case Dance:
			// Dance message
			stackChanClient := getStackChanClient(client.GetMac())
			if stackChanClient != nil {
				stackChanSendMessage(ctx, stackChanClient, messageType, msg)
			}
			break
		case GetAvatarPosture:
			stackChanClient := getStackChanClient(client.GetMac())
			if stackChanClient != nil {
				stackChanSendMessage(ctx, stackChanClient, messageType, msg)
			}
		case AimedTakePhoto:
			stackChanClient := getStackChanClient(client.GetMac())
			if stackChanClient != nil {
				stackChanClient.SetAimedTakePhotoAppClient(client)
				stackChanSendMessage(ctx, stackChanClient, messageType, msg)
			}
			break
		default:
			logger.Infof(ctx, "Unknown binary msgType: %d", msgType)
			stackChanClient := getStackChanClient(client.GetMac())
			if stackChanClient != nil {
				stackChanSendMessage(ctx, stackChanClient, messageType, msg)
			}
		}
	} else if *messageType == websocket.TextMessage {
		// Directly forward other message types
		stackChanClient := getStackChanClient(client.GetMac())
		if stackChanClient != nil {
			stackChanSendMessage(ctx, stackChanClient, messageType, msg)
		}
	} else if *messageType == websocket.PingMessage {
		logger.Info(ctx, "Received ping message from App side")
	}
}

// Send WebSocket messages to App clients
func appSendMessage(ctx context.Context, client *model.AppClient, messageType *int, msg *[]byte) {
	if client == nil || messageType == nil || msg == nil {
		logger.Warningf(ctx, "Cannot queue App message with nil client or payload")
		return
	}
	expectedConn := client.GetConn()
	highPriority := isControlQueueMessage(*messageType, *msg)
	if !client.TrySend(&model.WsSendMsg{
		MsgType:      *messageType,
		Data:         *msg,
		HighPriority: highPriority,
	}) && highPriority {
		logger.Warningf(ctx, "App client control queue is unavailable or full; closing connection for resync")
		// Preserve the client identity and writer so reconnect registration can
		// reuse its server-side subscriptions and current state.
		if conn := client.DetachConnForResync(expectedConn); conn != nil {
			if err := conn.Close(); err != nil {
				logger.Debugf(ctx, "Close overloaded App client connection: %v", err)
			}
		}
	}
}

// Send WebSocket messages to StackChan devices
func stackChanSendMessage(ctx context.Context, client *model.StackChanClient, messageType *int, msg *[]byte) {
	if client == nil || messageType == nil || msg == nil {
		logger.Warningf(ctx, "Cannot queue StackChan message with nil client or payload")
		return
	}
	expectedConn := client.GetConn()
	highPriority := isControlQueueMessage(*messageType, *msg)
	if !client.TrySend(&model.WsSendMsg{
		MsgType:      *messageType,
		Data:         *msg,
		HighPriority: highPriority,
	}) && highPriority {
		logger.Warningf(ctx, "StackChan client control queue is unavailable or full; closing connection for resync")
		// Preserve the client identity and writer so reconnect registration can
		// reuse its server-side subscriptions and current state.
		if conn := client.DetachConnForResync(expectedConn); conn != nil {
			if err := conn.Close(); err != nil {
				logger.Debugf(ctx, "Close overloaded StackChan client connection: %v", err)
			}
		}
	}
}

// High-rate media and motion traffic is intentionally lossy when a client
// cannot keep up. Lower-frequency control traffic uses a separate queue so a
// media backlog cannot suppress stop, state, or connection messages.
func isControlQueueMessage(messageType int, msg []byte) bool {
	if messageType != websocket.BinaryMessage || len(msg) == 0 {
		return true
	}
	switch msg[0] {
	case Opus, Jpeg, ControlAvatar, ControlMotion:
		return false
	default:
		return true
	}
}

// SendAppMessage Send WebSocket messages to App clients
func SendAppMessage(ctx context.Context, mac string, messageType *int, msg *[]byte, supportOfflineMode *bool) {
	clients := getAppClients(mac)
	if clients != nil {
		for _, client := range clients {
			appSendMessage(ctx, client, messageType, msg)
		}
	}
}

// SendStackChanMessage Send WebSocket messages to StackChan devices
func SendStackChanMessage(ctx context.Context, mac string, messageType *int, msg *[]byte, supportOfflineMode *bool) {
	stackChanClient := getStackChanClient(mac)
	if stackChanClient != nil {
		stackChanSendMessage(ctx, stackChanClient, messageType, msg)
	}
}

// Encapsulate binary messages for custom protocol (type + data length + data)
func createMessage(msgType byte, data []byte) *[]byte {
	var dataLen int
	if data != nil {
		dataLen = len(data)
	} else {
		dataLen = 0
	}
	msg := make([]byte, 1+4+dataLen)
	msg[0] = msgType
	binary.BigEndian.PutUint32(msg[1:5], uint32(dataLen))
	if dataLen > 0 {
		copy(msg[5:], data)
	}
	return &msg
}

// Encapsulate binary messages for custom protocol (type + data length + string data)
func createStringMessage(msgType byte, data string) *[]byte {
	return createMessage(msgType, []byte(data))
}
