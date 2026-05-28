package egress

import (
	"strings"
	"testing"

	"github.com/AgoraIO/RTC-Egress/pkg/queue"
)

func TestBuildUDSMessageFromQueueTaskStart(t *testing.T) {
	task := &queue.Task{
		ID:        "start-1",
		Cmd:       "record",
		Action:    "start",
		Channel:   "demo",
		RequestID: "req-1",
		Payload: map[string]interface{}{
			"layout":         "flat",
			"channel":        "demo",
			"access_token":   "token-value-12345",
			"users":          []string{"user1", "user2"},
			"workerUid":      float64(101),
			"interval_in_ms": float64(15000),
		},
	}

	msg, err := buildUDSMessageFromQueueTask(task)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}

	if msg.TaskID != "start-1" {
		t.Fatalf("expected task id %q, got %q", "start-1", msg.TaskID)
	}
	if msg.Channel != "demo" {
		t.Fatalf("expected channel %q, got %q", "demo", msg.Channel)
	}
	if msg.WorkerUid != 101 {
		t.Fatalf("expected worker UID 101, got %d", msg.WorkerUid)
	}
	if msg.IntervalInMs != 15000 {
		t.Fatalf("expected interval 15000, got %d", msg.IntervalInMs)
	}
	if msg.Action != "start" {
		t.Fatalf("expected action start, got %s", msg.Action)
	}
	if msg.Cmd != "record" {
		t.Fatalf("expected cmd record, got %s", msg.Cmd)
	}
	if msg.Layout != "flat" {
		t.Fatalf("expected layout flat, got %s", msg.Layout)
	}
	if msg.AccessToken != "token-value-12345" {
		t.Fatalf("expected access token token-value-12345, got %s", msg.AccessToken)
	}
	if len(msg.Uid) != 2 || msg.Uid[0] != "user1" || msg.Uid[1] != "user2" {
		t.Fatalf("expected uid slice [user1 user2], got %#v", msg.Uid)
	}
}

func TestBuildUDSMessageFromQueueTaskStopUsesOriginalID(t *testing.T) {
	task := &queue.Task{
		ID:        "stop-1",
		Cmd:       "record",
		Action:    "stop",
		Channel:   "demo",
		RequestID: "req-stop",
		Payload: map[string]interface{}{
			"task_id": "orig-1",
		},
	}

	msg, err := buildUDSMessageFromQueueTask(task)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}

	if msg.TaskID != "orig-1" {
		t.Fatalf("expected task id %q, got %q", "orig-1", msg.TaskID)
	}
	if msg.Channel != "demo" {
		t.Fatalf("expected channel %q, got %q", "demo", msg.Channel)
	}
	if msg.Action != "stop" {
		t.Fatalf("expected action stop, got %s", msg.Action)
	}
	if msg.IntervalInMs != 0 {
		t.Fatalf("expected interval 0, got %d", msg.IntervalInMs)
	}
}

func TestBuildUDSMessageFromQueueTaskStopNilPayload(t *testing.T) {
	task := &queue.Task{
		ID:      "stop-2",
		Cmd:     "snapshot",
		Action:  "stop",
		Channel: "demo",
		Payload: nil,
	}

	msg, err := buildUDSMessageFromQueueTask(task)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}

	if msg.TaskID != "stop-2" {
		t.Fatalf("expected task id %q, got %q", "stop-2", msg.TaskID)
	}
	if msg.Channel != "demo" {
		t.Fatalf("expected channel %q, got %q", "demo", msg.Channel)
	}
}

func TestBuildUDSMessageFromQueueTaskStartMissingAccessToken(t *testing.T) {
	task := &queue.Task{
		ID:      "start-missing-token",
		Cmd:     "record",
		Action:  "start",
		Channel: "demo",
		Payload: map[string]interface{}{
			"workerUid": float64(10),
		},
	}

	_, err := buildUDSMessageFromQueueTask(task)
	if err == nil {
		t.Fatal("expected error for missing access_token, got nil")
	}
	if !strings.Contains(err.Error(), "access_token") {
		t.Fatalf("expected error to mention access_token, got %v", err)
	}
}

func TestBuildUDSMessageFromQueueTaskStartChannelFallback(t *testing.T) {
	task := &queue.Task{
		ID:      "start-channel-fallback",
		Cmd:     "snapshot",
		Action:  "start",
		Channel: "channel-from-task",
		Payload: map[string]interface{}{
			"access_token": "token-123456",
			"workerUid":    float64(77),
		},
	}

	msg, err := buildUDSMessageFromQueueTask(task)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}

	if msg.Channel != "channel-from-task" {
		t.Fatalf("expected channel to fallback to task.Channel, got %q", msg.Channel)
	}
	if msg.WorkerUid != 77 {
		t.Fatalf("expected worker UID 77, got %d", msg.WorkerUid)
	}
}

func TestBuildUDSMessageFromQueueTaskUsersConversion(t *testing.T) {
	task := &queue.Task{
		ID:      "start-with-uids",
		Cmd:     "record",
		Action:  "start",
		Channel: "demo",
		Payload: map[string]interface{}{
			"access_token": "token-123456",
			"workerUid":    float64(42),
			"users":        []interface{}{"user1", "user2"},
		},
	}

	msg, err := buildUDSMessageFromQueueTask(task)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}

	if len(msg.Uid) != 2 || msg.Uid[0] != "user1" || msg.Uid[1] != "user2" {
		t.Fatalf("expected uid slice [user1 user2], got %#v", msg.Uid)
	}
}

func TestBuildUDSMessageFromQueueTaskDefaultInterval(t *testing.T) {
	task := &queue.Task{
		ID:      "start-default-interval",
		Cmd:     "record",
		Action:  "start",
		Channel: "demo",
		Payload: map[string]interface{}{
			"access_token": "token-123456",
			"workerUid":    float64(55),
		},
	}

	msg, err := buildUDSMessageFromQueueTask(task)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}

	if msg.IntervalInMs != 20000 {
		t.Fatalf("expected default interval 20000, got %d", msg.IntervalInMs)
	}
}

func TestBuildUDSMessageVideoDecodeMode(t *testing.T) {
	tests := []struct {
		name     string
		payload  map[string]interface{}
		expected int
	}{
		{
			name: "explicit ffmpeg",
			payload: map[string]interface{}{
				"channel": "demo", "access_token": "token-123456", "workerUid": float64(42),
				"videoDecodeMode": float64(1),
			},
			expected: 1,
		},
		{
			name: "explicit passthrough",
			payload: map[string]interface{}{
				"channel": "demo", "access_token": "token-123456", "workerUid": float64(42),
				"videoDecodeMode": float64(0),
			},
			expected: 0,
		},
		{
			name: "explicit sdk",
			payload: map[string]interface{}{
				"channel": "demo", "access_token": "token-123456", "workerUid": float64(42),
				"videoDecodeMode": float64(2),
			},
			expected: 2,
		},
		{
			name: "default is auto",
			payload: map[string]interface{}{
				"channel": "demo", "access_token": "token-123456", "workerUid": float64(42),
			},
			expected: -1, // default to auto (C++ decides based on user count)
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			task := &queue.Task{
				ID: "test1", Cmd: "record", Action: "start", Channel: "demo",
				Payload: tt.payload,
			}
			msg, err := buildUDSMessageFromQueueTask(task)
			if err != nil {
				t.Fatalf("unexpected error: %v", err)
			}
			if msg.VideoDecodeMode != tt.expected {
				t.Fatalf("expected videoDecodeMode %d, got %d", tt.expected, msg.VideoDecodeMode)
			}
		})
	}
}

func TestValidateUDSMessage_InvalidVideoDecodeMode(t *testing.T) {
	msg := &UDSMessage{
		Cmd: "record", Action: "start", Layout: "flat",
		Channel: "demo", AccessToken: "token-123456", WorkerUid: 42,
		VideoDecodeMode: 5,
	}
	err := ValidateUDSMessage(msg)
	if err == nil {
		t.Fatal("expected error for invalid videoDecodeMode")
	}
	if !strings.Contains(err.Error(), "videoDecodeMode") {
		t.Fatalf("expected error about videoDecodeMode, got: %v", err)
	}
}

func TestBuildUDSMessageFromQueueTaskNilTask(t *testing.T) {
	_, err := buildUDSMessageFromQueueTask(nil)
	if err == nil {
		t.Fatal("expected error when task is nil")
	}
	if !strings.Contains(err.Error(), "task cannot be nil") {
		t.Fatalf("expected nil task error, got %v", err)
	}
}

func TestBuildUDSMessageFromQueueTaskUsersAsSingleString(t *testing.T) {
	task := &queue.Task{
		ID: "test1", Cmd: "record", Action: "start", Channel: "demo",
		Payload: map[string]interface{}{
			"channel": "demo", "access_token": "token-123456", "workerUid": float64(42),
			"users": "single-user",
		},
	}
	msg, err := buildUDSMessageFromQueueTask(task)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if len(msg.Uid) != 1 || msg.Uid[0] != "single-user" {
		t.Fatalf("expected uid [single-user], got %#v", msg.Uid)
	}
}

func TestBuildUDSMessageFromQueueTaskEmptyUsersString(t *testing.T) {
	task := &queue.Task{
		ID: "test1", Cmd: "record", Action: "start", Channel: "demo",
		Payload: map[string]interface{}{
			"channel": "demo", "access_token": "token-123456", "workerUid": float64(42),
			"users": "",
		},
	}
	msg, err := buildUDSMessageFromQueueTask(task)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if len(msg.Uid) != 0 {
		t.Fatalf("expected empty uid slice for empty string, got %#v", msg.Uid)
	}
}

func TestBuildUDSMessageFromQueueTaskFreestyleLayout(t *testing.T) {
	task := &queue.Task{
		ID: "test1", Cmd: "record", Action: "start", Channel: "demo",
		Payload: map[string]interface{}{
			"channel": "demo", "access_token": "token-123456", "workerUid": float64(42),
			"layout":             "freestyle",
			"freestyleCanvasUrl": "https://example.com/canvas",
		},
	}
	msg, err := buildUDSMessageFromQueueTask(task)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if msg.Layout != "freestyle" {
		t.Fatalf("expected freestyle layout, got %s", msg.Layout)
	}
	if msg.FreestyleCanvasUrl != "https://example.com/canvas" {
		t.Fatalf("expected canvas URL, got %s", msg.FreestyleCanvasUrl)
	}
}

func TestBuildUDSMessageFromQueueTaskDefaultLayoutIsFlat(t *testing.T) {
	task := &queue.Task{
		ID: "test1", Cmd: "record", Action: "start", Channel: "demo",
		Payload: map[string]interface{}{
			"channel": "demo", "access_token": "token-123456", "workerUid": float64(42),
		},
	}
	msg, err := buildUDSMessageFromQueueTask(task)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if msg.Layout != "flat" {
		t.Fatalf("expected default layout flat, got %s", msg.Layout)
	}
}

func TestBuildUDSMessageFromQueueTaskEmptyLayoutDefaultsFlat(t *testing.T) {
	task := &queue.Task{
		ID: "test1", Cmd: "record", Action: "start", Channel: "demo",
		Payload: map[string]interface{}{
			"channel": "demo", "access_token": "token-123456", "workerUid": float64(42),
			"layout": "",
		},
	}
	msg, err := buildUDSMessageFromQueueTask(task)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if msg.Layout != "flat" {
		t.Fatalf("expected default layout flat for empty string, got %s", msg.Layout)
	}
}

func TestBuildUDSMessageFromQueueTaskStartMissingWorkerUid(t *testing.T) {
	task := &queue.Task{
		ID: "test1", Cmd: "record", Action: "start", Channel: "demo",
		Payload: map[string]interface{}{
			"channel":      "demo",
			"access_token": "token-123456",
		},
	}
	_, err := buildUDSMessageFromQueueTask(task)
	if err == nil {
		t.Fatal("expected error for missing workerUid on start")
	}
	if !strings.Contains(err.Error(), "workerUid") {
		t.Fatalf("expected error about workerUid, got: %v", err)
	}
}

func TestBuildUDSMessageFromQueueTaskStartMissingChannel(t *testing.T) {
	task := &queue.Task{
		ID: "test1", Cmd: "record", Action: "start",
		Payload: map[string]interface{}{
			"access_token": "token-123456",
			"workerUid":    float64(42),
		},
	}
	_, err := buildUDSMessageFromQueueTask(task)
	if err == nil {
		t.Fatal("expected error for missing channel on start")
	}
	if !strings.Contains(err.Error(), "channel") {
		t.Fatalf("expected error about channel, got: %v", err)
	}
}

func TestBuildUDSMessageFromQueueTaskChannelMustBeString(t *testing.T) {
	task := &queue.Task{
		ID: "test1", Cmd: "record", Action: "start", Channel: "demo",
		Payload: map[string]interface{}{
			"channel":      float64(123),
			"access_token": "token-123456",
			"workerUid":    float64(42),
		},
	}
	_, err := buildUDSMessageFromQueueTask(task)
	if err == nil {
		t.Fatal("expected error for non-string channel")
	}
	if !strings.Contains(err.Error(), "channel must be a string") {
		t.Fatalf("expected 'channel must be a string' error, got: %v", err)
	}
}

func TestBuildUDSMessageFromQueueTaskWorkerUidInt64(t *testing.T) {
	task := &queue.Task{
		ID: "test1", Cmd: "record", Action: "start", Channel: "demo",
		Payload: map[string]interface{}{
			"channel": "demo", "access_token": "token-123456",
			"workerUid": int64(99),
		},
	}
	msg, err := buildUDSMessageFromQueueTask(task)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if msg.WorkerUid != 99 {
		t.Fatalf("expected worker UID 99, got %d", msg.WorkerUid)
	}
}

func TestBuildUDSMessageFromQueueTaskWorkerUidInt(t *testing.T) {
	task := &queue.Task{
		ID: "test1", Cmd: "record", Action: "start", Channel: "demo",
		Payload: map[string]interface{}{
			"channel": "demo", "access_token": "token-123456",
			"workerUid": int(88),
		},
	}
	msg, err := buildUDSMessageFromQueueTask(task)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if msg.WorkerUid != 88 {
		t.Fatalf("expected worker UID 88, got %d", msg.WorkerUid)
	}
}

func TestBuildUDSMessageFromQueueTaskWorkerUidInvalidType(t *testing.T) {
	task := &queue.Task{
		ID: "test1", Cmd: "record", Action: "start", Channel: "demo",
		Payload: map[string]interface{}{
			"channel": "demo", "access_token": "token-123456",
			"workerUid": "not-a-number",
		},
	}
	_, err := buildUDSMessageFromQueueTask(task)
	if err == nil {
		t.Fatal("expected error for string workerUid")
	}
	if !strings.Contains(err.Error(), "workerUid must be a number") {
		t.Fatalf("expected 'workerUid must be a number' error, got: %v", err)
	}
}

func TestBuildUDSMessageFromQueueTaskIntervalInvalidType(t *testing.T) {
	task := &queue.Task{
		ID: "test1", Cmd: "record", Action: "start", Channel: "demo",
		Payload: map[string]interface{}{
			"channel": "demo", "access_token": "token-123456",
			"workerUid":    float64(42),
			"interval_in_ms": "not-a-number",
		},
	}
	_, err := buildUDSMessageFromQueueTask(task)
	if err == nil {
		t.Fatal("expected error for string interval_in_ms")
	}
	if !strings.Contains(err.Error(), "interval_in_ms must be a number") {
		t.Fatalf("expected interval_in_ms error, got: %v", err)
	}
}

func TestBuildUDSMessageFromQueueTaskAccessTokenMustBeString(t *testing.T) {
	task := &queue.Task{
		ID: "test1", Cmd: "record", Action: "start", Channel: "demo",
		Payload: map[string]interface{}{
			"channel":      "demo",
			"access_token": float64(12345),
			"workerUid":    float64(42),
		},
	}
	_, err := buildUDSMessageFromQueueTask(task)
	if err == nil {
		t.Fatal("expected error for non-string access_token")
	}
	if !strings.Contains(err.Error(), "access_token must be a string") {
		t.Fatalf("expected 'access_token must be a string' error, got: %v", err)
	}
}

func TestBuildUDSMessageFromQueueTaskCanvasUrlTrimmed(t *testing.T) {
	task := &queue.Task{
		ID: "test1", Cmd: "record", Action: "stop", Channel: "demo",
		Payload: map[string]interface{}{
			"freestyleCanvasUrl": "  https://example.com/canvas  ",
		},
	}
	msg, err := buildUDSMessageFromQueueTask(task)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if msg.FreestyleCanvasUrl != "https://example.com/canvas" {
		t.Fatalf("expected trimmed canvas URL, got %q", msg.FreestyleCanvasUrl)
	}
}

func TestBuildUDSMessageAllCmdTypes(t *testing.T) {
	for _, cmd := range []string{"snapshot", "record", "rtmp", "whip"} {
		task := &queue.Task{
			ID: "test1", Cmd: cmd, Action: "start", Channel: "demo",
			Payload: map[string]interface{}{
				"channel": "demo", "access_token": "token-123456", "workerUid": float64(42),
			},
		}
		msg, err := buildUDSMessageFromQueueTask(task)
		if err != nil {
			t.Fatalf("cmd %q: unexpected error: %v", cmd, err)
		}
		if msg.Cmd != cmd {
			t.Fatalf("expected cmd %q, got %q", cmd, msg.Cmd)
		}
	}
}
