package webhook

import (
	"testing"
	"time"

	"github.com/AgoraIO/RTC-Egress/pkg/queue"
)

func TestIsValidTaskKey(t *testing.T) {
	tests := []struct {
		key  string
		want bool
	}{
		{"egress:task:abc123", true},
		{"egress:task:a", true},
		{"egress:task:", false},
		{"egress:tas:abc", false},
		{"other:key", false},
		{"", false},
	}

	for _, tt := range tests {
		if got := isValidTaskKey(tt.key); got != tt.want {
			t.Errorf("isValidTaskKey(%q) = %v, want %v", tt.key, got, tt.want)
		}
	}
}

func TestExtractTaskIDFromKey(t *testing.T) {
	tests := []struct {
		key  string
		want string
	}{
		{"egress:task:abc123", "abc123"},
		{"egress:task:a", "a"},
		{"egress:task:complex-id-with-dashes", "complex-id-with-dashes"},
	}

	for _, tt := range tests {
		if got := extractTaskIDFromKey(tt.key); got != tt.want {
			t.Errorf("extractTaskIDFromKey(%q) = %q, want %q", tt.key, got, tt.want)
		}
	}
}

func TestIsTerminalState(t *testing.T) {
	tests := []struct {
		state string
		want  bool
	}{
		{queue.TaskStateStopped, true},
		{queue.TaskStateFailed, true},
		{queue.TaskStateTimeout, true},
		{queue.TaskStateProcessing, false},
		{queue.TaskStateEnqueued, false},
		{queue.TaskStateStopping, false},
		{"", false},
	}

	for _, tt := range tests {
		if got := isTerminalState(tt.state); got != tt.want {
			t.Errorf("isTerminalState(%q) = %v, want %v", tt.state, got, tt.want)
		}
	}
}

func TestGetNotificationHash(t *testing.T) {
	ts := time.Date(2025, 1, 1, 0, 0, 0, 0, time.UTC)
	p1 := WebhookPayload{TaskID: "task1", State: "PROCESSING", Timestamp: ts}
	p2 := WebhookPayload{TaskID: "task1", State: "PROCESSING", Timestamp: ts}
	p3 := WebhookPayload{TaskID: "task2", State: "PROCESSING", Timestamp: ts}

	h1 := GetNotificationHash(p1)
	h2 := GetNotificationHash(p2)
	h3 := GetNotificationHash(p3)

	if h1 != h2 {
		t.Fatalf("same payload should produce same hash: %s != %s", h1, h2)
	}
	if h1 == h3 {
		t.Fatalf("different payloads should produce different hashes")
	}
	if len(h1) != 16 { // 8 bytes = 16 hex chars
		t.Fatalf("expected 16 char hex hash, got %d chars: %s", len(h1), h1)
	}
}

func TestDefaultWebhookConfig(t *testing.T) {
	cfg := DefaultWebhookConfig()

	if cfg.MaxRetries != 5 {
		t.Fatalf("expected MaxRetries 5, got %d", cfg.MaxRetries)
	}
	if cfg.Timeout != 30*time.Second {
		t.Fatalf("expected Timeout 30s, got %v", cfg.Timeout)
	}
	if cfg.MaxRetryInterval != 5*time.Minute {
		t.Fatalf("expected MaxRetryInterval 5m, got %v", cfg.MaxRetryInterval)
	}
	if cfg.BatchSize != 0 {
		t.Fatalf("expected BatchSize 0 (disabled), got %d", cfg.BatchSize)
	}
	if len(cfg.NotifyStates) != 4 {
		t.Fatalf("expected 4 notify states, got %d", len(cfg.NotifyStates))
	}
	if cfg.DeliveredTTL != 48*time.Hour {
		t.Fatalf("expected DeliveredTTL 48h, got %v", cfg.DeliveredTTL)
	}
}

func TestShouldNotifyState(t *testing.T) {
	wn := &WebhookNotifier{
		config: WebhookConfig{
			NotifyStates: []string{queue.TaskStateProcessing, queue.TaskStateStopped, queue.TaskStateFailed},
		},
	}

	if !wn.shouldNotifyState(queue.TaskStateProcessing) {
		t.Fatal("should notify PROCESSING state")
	}
	if !wn.shouldNotifyState(queue.TaskStateStopped) {
		t.Fatal("should notify STOPPED state")
	}
	if wn.shouldNotifyState(queue.TaskStateEnqueued) {
		t.Fatal("should NOT notify ENQUEUED state")
	}
	if wn.shouldNotifyState(queue.TaskStateStopping) {
		t.Fatal("should NOT notify STOPPING state")
	}
}

func TestCalculateBackoff(t *testing.T) {
	wn := &WebhookNotifier{
		config: WebhookConfig{
			BaseRetryInterval: 15 * time.Second,
			MaxRetryInterval:  5 * time.Minute,
		},
	}

	// retry 1: 15s * 2^0 = 15s
	b1 := wn.calculateBackoff(1)
	if b1 != 15*time.Second {
		t.Fatalf("expected 15s for retry 1, got %v", b1)
	}

	// retry 2: 15s * 2^1 = 30s
	b2 := wn.calculateBackoff(2)
	if b2 != 30*time.Second {
		t.Fatalf("expected 30s for retry 2, got %v", b2)
	}

	// retry 3: 15s * 2^2 = 60s
	b3 := wn.calculateBackoff(3)
	if b3 != 60*time.Second {
		t.Fatalf("expected 60s for retry 3, got %v", b3)
	}

	// Large retry count should cap at max
	b10 := wn.calculateBackoff(10)
	if b10 != 5*time.Minute {
		t.Fatalf("expected max 5m for retry 10, got %v", b10)
	}
}

func TestNewWebhookNotifier_AppliesDefaults(t *testing.T) {
	wn := NewWebhookNotifier(WebhookConfig{URL: "http://example.com"}, nil)

	if wn.config.MaxRetries != 5 {
		t.Fatalf("expected default MaxRetries 5, got %d", wn.config.MaxRetries)
	}
	if wn.config.Timeout != 30*time.Second {
		t.Fatalf("expected default Timeout 30s, got %v", wn.config.Timeout)
	}
	if wn.config.MaxRetryInterval != 5*time.Minute {
		t.Fatalf("expected default MaxRetryInterval 5m, got %v", wn.config.MaxRetryInterval)
	}
	if len(wn.config.NotifyStates) != 4 {
		t.Fatalf("expected 4 default notify states, got %d", len(wn.config.NotifyStates))
	}
	if wn.config.DeliveredTTL != 48*time.Hour {
		t.Fatalf("expected default DeliveredTTL 48h, got %v", wn.config.DeliveredTTL)
	}
}

func TestNewWebhookNotifier_PreservesExplicitConfig(t *testing.T) {
	cfg := WebhookConfig{
		URL:               "http://example.com",
		MaxRetries:        10,
		Timeout:           60 * time.Second,
		MaxRetryInterval:  10 * time.Minute,
		BaseRetryInterval: 30 * time.Second,
		NotifyStates:      []string{queue.TaskStateFailed},
		DeliveredTTL:      24 * time.Hour,
	}

	wn := NewWebhookNotifier(cfg, nil)

	if wn.config.MaxRetries != 10 {
		t.Fatalf("expected MaxRetries 10, got %d", wn.config.MaxRetries)
	}
	if wn.config.Timeout != 60*time.Second {
		t.Fatalf("expected Timeout 60s, got %v", wn.config.Timeout)
	}
	if len(wn.config.NotifyStates) != 1 {
		t.Fatalf("expected 1 notify state, got %d", len(wn.config.NotifyStates))
	}
	if wn.config.DeliveredTTL != 24*time.Hour {
		t.Fatalf("expected DeliveredTTL 24h, got %v", wn.config.DeliveredTTL)
	}
}

func TestDeliveredKey(t *testing.T) {
	wn := &WebhookNotifier{}
	key := wn.deliveredKey("task123", "STOPPED")
	expected := "egress:notif:delivered:task123:STOPPED"
	if key != expected {
		t.Fatalf("expected %q, got %q", expected, key)
	}
}
