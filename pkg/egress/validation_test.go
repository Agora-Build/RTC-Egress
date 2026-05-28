package egress

import (
	"strings"
	"testing"

	"github.com/AgoraIO/RTC-Egress/pkg/queue"
)

// --- ValidateTaskID ---

func TestValidateTaskID_Valid(t *testing.T) {
	if err := ValidateTaskID("abc123def456"); err != nil {
		t.Fatalf("expected valid task ID, got error: %v", err)
	}
}

func TestValidateTaskID_Empty(t *testing.T) {
	err := ValidateTaskID("")
	if err == nil {
		t.Fatal("expected error for empty task ID")
	}
}

func TestValidateTaskID_TooLong(t *testing.T) {
	longID := strings.Repeat("a", 65)
	err := ValidateTaskID(longID)
	if err == nil {
		t.Fatal("expected error for task ID > 64 chars")
	}
}

func TestValidateTaskID_InvalidChars(t *testing.T) {
	err := ValidateTaskID("ABC-not-hex!")
	if err == nil {
		t.Fatal("expected error for non-hex task ID")
	}
}

// --- ValidateRequestID ---

func TestValidateRequestID_Valid(t *testing.T) {
	if err := ValidateRequestID("req123ABC"); err != nil {
		t.Fatalf("expected valid request ID, got error: %v", err)
	}
}

func TestValidateRequestID_Empty(t *testing.T) {
	err := ValidateRequestID("")
	if err == nil {
		t.Fatal("expected error for empty request ID")
	}
}

func TestValidateRequestID_TooLong(t *testing.T) {
	longID := strings.Repeat("a", 33)
	err := ValidateRequestID(longID)
	if err == nil {
		t.Fatal("expected error for request ID > 32 chars")
	}
}

func TestValidateRequestID_InvalidChars(t *testing.T) {
	err := ValidateRequestID("req-with-dashes")
	if err == nil {
		t.Fatal("expected error for request ID with special chars")
	}
}

// --- ValidateUDSMessage ---

func TestValidateUDSMessage_ValidStartSnapshot(t *testing.T) {
	msg := &UDSMessage{
		Cmd:         "snapshot",
		Action:      "start",
		Layout:      "flat",
		Channel:     "test-channel",
		AccessToken: "test-token-12345",
		WorkerUid:   42,
	}
	if err := ValidateUDSMessage(msg); err != nil {
		t.Fatalf("expected valid message, got error: %v", err)
	}
}

func TestValidateUDSMessage_ValidStopDoesNotRequireFields(t *testing.T) {
	msg := &UDSMessage{
		Cmd:    "record",
		Action: "stop",
		Layout: "flat",
	}
	if err := ValidateUDSMessage(msg); err != nil {
		t.Fatalf("stop action should not require channel/token/workerUid, got error: %v", err)
	}
}

func TestValidateUDSMessage_InvalidCmd(t *testing.T) {
	msg := &UDSMessage{
		Cmd:    "invalid",
		Action: "start",
		Layout: "flat",
	}
	err := ValidateUDSMessage(msg)
	if err == nil {
		t.Fatal("expected error for invalid cmd")
	}
}

func TestValidateUDSMessage_InvalidAction(t *testing.T) {
	msg := &UDSMessage{
		Cmd:    "snapshot",
		Action: "restart",
		Layout: "flat",
	}
	err := ValidateUDSMessage(msg)
	if err == nil {
		t.Fatal("expected error for invalid action")
	}
}

func TestValidateUDSMessage_InvalidLayout(t *testing.T) {
	msg := &UDSMessage{
		Cmd:    "record",
		Action: "stop",
		Layout: "invalid-layout",
	}
	err := ValidateUDSMessage(msg)
	if err == nil {
		t.Fatal("expected error for invalid layout")
	}
}

func TestValidateUDSMessage_TooManyUIDs(t *testing.T) {
	uids := make([]string, 33)
	for i := range uids {
		uids[i] = "user"
	}
	msg := &UDSMessage{
		Cmd:         "record",
		Action:      "start",
		Layout:      "flat",
		Channel:     "ch",
		AccessToken: "token-12345678",
		WorkerUid:   1,
		Uid:         uids,
	}
	err := ValidateUDSMessage(msg)
	if err == nil {
		t.Fatal("expected error for > 32 UIDs")
	}
}

func TestValidateUDSMessage_StartMissingChannel(t *testing.T) {
	msg := &UDSMessage{
		Cmd:         "snapshot",
		Action:      "start",
		Layout:      "flat",
		AccessToken: "test-token-12345",
		WorkerUid:   1,
	}
	err := ValidateUDSMessage(msg)
	if err == nil {
		t.Fatal("expected error for missing channel on start")
	}
}

func TestValidateUDSMessage_StartMissingAccessToken(t *testing.T) {
	msg := &UDSMessage{
		Cmd:       "snapshot",
		Action:    "start",
		Layout:    "flat",
		Channel:   "ch",
		WorkerUid: 1,
	}
	err := ValidateUDSMessage(msg)
	if err == nil {
		t.Fatal("expected error for missing access_token on start")
	}
}

func TestValidateUDSMessage_StartMissingWorkerUid(t *testing.T) {
	msg := &UDSMessage{
		Cmd:         "snapshot",
		Action:      "start",
		Layout:      "flat",
		Channel:     "ch",
		AccessToken: "test-token-12345",
	}
	err := ValidateUDSMessage(msg)
	if err == nil {
		t.Fatal("expected error for missing workerUid on start")
	}
}

func TestValidateUDSMessage_FreestyleCanvasUrlTooLong(t *testing.T) {
	msg := &UDSMessage{
		Cmd:                "record",
		Action:             "stop",
		Layout:             "flat",
		FreestyleCanvasUrl: "http://" + strings.Repeat("x", 1020),
	}
	err := ValidateUDSMessage(msg)
	if err == nil {
		t.Fatal("expected error for canvas URL > 1024 chars")
	}
}

func TestValidateUDSMessage_FreestyleCanvasUrlNoHTTP(t *testing.T) {
	msg := &UDSMessage{
		Cmd:                "record",
		Action:             "stop",
		Layout:             "flat",
		FreestyleCanvasUrl: "ftp://example.com",
	}
	err := ValidateUDSMessage(msg)
	if err == nil {
		t.Fatal("expected error for non-http canvas URL")
	}
}

func TestValidateUDSMessage_FreestyleCanvasUrlWithSpaces(t *testing.T) {
	msg := &UDSMessage{
		Cmd:                "record",
		Action:             "stop",
		Layout:             "flat",
		FreestyleCanvasUrl: "http://example.com/path with spaces",
	}
	err := ValidateUDSMessage(msg)
	if err == nil {
		t.Fatal("expected error for canvas URL with spaces")
	}
}

func TestValidateUDSMessage_AllValidCmds(t *testing.T) {
	for _, cmd := range []string{"snapshot", "record", "rtmp", "whip"} {
		msg := &UDSMessage{
			Cmd:    cmd,
			Action: "stop",
			Layout: "flat",
		}
		if err := ValidateUDSMessage(msg); err != nil {
			t.Fatalf("cmd %q should be valid, got error: %v", cmd, err)
		}
	}
}

func TestValidateUDSMessage_AllValidLayouts(t *testing.T) {
	for _, layout := range []string{"flat", "spotlight", "customized", "freestyle"} {
		msg := &UDSMessage{
			Cmd:    "record",
			Action: "stop",
			Layout: layout,
		}
		if err := ValidateUDSMessage(msg); err != nil {
			t.Fatalf("layout %q should be valid, got error: %v", layout, err)
		}
	}
}

// --- ValidateStopTaskRequest ---

func TestValidateStopTaskRequest_Valid(t *testing.T) {
	err := ValidateStopTaskRequest("req123", "abcdef1234567890")
	if err != nil {
		t.Fatalf("expected valid, got error: %v", err)
	}
}

func TestValidateStopTaskRequest_EmptyRequestID(t *testing.T) {
	err := ValidateStopTaskRequest("", "abcdef1234567890")
	if err == nil {
		t.Fatal("expected error for empty request ID")
	}
}

func TestValidateStopTaskRequest_EmptyTaskID(t *testing.T) {
	err := ValidateStopTaskRequest("req123", "")
	if err == nil {
		t.Fatal("expected error for empty task ID")
	}
}

// --- ValidateRedisTask ---

func TestValidateRedisTask_Nil(t *testing.T) {
	err := ValidateRedisTask(nil)
	if err == nil {
		t.Fatal("expected error for nil task")
	}
}

func TestValidateRedisTask_InvalidCmd(t *testing.T) {
	task := &queue.Task{
		ID:     "abcdef1234567890",
		Cmd:    "invalid",
		Action: "start",
	}
	err := ValidateRedisTask(task)
	if err == nil {
		t.Fatal("expected error for invalid cmd")
	}
}

func TestValidateRedisTask_InvalidAction(t *testing.T) {
	task := &queue.Task{
		ID:     "abcdef1234567890",
		Cmd:    "record",
		Action: "restart",
	}
	err := ValidateRedisTask(task)
	if err == nil {
		t.Fatal("expected error for invalid action")
	}
}

func TestValidateRedisTask_ChannelTooLong(t *testing.T) {
	task := &queue.Task{
		ID:      "abcdef1234567890",
		Cmd:     "record",
		Action:  "stop",
		Channel: strings.Repeat("x", 65),
	}
	err := ValidateRedisTask(task)
	if err == nil {
		t.Fatal("expected error for channel > 64 chars")
	}
}

func TestValidateRedisTask_ChannelInvalidChars(t *testing.T) {
	task := &queue.Task{
		ID:      "abcdef1234567890",
		Cmd:     "record",
		Action:  "stop",
		Channel: "chan with spaces",
	}
	err := ValidateRedisTask(task)
	if err == nil {
		t.Fatal("expected error for channel with spaces")
	}
}

func TestValidateRedisTask_ValidStopTask(t *testing.T) {
	task := &queue.Task{
		ID:      "abcdef1234567890",
		Cmd:     "snapshot",
		Action:  "stop",
		Channel: "valid-channel",
	}
	err := ValidateRedisTask(task)
	if err != nil {
		t.Fatalf("expected valid stop task, got error: %v", err)
	}
}

// --- deriveCompletionResult ---

func TestDeriveCompletionResultFailurePreservesMessage(t *testing.T) {
	completion := &UDSCompletionMessage{
		TaskID:  "task-1",
		Status:  "failed",
		Message: "disk full",
		Error:   "IO error",
	}

	status, message := deriveCompletionResult(completion)

	if status != queue.TaskStateFailed {
		t.Fatalf("expected FAILED, got %q", status)
	}
	// Message should be preferred over Error when both present
	if message != "disk full" {
		t.Fatalf("expected message 'disk full', got %q", message)
	}
}

func TestDeriveCompletionResultUnknownStatus(t *testing.T) {
	completion := &UDSCompletionMessage{
		TaskID: "task-1",
		Status: "unknown",
	}

	status, _ := deriveCompletionResult(completion)

	if status != queue.TaskStateFailed {
		t.Fatalf("expected FAILED for unknown status, got %q", status)
	}
}
