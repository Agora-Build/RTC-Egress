package logger

import (
	"bytes"
	"errors"
	"os"
	"strings"
	"testing"
	"time"
)

func captureOutputExt(t *testing.T, fn func()) string {
	t.Helper()
	var buf bytes.Buffer
	SetOutput(&buf)
	defer SetOutput(os.Stdout)
	fn()
	return buf.String()
}

func TestWarnLevel(t *testing.T) {
	out := captureOutputExt(t, func() {
		Init("test-service")
		Warn("disk usage high", String("usage", "85%"))
	})
	if !strings.Contains(out, "][WARN]disk usage high") {
		t.Fatalf("expected WARN level, got %q", out)
	}
	if !strings.Contains(out, "usage=85%") {
		t.Fatalf("expected usage field, got %q", out)
	}
}

func TestErrorLevel(t *testing.T) {
	out := captureOutputExt(t, func() {
		Init("test-service")
		Error("connection failed", String("host", "redis:6379"))
	})
	if !strings.Contains(out, "][ERROR]connection failed") {
		t.Fatalf("expected ERROR level, got %q", out)
	}
}

func TestDebugLevel(t *testing.T) {
	out := captureOutputExt(t, func() {
		Init("test-service")
		Debug("processing frame", Int("frame_id", 42))
	})
	if !strings.Contains(out, "][DEBUG]processing frame") {
		t.Fatalf("expected DEBUG level, got %q", out)
	}
	if !strings.Contains(out, "frame_id=42") {
		t.Fatalf("expected frame_id field, got %q", out)
	}
}

func TestIntField(t *testing.T) {
	out := captureOutputExt(t, func() {
		Init("test")
		Info("workers ready", Int("count", 5))
	})
	if !strings.Contains(out, "count=5") {
		t.Fatalf("expected int field count=5, got %q", out)
	}
}

func TestBoolField(t *testing.T) {
	out := captureOutputExt(t, func() {
		Init("test")
		Info("feature flag", Bool("enabled", true))
	})
	if !strings.Contains(out, "enabled=true") {
		t.Fatalf("expected bool field enabled=true, got %q", out)
	}
}

func TestDurationField(t *testing.T) {
	out := captureOutputExt(t, func() {
		Init("test")
		Info("request latency", Duration("elapsed", 150*time.Millisecond))
	})
	if !strings.Contains(out, "elapsed=150ms") {
		t.Fatalf("expected duration field, got %q", out)
	}
}

func TestErrorFieldWithNil(t *testing.T) {
	f := ErrorField(nil)
	if f.key != "" {
		t.Fatalf("expected empty field for nil error, got key=%q", f.key)
	}
}

func TestErrorFieldWithError(t *testing.T) {
	out := captureOutputExt(t, func() {
		Init("test")
		Info("operation failed", ErrorField(errors.New("something broke")))
	})
	if !strings.Contains(out, `error="something broke"`) {
		t.Fatalf("expected error field, got %q", out)
	}
}

func TestWithDeduplicatesKeys(t *testing.T) {
	out := captureOutputExt(t, func() {
		Init("test")
		With(String("env", "staging"))
		With(String("env", "production")) // should override
		Info("config loaded")
	})
	// Should only contain one env= and it should be production
	if strings.Count(out, "env=") != 1 {
		t.Fatalf("expected exactly one env field, got %q", out)
	}
	if !strings.Contains(out, "env=production") {
		t.Fatalf("expected env=production, got %q", out)
	}
}

func TestWithEmptyKeyIgnored(t *testing.T) {
	out := captureOutputExt(t, func() {
		Init("test")
		With(String("", "value"))
		Info("test message")
	})
	// Empty key should not appear
	if strings.Contains(out, "=value") {
		t.Fatalf("empty key should be ignored, got %q", out)
	}
}

func TestFieldOverrideInLogCall(t *testing.T) {
	out := captureOutputExt(t, func() {
		Init("test", String("version", "v1.0"))
		Info("request", String("version", "v2.0")) // per-call override
	})
	// Per-call field should override default
	if strings.Count(out, "version=") != 1 {
		t.Fatalf("expected one version field, got %q", out)
	}
	if !strings.Contains(out, "version=v2.0") {
		t.Fatalf("expected per-call version to override default, got %q", out)
	}
}

func TestMultipleFields(t *testing.T) {
	out := captureOutputExt(t, func() {
		Init("test")
		Info("task started",
			String("task_id", "abc123"),
			String("channel", "demo"),
			Int("worker", 3),
			Bool("web", false),
		)
	})
	if !strings.Contains(out, "task_id=abc123") {
		t.Fatalf("expected task_id, got %q", out)
	}
	if !strings.Contains(out, "channel=demo") {
		t.Fatalf("expected channel, got %q", out)
	}
	if !strings.Contains(out, "worker=3") {
		t.Fatalf("expected worker, got %q", out)
	}
	if !strings.Contains(out, "web=false") {
		t.Fatalf("expected web, got %q", out)
	}
}

func TestNoFields(t *testing.T) {
	out := captureOutputExt(t, func() {
		Init("test")
		Info("simple message")
	})
	if !strings.Contains(out, "][INFO]simple message") {
		t.Fatalf("expected simple message without fields, got %q", out)
	}
	// Should not have parentheses for empty fields
	if strings.Contains(out, "()") {
		t.Fatalf("expected no empty parens, got %q", out)
	}
}
