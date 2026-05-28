package webhook

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"

	"github.com/AgoraIO/RTC-Egress/pkg/queue"
)

func TestMakeWebhookRequest_SinglePayload(t *testing.T) {
	var receivedBody map[string]interface{}
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Header.Get("Content-Type") != "application/json" {
			t.Error("expected application/json content type")
		}
		json.NewDecoder(r.Body).Decode(&receivedBody)
		w.WriteHeader(http.StatusOK)
	}))
	defer server.Close()

	wn := NewWebhookNotifier(WebhookConfig{
		URL:     server.URL,
		Timeout: 5 * time.Second,
	}, nil)

	payload := WebhookPayload{
		TaskID:    "task-1",
		State:     queue.TaskStateProcessing,
		Channel:   "demo",
		Timestamp: time.Now(),
	}

	success := wn.makeWebhookRequest([]WebhookPayload{payload})
	if !success {
		t.Fatal("expected successful webhook request")
	}
	if receivedBody["task_id"] != "task-1" {
		t.Fatalf("expected task_id 'task-1', got %v", receivedBody["task_id"])
	}
}

func TestMakeWebhookRequest_BatchPayload(t *testing.T) {
	var receivedBody map[string]interface{}
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		json.NewDecoder(r.Body).Decode(&receivedBody)
		w.WriteHeader(http.StatusOK)
	}))
	defer server.Close()

	wn := NewWebhookNotifier(WebhookConfig{
		URL:     server.URL,
		Timeout: 5 * time.Second,
	}, nil)

	payloads := []WebhookPayload{
		{TaskID: "task-1", State: "PROCESSING", Timestamp: time.Now()},
		{TaskID: "task-2", State: "STOPPED", Timestamp: time.Now()},
	}

	success := wn.makeWebhookRequest(payloads)
	if !success {
		t.Fatal("expected successful batch webhook request")
	}
	if receivedBody["batch_size"] != float64(2) {
		t.Fatalf("expected batch_size 2, got %v", receivedBody["batch_size"])
	}
}

func TestMakeWebhookRequest_ServerError(t *testing.T) {
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusInternalServerError)
	}))
	defer server.Close()

	wn := NewWebhookNotifier(WebhookConfig{
		URL:     server.URL,
		Timeout: 5 * time.Second,
	}, nil)

	success := wn.makeWebhookRequest([]WebhookPayload{{TaskID: "task-1"}})
	if success {
		t.Fatal("expected failure for 500 response")
	}
}

func TestMakeWebhookRequest_WithAuthToken(t *testing.T) {
	var receivedAuth string
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		receivedAuth = r.Header.Get("Authorization")
		w.WriteHeader(http.StatusOK)
	}))
	defer server.Close()

	wn := NewWebhookNotifier(WebhookConfig{
		URL:       server.URL,
		Timeout:   5 * time.Second,
		AuthToken: "my-secret-token",
	}, nil)

	wn.makeWebhookRequest([]WebhookPayload{{TaskID: "task-1"}})
	if receivedAuth != "Bearer my-secret-token" {
		t.Fatalf("expected Bearer auth header, got %q", receivedAuth)
	}
}

func TestMakeWebhookRequest_NoAuthToken(t *testing.T) {
	var receivedAuth string
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		receivedAuth = r.Header.Get("Authorization")
		w.WriteHeader(http.StatusOK)
	}))
	defer server.Close()

	wn := NewWebhookNotifier(WebhookConfig{
		URL:     server.URL,
		Timeout: 5 * time.Second,
	}, nil)

	wn.makeWebhookRequest([]WebhookPayload{{TaskID: "task-1"}})
	if receivedAuth != "" {
		t.Fatalf("expected no auth header, got %q", receivedAuth)
	}
}

func TestSendNotification_Success(t *testing.T) {
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusOK)
	}))
	defer server.Close()

	wn := NewWebhookNotifier(WebhookConfig{
		URL:     server.URL,
		Timeout: 5 * time.Second,
	}, nil)

	notifState := &NotificationState{TaskID: "task-1"}
	payload := WebhookPayload{
		TaskID:    "task-1",
		State:     queue.TaskStateProcessing,
		Timestamp: time.Now(),
	}

	wn.sendNotification(payload, notifState)

	if notifState.LastState != queue.TaskStateProcessing {
		t.Fatalf("expected LastState PROCESSING, got %q", notifState.LastState)
	}
	if notifState.RetryCount != 0 {
		t.Fatalf("expected RetryCount 0 after success, got %d", notifState.RetryCount)
	}
}

func TestSendNotification_FailureSetsRetry(t *testing.T) {
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusServiceUnavailable)
	}))
	defer server.Close()

	wn := NewWebhookNotifier(WebhookConfig{
		URL:               server.URL,
		Timeout:           5 * time.Second,
		MaxRetries:        3,
		BaseRetryInterval: 1 * time.Second,
		MaxRetryInterval:  10 * time.Second,
	}, nil)

	notifState := &NotificationState{TaskID: "task-1"}
	payload := WebhookPayload{
		TaskID:    "task-1",
		State:     queue.TaskStateFailed,
		Timestamp: time.Now(),
	}

	wn.sendNotification(payload, notifState)

	if notifState.RetryCount != 1 {
		t.Fatalf("expected RetryCount 1 after failure, got %d", notifState.RetryCount)
	}
	if notifState.NextRetryTime.IsZero() {
		t.Fatal("expected NextRetryTime to be set after failure")
	}
}

func TestAddToBatch(t *testing.T) {
	wn := NewWebhookNotifier(WebhookConfig{
		URL:       "http://unused",
		BatchSize: 3,
	}, nil)

	wn.addToBatch(WebhookPayload{TaskID: "t1"})
	wn.addToBatch(WebhookPayload{TaskID: "t2"})

	wn.batchMutex.Lock()
	count := len(wn.batchQueue)
	wn.batchMutex.Unlock()

	if count != 2 {
		t.Fatalf("expected 2 items in batch, got %d", count)
	}
}
