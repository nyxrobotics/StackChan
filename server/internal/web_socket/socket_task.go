/*
SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
SPDX-License-Identifier: MIT
*/

package web_socket

import (
	"context"
	"math/rand"
	"stackChan/internal/model"
	"sync"
	"time"

	"github.com/gorilla/websocket"
)

var (
	randMu sync.Mutex
	rander = rand.New(rand.NewSource(time.Now().UnixNano()))
)

const (
	ClientExpireTimeout = 15 * time.Second
)

type expiredAppClient struct {
	client *model.AppClient
	conn   *websocket.Conn
}

type expiredStackChanClient struct {
	mac    string
	client *model.StackChanClient
	conn   *websocket.Conn
}

// StartPingTime sends Ping messages to all connected clients for heartbeat detection
func StartPingTime(ctx context.Context) {
	// Global panic recovery, prevent entire heartbeat detection logic from crashing
	defer func() {
		if r := recover(); r != nil {
			logger.Errorf(ctx, "StartPingTime panic recovered: %v", r)
		}
	}()

	message := createMessage(ping, nil)
	messageType := websocket.BinaryMessage

	// Iterate over StackChanClientPool
	stackChanClientPool.Range(func(_, value any) bool {
		if value == nil {
			return true
		}
		client, ok := value.(*model.StackChanClient)
		if !ok {
			logger.Warningf(ctx, "StartPingTime: invalid type in StackChanClientPool, skip")
			return true
		}
		if client == nil {
			return true
		}

		func() {
			defer func() {
				if r := recover(); r != nil {
					logger.Errorf(ctx, "panic in StartPingTime StackChanClientPool forwardMessage: %v", r)
				}
			}()
			if client.GetConn() == nil {
				logger.Debugf(ctx, "StartPingTime: StackChanClient %s has nil conn, skip ping", client.GetMac())
				return
			}
			stackChanSendMessage(ctx, client, &messageType, message)
		}()
		return true // continue iteration
	})

	// Iterate over an immutable snapshot of AppClientPool.
	for _, entry := range getAppClientPoolSnapshot() {
		if !entry.valid {
			logger.Warningf(ctx, "StartPingTime: invalid type in AppClientPool for key %v, skip", entry.key)
			continue
		}
		clients := entry.clients
		if len(clients) == 0 {
			continue
		}

		for _, client := range clients {
			func() {
				defer func() {
					if r := recover(); r != nil {
						logger.Errorf(ctx, "panic in StartPingTime AppClientPool forwardMessage: %v", r)
					}
				}()
				if client == nil {
					return
				}
				if client.GetConn() == nil {
					logger.Debugf(ctx, "StartPingTime: AppClient %s (deviceId: %s) has nil conn, skip ping", client.GetMac(), client.GetDeviceId())
					return
				}
				appSendMessage(ctx, client, &messageType, message)
			}()
		}
	}
}

// CheckExpiredLinks cleans up clients that have exceeded ClientExpireTimeout.
func CheckExpiredLinks(ctx context.Context) {
	defer func() {
		if r := recover(); r != nil {
			logger.Errorf(ctx, "CheckExpiredLinks panic recovered: %v", r)
		}
	}()

	now := time.Now()
	expiredClients := removeExpiredAppClients(ctx, now)

	for _, expiredClient := range expiredClients {
		client := expiredClient.client
		if client == nil {
			continue
		}

		stackChanClientPool.Range(func(_, scValue any) bool {
			defer func() {
				if r := recover(); r != nil {
					logger.Errorf(ctx, "Clean StackChanClient panic: %v", r)
				}
			}()
			stackChanClient, ok := scValue.(*model.StackChanClient)
			if !ok || stackChanClient == nil {
				return true
			}
			if stackChanClient.GetCallAppClient() == client {
				stackChanClient.SetCallAppClient(nil)
			}

			removedCamera, cameraSubscriptionsEmpty := stackChanClient.UnsubscribeCamera(client)
			if removedCamera && cameraSubscriptionsEmpty && stackChanClient.GetConn() != nil {
				msg := createMessage(OffCamera, nil)
				msgType := websocket.BinaryMessage
				stackChanSendMessage(ctx, stackChanClient, &msgType, msg)
			}

			removedAudio, audioSubscriptionsEmpty := stackChanClient.UnsubscribeAudio(client)
			if removedAudio && audioSubscriptionsEmpty && stackChanClient.GetConn() != nil {
				msg := createMessage(OffAudio, nil)
				msgType := websocket.BinaryMessage
				stackChanSendMessage(ctx, stackChanClient, &msgType, msg)
			}
			return true
		})

		logger.Infof(ctx, "Kicked out expired App client: %s", client.GetMac())
		func() {
			defer func() {
				if r := recover(); r != nil {
					logger.Errorf(ctx, "Close AppClient conn panic: %v", r)
				}
			}()
			if expiredClient.conn != nil {
				_ = expiredClient.conn.Close()
			}
		}()
	}

	var expiredStackChanClients []expiredStackChanClient
	func() {
		stackChanClientMu.Lock()
		defer stackChanClientMu.Unlock()

		stackChanClientPool.Range(func(mac, value any) bool {
			macStr, validMac := mac.(string)
			stackChanClient, validClient := value.(*model.StackChanClient)
			if !validMac || !validClient || stackChanClient == nil {
				logger.Warningf(ctx, "StackChanClientPool invalid entry for mac: %v, delete invalid entry", mac)
				stackChanClientPool.Delete(mac)
				return true
			}
			conn, expired := stackChanClient.ExpireIfInactive(now, ClientExpireTimeout)
			if !expired {
				return true
			}
			stackChanClientPool.Delete(mac)
			expiredStackChanClients = append(expiredStackChanClients, expiredStackChanClient{
				mac:    macStr,
				client: stackChanClient,
				conn:   conn,
			})
			return true
		})
	}()

	for _, expiredClient := range expiredStackChanClients {
		mac := expiredClient.mac
		stackChanClient := expiredClient.client

		// Keep registration blocked until the offline messages are queued. This
		// guarantees that a reconnect's online message is not overtaken by stale
		// cleanup from the previous connection.
		func() {
			stackChanClientMu.Lock()
			defer stackChanClientMu.Unlock()

			current, reconnected := stackChanClientPool.Load(mac)
			reconnected = reconnected && current != stackChanClient
			if !reconnected {
				offlineMsg := createStringMessage(DeviceOffline, "Your StackChan is offline.")
				msgType := websocket.BinaryMessage
				appClients := getAppClients(stackChanClient.GetMac())
				for _, appClient := range appClients {
					if appClient == nil {
						continue
					}
					func() {
						defer func() {
							if r := recover(); r != nil {
								logger.Errorf(ctx, "Notify AppClient offline panic: %v", r)
							}
						}()
						appSendMessage(ctx, appClient, &msgType, offlineMsg)
					}()
				}
			}
		}()

		logger.Infof(ctx, "Kicked out expired StackChan client: %s", mac)

		if expiredClient.conn != nil {
			func() {
				defer func() {
					if r := recover(); r != nil {
						logger.Errorf(ctx, "Close StackChan conn panic: %v", r)
					}
				}()
				_ = expiredClient.conn.Close()
			}()
		}
	}
}

func removeExpiredAppClients(ctx context.Context, now time.Time) []expiredAppClient {
	appClientMu.Lock()
	defer appClientMu.Unlock()

	var expiredClients []expiredAppClient
	appClientPool.Range(func(mac, value any) bool {
		if mac == nil || value == nil {
			return true
		}
		clients, ok := value.([]*model.AppClient)
		if !ok {
			logger.Warningf(ctx, "AppClientPool invalid type for mac: %v, delete invalid entry", mac)
			appClientPool.Delete(mac)
			return true
		}

		newClients := make([]*model.AppClient, 0, len(clients))
		for _, client := range clients {
			if client == nil {
				continue
			}
			conn, expired := client.ExpireIfInactive(now, ClientExpireTimeout)
			if expired {
				expiredClients = append(expiredClients, expiredAppClient{
					client: client,
					conn:   conn,
				})
				continue
			}
			newClients = append(newClients, client)
		}

		if len(newClients) == 0 {
			appClientPool.Delete(mac)
		} else {
			appClientPool.Store(mac, newClients)
		}
		return true
	})
	return expiredClients
}

// GetRandomStackChanDevice get Random StackChan Device list
func GetRandomStackChanDevice(userMac string, maxLength int) (list []string) {
	if maxLength <= 0 {
		return []string{}
	}
	var macs []string

	stackChanClientPool.Range(func(key, value interface{}) bool {
		mac, validMac := key.(string)
		client, validClient := value.(*model.StackChanClient)
		if !validMac || !validClient || client == nil {
			logger.Warningf(context.Background(), "GetRandomStackChanDevice: invalid pool entry for key %v", key)
			return true
		}

		if mac == userMac {
			return true
		}
		online := client.GetConn() != nil
		if online {
			macs = append(macs, mac)
		}

		return true
	})

	if len(macs) == 0 {
		return []string{}
	}

	randMu.Lock()
	rander.Shuffle(len(macs), func(i, j int) {
		macs[i], macs[j] = macs[j], macs[i]
	})
	randMu.Unlock()
	if len(macs) > maxLength {
		macs = macs[:maxLength]
	}

	return macs
}
