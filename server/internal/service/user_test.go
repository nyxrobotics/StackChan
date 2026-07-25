/*
SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
SPDX-License-Identifier: MIT
*/

package service

import (
	"context"
	"errors"
	"io"
	"net/http"
	"net/http/httptest"
	"net/url"
	"strings"
	"testing"
	"time"
)

func TestPostRemoteUserForm(t *testing.T) {
	server := httptest.NewServer(http.HandlerFunc(func(writer http.ResponseWriter, request *http.Request) {
		if request.Method != http.MethodPost {
			t.Errorf("method = %q, want %q", request.Method, http.MethodPost)
		}
		if got := request.Header.Get("Content-Type"); got != "application/x-www-form-urlencoded" {
			t.Errorf("Content-Type = %q", got)
		}
		if got := request.Header.Get("Authorization"); got != "registration-token" {
			t.Errorf("Authorization = %q", got)
		}
		if err := request.ParseForm(); err != nil {
			t.Errorf("ParseForm() error = %v", err)
		}
		if got := request.Form.Get("username"); got != "stackchan" {
			t.Errorf("username = %q", got)
		}
		writer.Header().Set("Content-Type", "application/json")
		_, _ = io.WriteString(writer, `{"status":{"code":"ok"}}`)
	}))
	t.Cleanup(server.Close)

	body, err := postRemoteUserForm(
		context.Background(),
		newRemoteUserHTTPClient(time.Second),
		server.URL,
		http.Header{"Authorization": {"registration-token"}},
		url.Values{"username": {"stackchan"}},
	)
	if err != nil {
		t.Fatalf("postRemoteUserForm() error = %v", err)
	}
	if got, want := string(body), `{"status":{"code":"ok"}}`; got != want {
		t.Fatalf("body = %q, want %q", got, want)
	}
}

func TestPostRemoteUserFormRejectsUnexpectedStatus(t *testing.T) {
	server := httptest.NewServer(http.HandlerFunc(func(writer http.ResponseWriter, _ *http.Request) {
		http.Error(writer, "unavailable", http.StatusServiceUnavailable)
	}))
	t.Cleanup(server.Close)

	_, err := postRemoteUserForm(
		context.Background(),
		newRemoteUserHTTPClient(time.Second),
		server.URL,
		nil,
		url.Values{},
	)
	var statusErr *remoteUserHTTPStatusError
	if !errors.As(err, &statusErr) {
		t.Fatalf("error = %v, want *remoteUserHTTPStatusError", err)
	}
	if statusErr.statusCode != http.StatusServiceUnavailable {
		t.Fatalf("status code = %d, want %d", statusErr.statusCode, http.StatusServiceUnavailable)
	}
}

func TestPostRemoteUserFormRejectsOversizedResponse(t *testing.T) {
	server := httptest.NewServer(http.HandlerFunc(func(writer http.ResponseWriter, _ *http.Request) {
		_, _ = io.WriteString(writer, strings.Repeat("x", remoteUserResponseBodyLimit+1))
	}))
	t.Cleanup(server.Close)

	_, err := postRemoteUserForm(
		context.Background(),
		newRemoteUserHTTPClient(time.Second),
		server.URL,
		nil,
		url.Values{},
	)
	if !errors.Is(err, errRemoteUserResponseTooLarge) {
		t.Fatalf("error = %v, want %v", err, errRemoteUserResponseTooLarge)
	}
}

func TestPostRemoteUserFormTimesOut(t *testing.T) {
	server := httptest.NewServer(http.HandlerFunc(func(_ http.ResponseWriter, request *http.Request) {
		<-request.Context().Done()
	}))
	t.Cleanup(server.Close)

	_, err := postRemoteUserForm(
		context.Background(),
		newRemoteUserHTTPClient(20*time.Millisecond),
		server.URL,
		nil,
		url.Values{},
	)
	if !errors.Is(err, context.DeadlineExceeded) {
		t.Fatalf("error = %v, want context deadline exceeded", err)
	}
}
