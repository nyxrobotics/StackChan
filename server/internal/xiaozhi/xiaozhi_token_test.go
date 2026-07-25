/*
SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
SPDX-License-Identifier: MIT
*/

package xiaozhi

import (
	"context"
	"errors"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/gogf/gf/v2/net/gclient"
)

func resetTokenStateForTest() {
	tokenMu.Lock()
	defer tokenMu.Unlock()

	token = ""
	tokenExpire = time.Time{}
	tokenVersion = 0
	tokenRefreshDone = nil
	tokenRefreshErr = nil
	tokenRefreshFailedAt = time.Time{}
}

func TestConcurrentGetTokenUsesOneRefresh(t *testing.T) {
	resetTokenStateForTest()
	originalFetcher := tokenFetcher

	var calls atomic.Int32
	started := make(chan struct{})
	release := make(chan struct{})
	var startedOnce sync.Once
	tokenFetcher = func(requestCtx context.Context) (string, error) {
		calls.Add(1)
		startedOnce.Do(func() { close(started) })
		select {
		case <-requestCtx.Done():
			return "", requestCtx.Err()
		case <-release:
			return "shared-token", nil
		}
	}
	t.Cleanup(func() {
		tokenFetcher = originalFetcher
		resetTokenStateForTest()
	})

	const callers = 32
	results := make(chan string, callers)
	errs := make(chan error, callers)
	start := make(chan struct{})
	var wg sync.WaitGroup
	wg.Add(callers)
	for range callers {
		go func() {
			defer wg.Done()
			<-start
			got, err := GetTokenContext(context.Background())
			results <- got
			errs <- err
		}()
	}
	close(start)
	<-started
	close(release)
	wg.Wait()
	close(results)
	close(errs)

	if got := calls.Load(); got != 1 {
		t.Fatalf("token refresh calls = %d, want 1", got)
	}
	for err := range errs {
		if err != nil {
			t.Fatalf("GetTokenContext() error = %v", err)
		}
	}
	for got := range results {
		if got != "shared-token" {
			t.Fatalf("GetTokenContext() = %q, want shared-token", got)
		}
	}
}

func TestGetTokenAppliesFailureCooldown(t *testing.T) {
	resetTokenStateForTest()
	originalFetcher := tokenFetcher

	refreshErr := errors.New("upstream unavailable")
	var calls atomic.Int32
	tokenFetcher = func(context.Context) (string, error) {
		calls.Add(1)
		return "", refreshErr
	}
	t.Cleanup(func() {
		tokenFetcher = originalFetcher
		resetTokenStateForTest()
	})

	for range 2 {
		if _, err := GetTokenContext(context.Background()); !errors.Is(err, refreshErr) {
			t.Fatalf("GetTokenContext() error = %v, want %v", err, refreshErr)
		}
	}
	if got := calls.Load(); got != 1 {
		t.Fatalf("token refresh calls = %d, want 1 during cooldown", got)
	}
}

func TestTokenRefreshPanicReleasesWaitersAndEntersCooldown(t *testing.T) {
	resetTokenStateForTest()
	originalFetcher := tokenFetcher

	var calls atomic.Int32
	tokenFetcher = func(context.Context) (string, error) {
		calls.Add(1)
		panic("refresh panic")
	}
	t.Cleanup(func() {
		tokenFetcher = originalFetcher
		resetTokenStateForTest()
	})

	const callers = 8
	start := make(chan struct{})
	errs := make(chan error, callers)
	var wg sync.WaitGroup
	wg.Add(callers)
	for range callers {
		go func() {
			defer wg.Done()
			<-start
			_, err := GetTokenContext(context.Background())
			errs <- err
		}()
	}
	close(start)
	wg.Wait()
	close(errs)

	for err := range errs {
		if err == nil || !strings.Contains(err.Error(), "refresh token panic") {
			t.Fatalf("GetTokenContext() error = %v, want recovered panic", err)
		}
	}
	if got := calls.Load(); got != 1 {
		t.Fatalf("token refresh calls = %d, want one recovered refresh", got)
	}
}

func TestCanceledRefreshLeaderAllowsAnotherCallerToRetry(t *testing.T) {
	resetTokenStateForTest()
	originalFetcher := tokenFetcher

	var calls atomic.Int32
	firstStarted := make(chan struct{})
	tokenFetcher = func(requestCtx context.Context) (string, error) {
		if calls.Add(1) == 1 {
			close(firstStarted)
			<-requestCtx.Done()
			return "", requestCtx.Err()
		}
		return "retry-token", nil
	}
	t.Cleanup(func() {
		tokenFetcher = originalFetcher
		resetTokenStateForTest()
	})

	leaderCtx, cancelLeader := context.WithCancel(context.Background())
	leaderResult := make(chan error, 1)
	go func() {
		_, err := GetTokenContext(leaderCtx)
		leaderResult <- err
	}()
	<-firstStarted

	waiterResult := make(chan struct {
		token string
		err   error
	}, 1)
	go func() {
		got, err := GetTokenContext(context.Background())
		waiterResult <- struct {
			token string
			err   error
		}{token: got, err: err}
	}()

	cancelLeader()
	if err := <-leaderResult; !errors.Is(err, context.Canceled) {
		t.Fatalf("refresh leader error = %v, want context cancellation", err)
	}

	select {
	case result := <-waiterResult:
		if result.err != nil || result.token != "retry-token" {
			t.Fatalf("waiting caller result = (%q, %v), want retry-token", result.token, result.err)
		}
	case <-time.After(time.Second):
		t.Fatal("waiting caller was not released after refresh leader cancellation")
	}
	if got := calls.Load(); got != 2 {
		t.Fatalf("token refresh calls = %d, want canceled leader plus one retry", got)
	}
}

func TestInvalidateTokenDoesNotDiscardNewerRefresh(t *testing.T) {
	resetTokenStateForTest()
	t.Cleanup(resetTokenStateForTest)

	tokenMu.Lock()
	token = "new-token"
	tokenExpire = time.Now().Add(time.Hour)
	tokenVersion = 2
	tokenMu.Unlock()

	invalidateTokenIfCurrent("old-token", 1)
	if got, _, err := getToken(context.Background(), false); err != nil || got != "new-token" {
		t.Fatalf("newer token was invalidated: token=%q, error=%v", got, err)
	}
}

func TestRequestVarContextRejectsOversizedResponse(t *testing.T) {
	server := httptest.NewServer(http.HandlerFunc(func(writer http.ResponseWriter, _ *http.Request) {
		_, _ = writer.Write([]byte(strings.Repeat("x", responseBodyLimit+1)))
	}))
	t.Cleanup(server.Close)

	_, err := requestVarContext(
		context.Background(),
		gclient.New().SetTimeout(time.Second),
		http.MethodGet,
		server.URL,
		nil,
	)
	if err == nil || !strings.Contains(err.Error(), "exceeds") {
		t.Fatalf("requestVarContext() error = %v, want response size error", err)
	}
}
