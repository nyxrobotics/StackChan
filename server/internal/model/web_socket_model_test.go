/*
SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
SPDX-License-Identifier: MIT
*/

package model

import (
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
