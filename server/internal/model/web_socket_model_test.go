/*
SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
SPDX-License-Identifier: MIT
*/

package model

import (
	"sync"
	"testing"

	"github.com/gorilla/websocket"
)

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
