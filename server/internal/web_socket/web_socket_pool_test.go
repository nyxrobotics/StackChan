/*
SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
SPDX-License-Identifier: MIT
*/

package web_socket

import (
	"context"
	"sync"
	"testing"
	"time"

	"stackChan/internal/model"
)

func resetAppClientPoolForTest() {
	appClientMu.Lock()
	defer appClientMu.Unlock()
	appClientPool = sync.Map{}
}

func resetStackChanClientPoolForTest() {
	stackChanClientMu.Lock()
	defer stackChanClientMu.Unlock()
	stackChanClientPool = sync.Map{}
}

func TestAppClientPoolReturnsCopies(t *testing.T) {
	resetAppClientPoolForTest()

	first := model.NewAppClient("001122334455", nil, "first")
	second := model.NewAppClient("001122334455", nil, "second")
	t.Cleanup(func() {
		resetAppClientPoolForTest()
		first.CloseWriterCoroutine()
		second.CloseWriterCoroutine()
	})

	addAppClient(first)
	addAppClient(second)

	clients := getAppClients(first.GetMac())
	if len(clients) != 2 {
		t.Fatalf("client count = %d, want 2", len(clients))
	}
	clients[0] = nil
	if got := getAppClients(first.GetMac())[0]; got != first {
		t.Fatal("getAppClients exposed the stored slice")
	}

	entries := getAppClientPoolSnapshot()
	if len(entries) != 1 || !entries[0].valid {
		t.Fatalf("pool snapshot = %#v, want one valid entry", entries)
	}
	entries[0].clients[0] = nil
	if got := getAppClients(first.GetMac())[0]; got != first {
		t.Fatal("getAppClientPoolSnapshot exposed the stored slice")
	}
}

func TestConcurrentAppClientAddAndCleanupDoesNotLoseLiveClients(t *testing.T) {
	resetAppClientPoolForTest()

	const liveClientCount = 128
	const mac = "AABBCCDDEEFF"
	now := time.Now()
	expired := model.NewAppClient(mac, nil, "expired")
	expired.SetLastTime(now.Add(-2 * ClientExpireTimeout))
	addAppClient(expired)

	liveClients := make([]*model.AppClient, liveClientCount)
	for i := range liveClients {
		liveClients[i] = model.NewAppClient(mac, nil, "live")
		liveClients[i].SetLastTime(now)
	}
	t.Cleanup(func() {
		resetAppClientPoolForTest()
		expired.CloseWriterCoroutine()
		for _, client := range liveClients {
			client.CloseWriterCoroutine()
		}
	})

	start := make(chan struct{})
	var wg sync.WaitGroup
	wg.Add(liveClientCount + 1)
	for _, client := range liveClients {
		go func() {
			defer wg.Done()
			<-start
			addAppClient(client)
		}()
	}

	var expiredClients []expiredAppClient
	go func() {
		defer wg.Done()
		<-start
		expiredClients = removeExpiredAppClients(context.Background(), now)
	}()

	close(start)
	wg.Wait()

	if len(expiredClients) != 1 || expiredClients[0].client != expired {
		t.Fatalf("expired clients = %#v, want only the expired client", expiredClients)
	}

	clients := getAppClients(mac)
	if len(clients) != liveClientCount {
		t.Fatalf("live client count = %d, want %d", len(clients), liveClientCount)
	}
	for _, client := range clients {
		if client == expired {
			t.Fatal("expired client remained in the pool")
		}
	}
}

func TestConcurrentAppRegistrationReusesOneClient(t *testing.T) {
	resetAppClientPoolForTest()

	const (
		registrationCount = 64
		mac               = "AABBCCDDEEFF"
		deviceID          = "phone"
	)
	clients := make(chan *model.AppClient, registrationCount)
	var wg sync.WaitGroup
	wg.Add(registrationCount)
	for range registrationCount {
		go func() {
			defer wg.Done()
			client, _, _ := registerAppClient(mac, deviceID, nil)
			clients <- client
		}()
	}
	wg.Wait()
	close(clients)

	var first *model.AppClient
	for client := range clients {
		if first == nil {
			first = client
			t.Cleanup(func() {
				first.CloseWriterCoroutine()
				resetAppClientPoolForTest()
			})
			continue
		}
		if client != first {
			t.Fatal("concurrent registration created duplicate AppClient instances")
		}
	}

	if got := getAppClients(mac); len(got) != 1 || got[0] != first {
		t.Fatalf("registered clients = %#v, want one shared client", got)
	}
}

func TestConcurrentStackChanRegistrationReusesOneClient(t *testing.T) {
	resetStackChanClientPoolForTest()

	const (
		registrationCount = 64
		mac               = "AABBCCDDEEFF"
	)
	clients := make(chan *model.StackChanClient, registrationCount)
	var wg sync.WaitGroup
	wg.Add(registrationCount)
	for range registrationCount {
		go func() {
			defer wg.Done()
			client, _, _ := registerStackChanClient(mac, nil)
			clients <- client
		}()
	}
	wg.Wait()
	close(clients)

	var first *model.StackChanClient
	for client := range clients {
		if first == nil {
			first = client
			t.Cleanup(func() {
				first.CloseWriterCoroutine()
				resetStackChanClientPoolForTest()
			})
			continue
		}
		if client != first {
			t.Fatal("concurrent registration created duplicate StackChanClient instances")
		}
	}

	value, ok := stackChanClientPool.Load(mac)
	if !ok || value != first {
		t.Fatalf("registered StackChan client = %#v, want shared client %#v", value, first)
	}
}

func TestRegistrationReplacesStoppedWriters(t *testing.T) {
	resetAppClientPoolForTest()
	resetStackChanClientPoolForTest()
	t.Cleanup(func() {
		resetAppClientPoolForTest()
		resetStackChanClientPoolForTest()
	})

	const (
		mac      = "AABBCCDDEEFF"
		deviceID = "phone"
	)

	oldApp, _, _ := registerAppClient(mac, deviceID, nil)
	oldApp.CloseWriterCoroutine()
	newApp, _, created := registerAppClient(mac, deviceID, nil)
	if !created || newApp == oldApp || newApp.IsStopped() {
		t.Fatal("stopped AppClient writer was reused")
	}
	newApp.CloseWriterCoroutine()

	oldStackChan, _, _ := registerStackChanClient(mac, nil)
	oldStackChan.CloseWriterCoroutine()
	newStackChan, _, created := registerStackChanClient(mac, nil)
	if !created || newStackChan == oldStackChan || newStackChan.IsStopped() {
		t.Fatal("stopped StackChanClient writer was reused")
	}
	newStackChan.CloseWriterCoroutine()
}
