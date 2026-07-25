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

	var expiredClients []*model.AppClient
	go func() {
		defer wg.Done()
		<-start
		expiredClients = removeExpiredAppClients(context.Background(), now)
	}()

	close(start)
	wg.Wait()

	if len(expiredClients) != 1 || expiredClients[0] != expired {
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
