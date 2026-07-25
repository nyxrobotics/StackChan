/*
SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
SPDX-License-Identifier: MIT
*/

package model

import (
	"context"
	"sync"
	"sync/atomic"
	"time"

	"github.com/gogf/gf/v2/frame/g"
	"github.com/gorilla/websocket"
)

type WsSendMsg struct {
	MsgType      int
	Data         []byte
	HighPriority bool
}

type AppClient struct {
	mac      string
	conn     *websocket.Conn
	mu       sync.RWMutex
	deviceId string
	lastTime time.Time

	sendChan    chan *WsSendMsg
	controlChan chan *WsSendMsg
	queueMu     sync.Mutex
	ctx         context.Context
	cancel      context.CancelFunc
	stopped     atomic.Bool
}

type StackChanClient struct {
	mac                     string
	conn                    *websocket.Conn
	mu                      sync.RWMutex
	cameraSubscriptionList  []*AppClient
	audioSubscriptionList   []*AppClient
	callAppClient           *AppClient
	aimedTakePhotoAppClient *AppClient
	phoneScreen             bool
	lastTime                time.Time

	sendChan    chan *WsSendMsg
	controlChan chan *WsSendMsg
	queueMu     sync.Mutex
	ctx         context.Context
	cancel      context.CancelFunc
	stopped     atomic.Bool
}

const (
	websocketWriteTimeout = 5 * time.Second
	controlQueueCapacity  = 32
	mediaQueueCapacity    = 100
	controlBurstLimit     = 8
)

// NewAppClient creates and initializes an AppClient
func NewAppClient(mac string, conn *websocket.Conn, deviceId string) *AppClient {
	ctx, cancel := context.WithCancel(context.Background())
	client := &AppClient{
		mac:         mac,
		conn:        conn,
		deviceId:    deviceId,
		lastTime:    time.Now(),
		sendChan:    make(chan *WsSendMsg, mediaQueueCapacity),
		controlChan: make(chan *WsSendMsg, controlQueueCapacity),
		ctx:         ctx,
		cancel:      cancel,
	}
	client.StartWriterCoroutine()
	return client
}

// NewStackChanClient creates and initializes a StackChanClient
func NewStackChanClient(mac string, conn *websocket.Conn, cameraSubscriptionList []*AppClient, callAppClient *AppClient, phoneScreen bool) *StackChanClient {
	ctx, cancel := context.WithCancel(context.Background())
	client := &StackChanClient{
		mac:                    mac,
		conn:                   conn,
		cameraSubscriptionList: cameraSubscriptionList,
		callAppClient:          callAppClient,
		phoneScreen:            phoneScreen,
		lastTime:               time.Now(),
		sendChan:               make(chan *WsSendMsg, mediaQueueCapacity),
		controlChan:            make(chan *WsSendMsg, controlQueueCapacity),
		ctx:                    ctx,
		cancel:                 cancel,
	}
	client.StartWriterCoroutine()
	return client
}

// StartWriterCoroutine AppClient Start message sending coroutine
func (a *AppClient) StartWriterCoroutine() {
	go func() {
		defer func() {
			if r := recover(); r != nil {
				g.Log().Errorf(context.Background(), "AppClient writer coroutine panic: %v", r)
			}
			a.CloseWriterCoroutine()
		}()

		controlBurst := 0
		for {
			msg, fromControl, ok := nextQueuedMessage(
				a.ctx,
				a.controlChan,
				a.sendChan,
				controlBurst >= controlBurstLimit,
			)
			if !ok {
				return
			}
			a.writeQueuedMessage(msg)
			if fromControl {
				controlBurst++
			} else {
				controlBurst = 0
			}
		}
	}()
}

// StartWriterCoroutine StackChanClient Start message sending coroutine
func (s *StackChanClient) StartWriterCoroutine() {
	go func() {
		defer func() {
			if r := recover(); r != nil {
				g.Log().Errorf(context.Background(), "StackChan writer coroutine panic: %v", r)
			}
			s.CloseWriterCoroutine()
		}()
		controlBurst := 0
		for {
			msg, fromControl, ok := nextQueuedMessage(
				s.ctx,
				s.controlChan,
				s.sendChan,
				controlBurst >= controlBurstLimit,
			)
			if !ok {
				return
			}
			s.writeQueuedMessage(msg)
			if fromControl {
				controlBurst++
			} else {
				controlBurst = 0
			}
		}
	}()
}

func nextQueuedMessage(
	ctx context.Context,
	control <-chan *WsSendMsg,
	media <-chan *WsSendMsg,
	forceMedia bool,
) (msg *WsSendMsg, fromControl bool, ok bool) {
	if forceMedia {
		select {
		case <-ctx.Done():
			return nil, false, false
		case msg, ok = <-media:
			return msg, false, ok
		default:
		}
	}

	select {
	case <-ctx.Done():
		return nil, false, false
	case msg, ok = <-control:
		return msg, true, ok
	default:
	}

	select {
	case <-ctx.Done():
		return nil, false, false
	case msg, ok = <-control:
		return msg, true, ok
	case msg, ok = <-media:
		return msg, false, ok
	}
}

func (a *AppClient) writeQueuedMessage(msg *WsSendMsg) {
	if msg == nil {
		return
	}

	var conn *websocket.Conn
	defer func() {
		if recovered := recover(); recovered != nil {
			g.Log().Errorf(context.Background(), "AppClient write panic: %v", recovered)
			if conn != nil {
				a.ClearConnIf(conn)
				_ = conn.Close()
			}
		}
	}()

	a.mu.RLock()
	conn = a.conn
	a.mu.RUnlock()
	if conn == nil {
		return
	}
	if err := conn.SetWriteDeadline(time.Now().Add(websocketWriteTimeout)); err != nil {
		g.Log().Errorf(context.Background(), "AppClient set write deadline error: %v", err)
		a.ClearConnIf(conn)
		_ = conn.Close()
		return
	}
	if err := conn.WriteMessage(msg.MsgType, msg.Data); err != nil {
		g.Log().Errorf(context.Background(), "AppClient send message error: %v", err)
		a.ClearConnIf(conn)
		_ = conn.Close()
	}
}

func (s *StackChanClient) writeQueuedMessage(msg *WsSendMsg) {
	if msg == nil {
		return
	}

	var conn *websocket.Conn
	defer func() {
		if recovered := recover(); recovered != nil {
			g.Log().Errorf(context.Background(), "StackChan write panic: %v", recovered)
			if conn != nil {
				s.ClearConnIf(conn)
				_ = conn.Close()
			}
		}
	}()

	s.mu.RLock()
	conn = s.conn
	s.mu.RUnlock()
	if conn == nil {
		return
	}
	if err := conn.SetWriteDeadline(time.Now().Add(websocketWriteTimeout)); err != nil {
		g.Log().Errorf(context.Background(), "StackChan set write deadline error: %v", err)
		s.ClearConnIf(conn)
		_ = conn.Close()
		return
	}
	if err := conn.WriteMessage(msg.MsgType, msg.Data); err != nil {
		g.Log().Errorf(context.Background(), "StackChan writer coroutine send message error: %v", err)
		s.ClearConnIf(conn)
		_ = conn.Close()
	}
}

func (a *AppClient) CloseWriterCoroutine() {
	a.queueMu.Lock()
	defer a.queueMu.Unlock()
	if a.stopped.CompareAndSwap(false, true) && a.cancel != nil {
		a.cancel()
	}
}

func (s *StackChanClient) CloseWriterCoroutine() {
	s.queueMu.Lock()
	defer s.queueMu.Unlock()
	if s.stopped.CompareAndSwap(false, true) && s.cancel != nil {
		s.cancel()
	}
}

// DetachConnForResync atomically detaches expectedConn only if it is still the
// current connection. A nil or replaced connection is never detached. The
// caller owns the returned connection and must close it.
func (a *AppClient) DetachConnForResync(expectedConn *websocket.Conn) *websocket.Conn {
	if expectedConn == nil {
		return nil
	}
	a.mu.Lock()
	defer a.mu.Unlock()
	if a.conn != expectedConn {
		return nil
	}
	a.conn = nil
	return expectedConn
}

// DetachConnForResync atomically detaches expectedConn only if it is still the
// current connection. A nil or replaced connection is never detached. The
// caller owns the returned connection and must close it.
func (s *StackChanClient) DetachConnForResync(expectedConn *websocket.Conn) *websocket.Conn {
	if expectedConn == nil {
		return nil
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.conn != expectedConn {
		return nil
	}
	s.conn = nil
	return expectedConn
}

func (a *AppClient) IsStopped() bool {
	return a.stopped.Load()
}

func (s *StackChanClient) IsStopped() bool {
	return s.stopped.Load()
}

// TrySend queues a message without blocking. It returns false after shutdown
// or while the bounded queue is full.
func (a *AppClient) TrySend(msg *WsSendMsg) bool {
	a.queueMu.Lock()
	defer a.queueMu.Unlock()
	if msg == nil || a.stopped.Load() || a.sendChan == nil {
		return false
	}
	if msg.HighPriority && a.controlChan != nil {
		select {
		case a.controlChan <- msg:
			return true
		default:
			return false
		}
	}
	return enqueueLatest(a.sendChan, msg)
}

// TrySend queues a message without blocking. It returns false after shutdown
// or while the bounded queue is full.
func (s *StackChanClient) TrySend(msg *WsSendMsg) bool {
	s.queueMu.Lock()
	defer s.queueMu.Unlock()
	if msg == nil || s.stopped.Load() || s.sendChan == nil {
		return false
	}
	if msg.HighPriority && s.controlChan != nil {
		select {
		case s.controlChan <- msg:
			return true
		default:
			return false
		}
	}
	return enqueueLatest(s.sendChan, msg)
}

// enqueueLatest keeps bounded real-time traffic close to the present by
// discarding one queued stale frame when the media queue is full.
func enqueueLatest(queue chan *WsSendMsg, msg *WsSendMsg) bool {
	select {
	case queue <- msg:
		return true
	default:
	}

	select {
	case <-queue:
	default:
	}

	select {
	case queue <- msg:
		return true
	default:
		return false
	}
}

func (a *AppClient) SetMac(mac string) {
	a.mu.Lock()
	defer a.mu.Unlock()
	a.mac = mac
}

func (a *AppClient) GetMac() string {
	a.mu.RLock()
	defer a.mu.RUnlock()
	return a.mac
}

func (a *AppClient) GetConn() *websocket.Conn {
	a.mu.RLock()
	defer a.mu.RUnlock()
	return a.conn
}

func (a *AppClient) SetConn(conn *websocket.Conn) {
	a.mu.Lock()
	defer a.mu.Unlock()
	a.conn = conn
}

func (a *AppClient) ReplaceConn(conn *websocket.Conn) *websocket.Conn {
	a.mu.Lock()
	defer a.mu.Unlock()
	previous := a.conn
	a.conn = conn
	return previous
}

func (a *AppClient) ClearConnIf(conn *websocket.Conn) bool {
	a.mu.Lock()
	defer a.mu.Unlock()
	if a.conn != conn {
		return false
	}
	a.conn = nil
	return true
}

func (a *AppClient) SetDeviceId(deviceId string) {
	a.mu.Lock()
	defer a.mu.Unlock()
	a.deviceId = deviceId
}

func (a *AppClient) GetDeviceId() string {
	a.mu.RLock()
	defer a.mu.RUnlock()
	return a.deviceId
}

func (a *AppClient) SetLastTime(lastTime time.Time) {
	a.mu.Lock()
	defer a.mu.Unlock()
	a.lastTime = lastTime
}

func (a *AppClient) GetLastTime() time.Time {
	a.mu.RLock()
	defer a.mu.RUnlock()
	return a.lastTime
}

// ExpireIfInactive atomically claims an inactive client for cleanup. Once
// claimed, a reconnect must create a new client instance.
func (a *AppClient) ExpireIfInactive(now time.Time, timeout time.Duration) (*websocket.Conn, bool) {
	a.mu.Lock()
	if now.Sub(a.lastTime) <= timeout {
		a.mu.Unlock()
		return nil, false
	}
	conn := a.conn
	a.conn = nil
	cancel := a.cancel
	a.queueMu.Lock()
	a.stopped.Store(true)
	a.queueMu.Unlock()
	a.mu.Unlock()

	if cancel != nil {
		cancel()
	}
	return conn, true
}

func (s *StackChanClient) SetMac(mac string) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.mac = mac
}

func (s *StackChanClient) GetMac() string {
	s.mu.RLock()
	defer s.mu.RUnlock()
	return s.mac
}

func (s *StackChanClient) GetConn() *websocket.Conn {
	s.mu.RLock()
	defer s.mu.RUnlock()
	return s.conn
}

func (s *StackChanClient) SetConn(conn *websocket.Conn) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.conn = conn
}

func (s *StackChanClient) ReplaceConn(conn *websocket.Conn) *websocket.Conn {
	s.mu.Lock()
	defer s.mu.Unlock()
	previous := s.conn
	s.conn = conn
	return previous
}

func (s *StackChanClient) ClearConnIf(conn *websocket.Conn) bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.conn != conn {
		return false
	}
	s.conn = nil
	return true
}

func (s *StackChanClient) SetCameraSubscriptionList(cameraSubscriptionList []*AppClient) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.cameraSubscriptionList = append([]*AppClient(nil), cameraSubscriptionList...)
}

func (s *StackChanClient) AppendCameraSubscriptionList(appClient *AppClient) {
	_, _ = s.SubscribeCamera(appClient)
}

// SubscribeCamera adds appClient once and reports whether it was added and
// whether it became the first camera subscriber.
func (s *StackChanClient) SubscribeCamera(appClient *AppClient) (added bool, first bool) {
	if appClient == nil {
		return false, false
	}

	s.mu.Lock()
	defer s.mu.Unlock()

	for _, existing := range s.cameraSubscriptionList {
		if existing == appClient {
			return false, false
		}
	}
	s.cameraSubscriptionList = append(s.cameraSubscriptionList, appClient)
	return true, len(s.cameraSubscriptionList) == 1
}

// UnsubscribeCamera removes every occurrence of appClient atomically and
// reports whether it was present and whether the resulting list is empty.
func (s *StackChanClient) UnsubscribeCamera(appClient *AppClient) (removed bool, empty bool) {
	s.mu.Lock()
	defer s.mu.Unlock()

	if appClient == nil {
		return false, len(s.cameraSubscriptionList) == 0
	}

	newList := make([]*AppClient, 0, len(s.cameraSubscriptionList))
	for _, existing := range s.cameraSubscriptionList {
		if existing == appClient {
			removed = true
			continue
		}
		newList = append(newList, existing)
	}
	s.cameraSubscriptionList = newList
	return removed, len(newList) == 0
}

func (s *StackChanClient) GetCameraSubscriptionList() []*AppClient {
	s.mu.RLock()
	defer s.mu.RUnlock()
	out := make([]*AppClient, len(s.cameraSubscriptionList))
	copy(out, s.cameraSubscriptionList)
	return out
}

func (s *StackChanClient) SetAudioSubscriptionList(audioSubscriptionList []*AppClient) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.audioSubscriptionList = append([]*AppClient(nil), audioSubscriptionList...)
}

func (s *StackChanClient) AddAudioSubscriptionIfAbsent(appClient *AppClient) bool {
	added, _ := s.SubscribeAudio(appClient)
	return added
}

// SubscribeAudio adds appClient once and reports whether it was added and
// whether it became the first audio subscriber.
func (s *StackChanClient) SubscribeAudio(appClient *AppClient) (added bool, first bool) {
	if appClient == nil {
		return false, false
	}

	s.mu.Lock()
	defer s.mu.Unlock()

	for _, existing := range s.audioSubscriptionList {
		if existing == appClient {
			return false, false
		}
	}
	s.audioSubscriptionList = append(s.audioSubscriptionList, appClient)
	return true, len(s.audioSubscriptionList) == 1
}

// UnsubscribeAudio removes every occurrence of appClient atomically and
// reports whether it was present and whether the resulting list is empty.
func (s *StackChanClient) UnsubscribeAudio(appClient *AppClient) (removed bool, empty bool) {
	s.mu.Lock()
	defer s.mu.Unlock()

	if appClient == nil {
		return false, len(s.audioSubscriptionList) == 0
	}

	newList := make([]*AppClient, 0, len(s.audioSubscriptionList))
	for _, existing := range s.audioSubscriptionList {
		if existing == appClient {
			removed = true
			continue
		}
		newList = append(newList, existing)
	}
	s.audioSubscriptionList = newList
	return removed, len(newList) == 0
}

func (s *StackChanClient) GetAudioSubscriptionList() []*AppClient {
	s.mu.RLock()
	defer s.mu.RUnlock()
	out := make([]*AppClient, len(s.audioSubscriptionList))
	copy(out, s.audioSubscriptionList)
	return out
}

func (s *StackChanClient) SetCallAppClient(client *AppClient) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.callAppClient = client
}

func (s *StackChanClient) GetCallAppClient() *AppClient {
	s.mu.RLock()
	defer s.mu.RUnlock()
	return s.callAppClient
}

func (s *StackChanClient) GetAimedTakePhotoAppClient() *AppClient {
	s.mu.RLock()
	defer s.mu.RUnlock()
	return s.aimedTakePhotoAppClient
}

func (s *StackChanClient) SetAimedTakePhotoAppClient(aimedTakePhotoAppClient *AppClient) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.aimedTakePhotoAppClient = aimedTakePhotoAppClient
}

func (s *StackChanClient) GetPhoneScreen() bool {
	s.mu.RLock()
	defer s.mu.RUnlock()
	return s.phoneScreen
}

func (s *StackChanClient) SetPhoneScreen(phoneScreen bool) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.phoneScreen = phoneScreen
}

func (s *StackChanClient) GetLastTime() time.Time {
	s.mu.RLock()
	defer s.mu.RUnlock()
	return s.lastTime
}

func (s *StackChanClient) SetLastTime(lastTime time.Time) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.lastTime = lastTime
}

// ExpireIfInactive atomically claims an inactive client for cleanup. Once
// claimed, a reconnect must create a new client instance.
func (s *StackChanClient) ExpireIfInactive(now time.Time, timeout time.Duration) (*websocket.Conn, bool) {
	s.mu.Lock()
	if now.Sub(s.lastTime) <= timeout {
		s.mu.Unlock()
		return nil, false
	}
	conn := s.conn
	s.conn = nil
	cancel := s.cancel
	s.queueMu.Lock()
	s.stopped.Store(true)
	s.queueMu.Unlock()
	s.mu.Unlock()

	if cancel != nil {
		cancel()
	}
	return conn, true
}
