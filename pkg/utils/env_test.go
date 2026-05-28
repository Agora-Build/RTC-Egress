package utils

import "testing"

func TestResolveRedisAddr_ExplicitAddr(t *testing.T) {
	got := ResolveRedisAddr("localhost:6379")
	if got != "localhost:6379" {
		t.Fatalf("expected 'localhost:6379', got %q", got)
	}
}

func TestResolveRedisAddr_TrimsWhitespace(t *testing.T) {
	got := ResolveRedisAddr("  redis:6379  ")
	if got != "redis:6379" {
		t.Fatalf("expected 'redis:6379', got %q", got)
	}
}

func TestResolveRedisAddr_FallsBackToREDIS_ADDR(t *testing.T) {
	t.Setenv("REDIS_ADDR", "env-redis:6379")
	t.Setenv("REDIS_HOST", "")
	t.Setenv("REDIS_PORT", "")

	got := ResolveRedisAddr("")
	if got != "env-redis:6379" {
		t.Fatalf("expected 'env-redis:6379', got %q", got)
	}
}

func TestResolveRedisAddr_FallsBackToHostPort(t *testing.T) {
	t.Setenv("REDIS_ADDR", "")
	t.Setenv("REDIS_HOST", "myhost")
	t.Setenv("REDIS_PORT", "6380")

	got := ResolveRedisAddr("")
	if got != "myhost:6380" {
		t.Fatalf("expected 'myhost:6380', got %q", got)
	}
}

func TestResolveRedisAddr_EmptyWhenNothingSet(t *testing.T) {
	t.Setenv("REDIS_ADDR", "")
	t.Setenv("REDIS_HOST", "")
	t.Setenv("REDIS_PORT", "")

	got := ResolveRedisAddr("")
	if got != "" {
		t.Fatalf("expected empty string, got %q", got)
	}
}

func TestResolveRedisAddr_MissingPortReturnsEmpty(t *testing.T) {
	t.Setenv("REDIS_ADDR", "")
	t.Setenv("REDIS_HOST", "myhost")
	t.Setenv("REDIS_PORT", "")

	got := ResolveRedisAddr("")
	if got != "" {
		t.Fatalf("expected empty string when port missing, got %q", got)
	}
}

func TestResolveRedisAddr_MissingHostReturnsEmpty(t *testing.T) {
	t.Setenv("REDIS_ADDR", "")
	t.Setenv("REDIS_HOST", "")
	t.Setenv("REDIS_PORT", "6379")

	got := ResolveRedisAddr("")
	if got != "" {
		t.Fatalf("expected empty string when host missing, got %q", got)
	}
}

func TestResolveRedisAddr_ExplicitAddrOverridesEnv(t *testing.T) {
	t.Setenv("REDIS_ADDR", "env-redis:6379")

	got := ResolveRedisAddr("explicit:6379")
	if got != "explicit:6379" {
		t.Fatalf("expected explicit addr to take precedence, got %q", got)
	}
}
