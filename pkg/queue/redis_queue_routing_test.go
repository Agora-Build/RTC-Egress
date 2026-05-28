package queue

import "testing"

func TestGetQueueKey_NativeFlat(t *testing.T) {
	rq := NewRedisQueue("localhost:6379", "", 0, 60, "")
	got := rq.getQueueKey("flat", "snapshot", "chan1", "")
	if got != "egress:snapshot:chan1" {
		t.Fatalf("expected 'egress:snapshot:chan1', got %q", got)
	}
}

func TestGetQueueKey_NativeFlatWithRegion(t *testing.T) {
	rq := NewRedisQueue("localhost:6379", "", 0, 60, "us-west")
	got := rq.getQueueKey("flat", "record", "chan2", "us-west")
	if got != "egress:us-west:record:chan2" {
		t.Fatalf("expected 'egress:us-west:record:chan2', got %q", got)
	}
}

func TestGetQueueKey_FreestyleGlobal(t *testing.T) {
	rq := NewRedisQueue("localhost:6379", "", 0, 60, "")
	got := rq.getQueueKey("freestyle", "record", "chan3", "")
	if got != "egress:web:record:chan3" {
		t.Fatalf("expected 'egress:web:record:chan3', got %q", got)
	}
}

func TestGetQueueKey_FreestyleWithRegion(t *testing.T) {
	rq := NewRedisQueue("localhost:6379", "", 0, 60, "eu")
	got := rq.getQueueKey("freestyle", "snapshot", "chan4", "eu")
	if got != "egress:eu:web:snapshot:chan4" {
		t.Fatalf("expected 'egress:eu:web:snapshot:chan4', got %q", got)
	}
}

func TestGetQueueKey_SpotlightLayoutIsNative(t *testing.T) {
	rq := NewRedisQueue("localhost:6379", "", 0, 60, "")
	got := rq.getQueueKey("spotlight", "record", "chan5", "")
	if got != "egress:record:chan5" {
		t.Fatalf("spotlight layout should route to native queue, got %q", got)
	}
}

func TestBuildDedupeKey(t *testing.T) {
	rq := NewRedisQueue("localhost:6379", "", 0, 60, "")
	got := rq.buildDedupeKey("snapshot", "chan1", "req1")
	expected := "egress:dedupe:snapshot:chan1:req1"
	if got != expected {
		t.Fatalf("expected %q, got %q", expected, got)
	}
}

func TestBuildLeaseKey(t *testing.T) {
	rq := NewRedisQueue("localhost:6379", "", 0, 60, "")
	got := rq.BuildLeaseKey("task-abc")
	expected := "egress:lease:task-abc"
	if got != expected {
		t.Fatalf("expected %q, got %q", expected, got)
	}
}

func TestGenerateTaskID_Deterministic(t *testing.T) {
	rq := NewRedisQueue("localhost:6379", "", 0, 60, "")
	id1 := rq.generateTaskID("snapshot", "chan", "req1")
	id2 := rq.generateTaskID("snapshot", "chan", "req1")

	// IDs include time.Now().UnixNano() so they should differ
	if id1 == id2 {
		t.Fatal("task IDs should be unique across calls (time-based)")
	}

	// Should be 16 hex chars (8 bytes of SHA256)
	if len(id1) != 16 {
		t.Fatalf("expected 16 char hex ID, got %d chars: %s", len(id1), id1)
	}
}

func TestGetQueueKey_CustomizedLayoutIsNative(t *testing.T) {
	rq := NewRedisQueue("localhost:6379", "", 0, 60, "")
	got := rq.getQueueKey("customized", "record", "chan6", "")
	if got != "egress:record:chan6" {
		t.Fatalf("customized layout should route to native queue, got %q", got)
	}
}

func TestGetQueueKey_CustomizedLayoutWithRegion(t *testing.T) {
	rq := NewRedisQueue("localhost:6379", "", 0, 60, "ap-east")
	got := rq.getQueueKey("customized", "snapshot", "chan7", "ap-east")
	if got != "egress:ap-east:snapshot:chan7" {
		t.Fatalf("expected 'egress:ap-east:snapshot:chan7', got %q", got)
	}
}

func TestGetQueueKey_SpotlightWithRegion(t *testing.T) {
	rq := NewRedisQueue("localhost:6379", "", 0, 60, "us-east")
	got := rq.getQueueKey("spotlight", "record", "chan8", "us-east")
	if got != "egress:us-east:record:chan8" {
		t.Fatalf("expected 'egress:us-east:record:chan8', got %q", got)
	}
}

func TestGetQueueKey_FreestyleSnapshot(t *testing.T) {
	rq := NewRedisQueue("localhost:6379", "", 0, 60, "")
	got := rq.getQueueKey("freestyle", "snapshot", "chan9", "")
	if got != "egress:web:snapshot:chan9" {
		t.Fatalf("expected 'egress:web:snapshot:chan9', got %q", got)
	}
}

func TestTTL(t *testing.T) {
	rq := NewRedisQueue("localhost:6379", "", 0, 120, "")
	if rq.TTL().Seconds() != 120 {
		t.Fatalf("expected TTL 120s, got %v", rq.TTL())
	}
}

func TestTTL_ZeroValue(t *testing.T) {
	rq := NewRedisQueue("localhost:6379", "", 0, 0, "")
	if rq.TTL().Seconds() != 0 {
		t.Fatalf("expected TTL 0s, got %v", rq.TTL())
	}
}
