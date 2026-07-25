/*
SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
SPDX-License-Identifier: MIT
*/

package model

import (
	"context"
	"sync"
	"testing"

	"github.com/gorilla/websocket"
)

func TestTrySendReplacesStaleMediaAndRejectsStoppedClients(t *testing.T) {
	t.Parallel()

	appCtx, appCancel := context.WithCancel(context.Background())
	app := &AppClient{
		sendChan: make(chan *WsSendMsg, 1),
		ctx:      appCtx,
		cancel:   appCancel,
	}
	message := &WsSendMsg{MsgType: websocket.BinaryMessage, Data: []byte("test")}
	if !app.TrySend(message) {
		t.Fatal("AppClient.TrySend rejected an available queue")
	}
	replacement := &WsSendMsg{MsgType: websocket.BinaryMessage, Data: []byte("latest")}
	if !app.TrySend(replacement) {
		t.Fatal("AppClient.TrySend rejected replacement media")
	}
	app.CloseWriterCoroutine()
	if app.TrySend(message) {
		t.Fatal("AppClient.TrySend accepted a message after shutdown")
	}
	if got := <-app.sendChan; got != replacement {
		t.Fatal("AppClient did not replace stale queued media")
	}

	stackCtx, stackCancel := context.WithCancel(context.Background())
	stack := &StackChanClient{
		sendChan: make(chan *WsSendMsg, 1),
		ctx:      stackCtx,
		cancel:   stackCancel,
	}
	if !stack.TrySend(message) {
		t.Fatal("StackChanClient.TrySend rejected an available queue")
	}
	stack.CloseWriterCoroutine()
	if stack.TrySend(message) {
		t.Fatal("StackChanClient.TrySend accepted a message after shutdown")
	}
	if got := <-stack.sendChan; got != message {
		t.Fatal("StackChanClient queue was closed or changed during shutdown")
	}
}

func TestTrySendKeepsControlTrafficSeparateFromFullMediaQueue(t *testing.T) {
	t.Parallel()

	ctx, cancel := context.WithCancel(context.Background())
	client := &AppClient{
		sendChan:    make(chan *WsSendMsg, 1),
		controlChan: make(chan *WsSendMsg, 1),
		ctx:         ctx,
		cancel:      cancel,
	}
	media := &WsSendMsg{MsgType: websocket.BinaryMessage, Data: []byte("media")}
	control := &WsSendMsg{
		MsgType:      websocket.BinaryMessage,
		Data:         []byte("control"),
		HighPriority: true,
	}

	if !client.TrySend(media) {
		t.Fatal("media queue rejected an available slot")
	}
	if !client.TrySend(control) {
		t.Fatal("full media queue blocked control traffic")
	}
	if client.TrySend(control) {
		t.Fatal("control queue did not enforce its independent capacity")
	}
	if got := <-client.controlChan; got != control {
		t.Fatal("control message was queued on the wrong channel")
	}
	client.CloseWriterCoroutine()
}

func TestControlQueueFailureCanDetachForResync(t *testing.T) {
	t.Parallel()

	appCtx, appCancel := context.WithCancel(context.Background())
	appConn := &websocket.Conn{}
	app := &AppClient{
		conn:        appConn,
		sendChan:    make(chan *WsSendMsg, 1),
		controlChan: make(chan *WsSendMsg, 1),
		ctx:         appCtx,
		cancel:      appCancel,
	}
	control := &WsSendMsg{HighPriority: true}
	if !app.TrySend(control) || app.TrySend(control) {
		t.Fatal("AppClient control queue did not report its full state")
	}
	if detached := app.DetachConnForResync(appConn); detached != appConn {
		t.Fatal("AppClient did not return the connection detached for resync")
	}
	if app.GetConn() != nil || app.IsStopped() {
		t.Fatal("AppClient writer state changed while detaching for resync")
	}
	select {
	case <-appCtx.Done():
		t.Fatal("AppClient writer context was canceled while detaching for resync")
	default:
	}
	<-app.controlChan
	if !app.TrySend(control) {
		t.Fatal("AppClient writer could not be reused after detaching for resync")
	}
	app.CloseWriterCoroutine()

	stackCtx, stackCancel := context.WithCancel(context.Background())
	stackConn := &websocket.Conn{}
	stack := &StackChanClient{
		conn:        stackConn,
		sendChan:    make(chan *WsSendMsg, 1),
		controlChan: make(chan *WsSendMsg, 1),
		ctx:         stackCtx,
		cancel:      stackCancel,
	}
	if !stack.TrySend(control) || stack.TrySend(control) {
		t.Fatal("StackChanClient control queue did not report its full state")
	}
	if detached := stack.DetachConnForResync(stackConn); detached != stackConn {
		t.Fatal("StackChanClient did not return the connection detached for resync")
	}
	if stack.GetConn() != nil || stack.IsStopped() {
		t.Fatal("StackChanClient writer state changed while detaching for resync")
	}
	select {
	case <-stackCtx.Done():
		t.Fatal("StackChanClient writer context was canceled while detaching for resync")
	default:
	}
	<-stack.controlChan
	if !stack.TrySend(control) {
		t.Fatal("StackChanClient writer could not be reused after detaching for resync")
	}
	stack.CloseWriterCoroutine()
}

func TestAppClientDetachConnForResyncPreservesReplacement(t *testing.T) {
	t.Parallel()

	oldConn := &websocket.Conn{}
	newConn := &websocket.Conn{}
	client := &AppClient{conn: oldConn}

	expectedConn := client.GetConn()
	if replaced := client.ReplaceConn(newConn); replaced != oldConn {
		t.Fatal("AppClient did not return the replaced connection")
	}
	if detached := client.DetachConnForResync(expectedConn); detached != nil {
		t.Fatal("AppClient detached a replacement connection")
	}
	if got := client.GetConn(); got != newConn {
		t.Fatal("AppClient lost its replacement connection")
	}

	nilExpectedClient := &AppClient{}
	nilExpected := nilExpectedClient.GetConn()
	nilExpectedClient.ReplaceConn(newConn)
	if detached := nilExpectedClient.DetachConnForResync(nilExpected); detached != nil {
		t.Fatal("AppClient detached a connection after observing nil")
	}
	if got := nilExpectedClient.GetConn(); got != newConn {
		t.Fatal("AppClient lost a connection installed after observing nil")
	}
}

func TestStackChanClientDetachConnForResyncPreservesReplacement(t *testing.T) {
	t.Parallel()

	oldConn := &websocket.Conn{}
	newConn := &websocket.Conn{}
	client := &StackChanClient{conn: oldConn}

	expectedConn := client.GetConn()
	if replaced := client.ReplaceConn(newConn); replaced != oldConn {
		t.Fatal("StackChanClient did not return the replaced connection")
	}
	if detached := client.DetachConnForResync(expectedConn); detached != nil {
		t.Fatal("StackChanClient detached a replacement connection")
	}
	if got := client.GetConn(); got != newConn {
		t.Fatal("StackChanClient lost its replacement connection")
	}

	nilExpectedClient := &StackChanClient{}
	nilExpected := nilExpectedClient.GetConn()
	nilExpectedClient.ReplaceConn(newConn)
	if detached := nilExpectedClient.DetachConnForResync(nilExpected); detached != nil {
		t.Fatal("StackChanClient detached a connection after observing nil")
	}
	if got := nilExpectedClient.GetConn(); got != newConn {
		t.Fatal("StackChanClient lost a connection installed after observing nil")
	}
}

func TestNextQueuedMessageBoundsControlBurst(t *testing.T) {
	t.Parallel()

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	control := make(chan *WsSendMsg, controlBurstLimit+1)
	media := make(chan *WsSendMsg, 1)
	controlMessage := &WsSendMsg{HighPriority: true}
	mediaMessage := &WsSendMsg{}
	for range controlBurstLimit + 1 {
		control <- controlMessage
	}
	media <- mediaMessage

	controlBurst := 0
	for range controlBurstLimit {
		got, fromControl, ok := nextQueuedMessage(
			ctx,
			control,
			media,
			controlBurst >= controlBurstLimit,
		)
		if !ok || !fromControl || got != controlMessage {
			t.Fatal("control traffic was not prioritized within the burst limit")
		}
		controlBurst++
	}

	got, fromControl, ok := nextQueuedMessage(
		ctx,
		control,
		media,
		controlBurst >= controlBurstLimit,
	)
	if !ok || fromControl || got != mediaMessage {
		t.Fatal("continuous control traffic starved queued media")
	}
}

func TestAppClientClearConnIfDoesNotClearReplacement(t *testing.T) {
	t.Parallel()

	oldConn := &websocket.Conn{}
	newConn := &websocket.Conn{}
	client := &AppClient{conn: oldConn}

	if previous := client.ReplaceConn(newConn); previous != oldConn {
		t.Fatal("ReplaceConn did not return the previous connection")
	}
	if client.ClearConnIf(oldConn) {
		t.Fatal("ClearConnIf cleared a replacement connection")
	}
	if got := client.GetConn(); got != newConn {
		t.Fatal("replacement connection was lost")
	}
	if !client.ClearConnIf(newConn) || client.GetConn() != nil {
		t.Fatal("ClearConnIf did not clear the matching connection")
	}
}

func TestStackChanClientClearConnIfDoesNotClearReplacement(t *testing.T) {
	t.Parallel()

	oldConn := &websocket.Conn{}
	newConn := &websocket.Conn{}
	client := &StackChanClient{conn: oldConn}

	if previous := client.ReplaceConn(newConn); previous != oldConn {
		t.Fatal("ReplaceConn did not return the previous connection")
	}
	if client.ClearConnIf(oldConn) {
		t.Fatal("ClearConnIf cleared a replacement connection")
	}
	if got := client.GetConn(); got != newConn {
		t.Fatal("replacement connection was lost")
	}
}

func TestAddAudioSubscriptionIfAbsent(t *testing.T) {
	t.Parallel()

	stackChan := &StackChanClient{}
	app := &AppClient{}
	if !stackChan.AddAudioSubscriptionIfAbsent(app) {
		t.Fatal("first subscription was not added")
	}
	if stackChan.AddAudioSubscriptionIfAbsent(app) {
		t.Fatal("duplicate subscription was added")
	}
	if got := len(stackChan.GetAudioSubscriptionList()); got != 1 {
		t.Fatalf("subscription count = %d, want 1", got)
	}
}

func TestSubscriptionTransitions(t *testing.T) {
	t.Parallel()

	stackChan := &StackChanClient{}
	firstApp := &AppClient{}
	secondApp := &AppClient{}

	if added, first := stackChan.SubscribeCamera(firstApp); !added || !first {
		t.Fatalf("first camera subscription = (%v, %v), want (true, true)", added, first)
	}
	if added, first := stackChan.SubscribeCamera(firstApp); added || first {
		t.Fatalf("duplicate camera subscription = (%v, %v), want (false, false)", added, first)
	}
	if added, first := stackChan.SubscribeCamera(secondApp); !added || first {
		t.Fatalf("second camera subscription = (%v, %v), want (true, false)", added, first)
	}
	if removed, empty := stackChan.UnsubscribeCamera(firstApp); !removed || empty {
		t.Fatalf("non-final camera removal = (%v, %v), want (true, false)", removed, empty)
	}
	if removed, empty := stackChan.UnsubscribeCamera(secondApp); !removed || !empty {
		t.Fatalf("final camera removal = (%v, %v), want (true, true)", removed, empty)
	}

	if added, first := stackChan.SubscribeAudio(firstApp); !added || !first {
		t.Fatalf("first audio subscription = (%v, %v), want (true, true)", added, first)
	}
	if added, first := stackChan.SubscribeAudio(firstApp); added || first {
		t.Fatalf("duplicate audio subscription = (%v, %v), want (false, false)", added, first)
	}
	if removed, empty := stackChan.UnsubscribeAudio(firstApp); !removed || !empty {
		t.Fatalf("final audio removal = (%v, %v), want (true, true)", removed, empty)
	}
}

func TestConcurrentSubscriptionUpdatesAreAtomic(t *testing.T) {
	t.Parallel()

	const clientCount = 128
	stackChan := &StackChanClient{}
	apps := make([]*AppClient, clientCount)
	for i := range apps {
		apps[i] = &AppClient{}
	}

	var addWG sync.WaitGroup
	addWG.Add(clientCount * 2)
	for _, app := range apps {
		go func() {
			defer addWG.Done()
			stackChan.SubscribeCamera(app)
		}()
		go func() {
			defer addWG.Done()
			stackChan.SubscribeAudio(app)
		}()
	}
	addWG.Wait()

	if got := len(stackChan.GetCameraSubscriptionList()); got != clientCount {
		t.Fatalf("camera subscription count = %d, want %d", got, clientCount)
	}
	if got := len(stackChan.GetAudioSubscriptionList()); got != clientCount {
		t.Fatalf("audio subscription count = %d, want %d", got, clientCount)
	}

	var removeWG sync.WaitGroup
	var cameraFinalCount int
	var audioFinalCount int
	var countMu sync.Mutex
	removeWG.Add(clientCount * 2)
	for _, app := range apps {
		go func() {
			defer removeWG.Done()
			removed, empty := stackChan.UnsubscribeCamera(app)
			if removed && empty {
				countMu.Lock()
				cameraFinalCount++
				countMu.Unlock()
			}
		}()
		go func() {
			defer removeWG.Done()
			removed, empty := stackChan.UnsubscribeAudio(app)
			if removed && empty {
				countMu.Lock()
				audioFinalCount++
				countMu.Unlock()
			}
		}()
	}
	removeWG.Wait()

	if got := len(stackChan.GetCameraSubscriptionList()); got != 0 {
		t.Fatalf("camera subscription count after removal = %d, want 0", got)
	}
	if got := len(stackChan.GetAudioSubscriptionList()); got != 0 {
		t.Fatalf("audio subscription count after removal = %d, want 0", got)
	}
	if cameraFinalCount != 1 {
		t.Fatalf("camera final transition count = %d, want 1", cameraFinalCount)
	}
	if audioFinalCount != 1 {
		t.Fatalf("audio final transition count = %d, want 1", audioFinalCount)
	}
}

func TestSubscriptionListAccessUsesCopies(t *testing.T) {
	t.Parallel()

	stackChan := &StackChanClient{}
	original := &AppClient{}
	replacement := &AppClient{}

	cameraInput := []*AppClient{original}
	stackChan.SetCameraSubscriptionList(cameraInput)
	cameraInput[0] = replacement
	cameraSnapshot := stackChan.GetCameraSubscriptionList()
	if cameraSnapshot[0] != original {
		t.Fatal("camera setter retained the caller's backing slice")
	}
	cameraSnapshot[0] = replacement
	if stackChan.GetCameraSubscriptionList()[0] != original {
		t.Fatal("camera getter exposed the internal backing slice")
	}

	audioInput := []*AppClient{original}
	stackChan.SetAudioSubscriptionList(audioInput)
	audioInput[0] = replacement
	audioSnapshot := stackChan.GetAudioSubscriptionList()
	if audioSnapshot[0] != original {
		t.Fatal("audio setter retained the caller's backing slice")
	}
	audioSnapshot[0] = replacement
	if stackChan.GetAudioSubscriptionList()[0] != original {
		t.Fatal("audio getter exposed the internal backing slice")
	}
}
