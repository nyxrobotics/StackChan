/*
SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
SPDX-License-Identifier: MIT
*/

package boot

import (
	"context"
	"stackChan/internal/web_socket"
	"sync"
	"time"

	"github.com/gogf/gf/v2/frame/g"
)

var (
	cronMu     sync.Mutex
	cronCancel context.CancelFunc
	cronDone   chan struct{}
)

// InitCron starts the background WebSocket maintenance tasks once.
func InitCron() {
	InitCronContext(context.Background())
}

// InitCronContext ties the maintenance tasks to the server lifecycle. StopCron
// should be called when the server stops.
func InitCronContext(parent context.Context) {
	if parent == nil {
		parent = context.Background()
	}

	cronMu.Lock()
	if cronCancel != nil {
		cronMu.Unlock()
		return
	}
	ctx, cancel := context.WithCancel(parent)
	done := make(chan struct{})
	cronCancel = cancel
	cronDone = done
	cronMu.Unlock()

	var wg sync.WaitGroup
	wg.Add(2)

	go runPeriodicTask(
		ctx,
		&wg,
		5*time.Second,
		"The heartbeat sending timer has been activated",
		"Heartbeat sending task crash",
		web_socket.StartPingTime,
	)
	go runPeriodicTask(
		ctx,
		&wg,
		15*time.Second,
		"The connection cleaning timer has been started",
		"Connection cleanup task crash",
		web_socket.CheckExpiredLinks,
	)

	go func() {
		wg.Wait()

		cronMu.Lock()
		if cronDone == done {
			cronCancel = nil
			cronDone = nil
		}
		cronMu.Unlock()

		// Closing done after clearing the matching generation guarantees that
		// StopCron does not return while InitCronContext can still observe the
		// stopped generation as active.
		close(done)
	}()
}

// StopCron cancels and waits for the maintenance tasks. It is safe to call
// repeatedly.
func StopCron() {
	cronMu.Lock()
	cancel := cronCancel
	done := cronDone
	cronMu.Unlock()

	if cancel == nil {
		return
	}
	cancel()
	if done != nil {
		<-done
	}
}

func runPeriodicTask(
	ctx context.Context,
	wg *sync.WaitGroup,
	interval time.Duration,
	startMessage string,
	panicMessage string,
	task func(context.Context),
) {
	defer wg.Done()

	ticker := time.NewTicker(interval)
	defer ticker.Stop()
	g.Log().Info(ctx, startMessage)

	for {
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
			func() {
				defer func() {
					if recovered := recover(); recovered != nil {
						g.Log().Errorf(ctx, "%s: %v", panicMessage, recovered)
					}
				}()
				task(ctx)
			}()
		}
	}
}
