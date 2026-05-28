package egress

import (
	"testing"

	"github.com/AgoraIO/RTC-Egress/pkg/queue"
)

// --- isValidQueueKey ---

func TestIsValidQueueKey_GlobalSnapshot(t *testing.T) {
	// egress:snapshot:channel
	if !isValidQueueKey([]string{"egress", "snapshot", "ch1"}) {
		t.Fatal("expected valid for global snapshot queue")
	}
}

func TestIsValidQueueKey_GlobalRecord(t *testing.T) {
	if !isValidQueueKey([]string{"egress", "record", "ch1"}) {
		t.Fatal("expected valid for global record queue")
	}
}

func TestIsValidQueueKey_GlobalWebQueue(t *testing.T) {
	// egress:web:record:channel
	if !isValidQueueKey([]string{"egress", "web", "record", "ch1"}) {
		t.Fatal("expected valid for global web queue")
	}
}

func TestIsValidQueueKey_RegionalQueue(t *testing.T) {
	// egress:us-west:snapshot:channel
	if !isValidQueueKey([]string{"egress", "us-west", "snapshot", "ch1"}) {
		t.Fatal("expected valid for regional queue")
	}
}

func TestIsValidQueueKey_RegionalWebQueue(t *testing.T) {
	// egress:eu:web:record:channel
	if !isValidQueueKey([]string{"egress", "eu", "web", "record", "ch1"}) {
		t.Fatal("expected valid for regional web queue")
	}
}

func TestIsValidQueueKey_StopQueue(t *testing.T) {
	// egress:stop:pod-id
	if !isValidQueueKey([]string{"egress", "stop", "pod-123"}) {
		t.Fatal("expected valid for stop queue")
	}
}

func TestIsValidQueueKey_InvalidPrefix(t *testing.T) {
	if isValidQueueKey([]string{"other", "snapshot", "ch1"}) {
		t.Fatal("expected invalid for non-egress prefix")
	}
}

func TestIsValidQueueKey_TooShort(t *testing.T) {
	if isValidQueueKey([]string{"egress", "snapshot"}) {
		t.Fatal("expected invalid for only 2 parts")
	}
}

func TestIsValidQueueKey_InvalidCmd(t *testing.T) {
	if isValidQueueKey([]string{"egress", "invalid_cmd", "ch1"}) {
		t.Fatal("expected invalid for unsupported command")
	}
}

func TestIsValidQueueKey_RtmpAndWhip(t *testing.T) {
	if !isValidQueueKey([]string{"egress", "rtmp", "ch1"}) {
		t.Fatal("expected valid for rtmp queue")
	}
	if !isValidQueueKey([]string{"egress", "whip", "ch1"}) {
		t.Fatal("expected valid for whip queue")
	}
}

func TestIsValidQueueKey_Empty(t *testing.T) {
	if isValidQueueKey(nil) {
		t.Fatal("expected invalid for nil parts")
	}
	if isValidQueueKey([]string{}) {
		t.Fatal("expected invalid for empty parts")
	}
}

// --- isWebTask ---

func TestIsWebTask_BySourceQueueGlobalWeb(t *testing.T) {
	wm := &WorkerManager{mode: ModeNative}
	task := &queue.Task{
		SourceQueue: "egress:web:record:ch1",
	}
	if !wm.isWebTask(task) {
		t.Fatal("expected web task for egress:web:* source queue")
	}
}

func TestIsWebTask_BySourceQueueRegionalWeb(t *testing.T) {
	wm := &WorkerManager{mode: ModeNative}
	task := &queue.Task{
		SourceQueue: "egress:eu:web:record:ch1",
	}
	if !wm.isWebTask(task) {
		t.Fatal("expected web task for egress:region:web:* source queue")
	}
}

func TestIsWebTask_BySourceQueueNative(t *testing.T) {
	wm := &WorkerManager{mode: ModeNative}
	task := &queue.Task{
		SourceQueue: "egress:snapshot:ch1",
	}
	if wm.isWebTask(task) {
		t.Fatal("expected native task for egress:snapshot:* source queue")
	}
}

func TestIsWebTask_ByPayloadFreestyle(t *testing.T) {
	wm := &WorkerManager{mode: ModeNative}
	task := &queue.Task{
		Payload: map[string]interface{}{
			"layout": "freestyle",
		},
	}
	if !wm.isWebTask(task) {
		t.Fatal("expected web task for freestyle layout")
	}
}

func TestIsWebTask_ByPayloadFreestyleCaseInsensitive(t *testing.T) {
	wm := &WorkerManager{mode: ModeNative}
	task := &queue.Task{
		Payload: map[string]interface{}{
			"layout": "Freestyle",
		},
	}
	if !wm.isWebTask(task) {
		t.Fatal("expected web task for Freestyle layout (case insensitive)")
	}
}

func TestIsWebTask_ByPayloadFlat(t *testing.T) {
	wm := &WorkerManager{mode: ModeNative}
	task := &queue.Task{
		Payload: map[string]interface{}{
			"layout": "flat",
		},
	}
	if wm.isWebTask(task) {
		t.Fatal("expected native task for flat layout in native mode")
	}
}

func TestIsWebTask_FallbackToMode(t *testing.T) {
	wmNative := &WorkerManager{mode: ModeNative}
	wmWeb := &WorkerManager{mode: ModeWeb}
	task := &queue.Task{} // no source queue, no payload

	if wmNative.isWebTask(task) {
		t.Fatal("native mode manager should not return web for empty task")
	}
	if !wmWeb.isWebTask(task) {
		t.Fatal("web mode manager should return web for empty task")
	}
}

func TestIsWebTask_NilTask(t *testing.T) {
	wm := &WorkerManager{mode: ModeNative}
	if wm.isWebTask(nil) {
		t.Fatal("nil task should not be web task in native mode")
	}
}

// --- isUnrecoverableError ---

func TestIsUnrecoverableError_Empty(t *testing.T) {
	wm := &WorkerManager{}
	if wm.isUnrecoverableError("") {
		t.Fatal("empty error should be recoverable")
	}
}

func TestIsUnrecoverableError_AuthErrors(t *testing.T) {
	wm := &WorkerManager{}
	authErrors := []string{
		"access_token is invalid",
		"Authentication failed for channel",
		"Unauthorized access to channel",
		"Permission denied for user",
		"Invalid token provided",
		"Token expired for session",
		"access_token_expired",
	}
	for _, msg := range authErrors {
		if !wm.isUnrecoverableError(msg) {
			t.Fatalf("expected unrecoverable for auth error: %q", msg)
		}
	}
}

func TestIsUnrecoverableError_DiskErrors(t *testing.T) {
	wm := &WorkerManager{}
	diskErrors := []string{
		"no space left on device",
		"Disk full, cannot write",
		"Storage full error",
		"out of disk space",
		"insufficient disk space",
	}
	for _, msg := range diskErrors {
		if !wm.isUnrecoverableError(msg) {
			t.Fatalf("expected unrecoverable for disk error: %q", msg)
		}
	}
}

func TestIsUnrecoverableError_RTCErrors(t *testing.T) {
	wm := &WorkerManager{}
	rtcErrors := []string{
		"banned by server",
		"rejected by server",
		"invalid app id",
		"invalid channel name",
		"too many broadcasters",
		"same uid login detected",
	}
	for _, msg := range rtcErrors {
		if !wm.isUnrecoverableError(msg) {
			t.Fatalf("expected unrecoverable for RTC error: %q", msg)
		}
	}
}

func TestIsUnrecoverableError_SDKErrors(t *testing.T) {
	wm := &WorkerManager{}
	sdkErrors := []string{
		"SDK error: initialization failed",
		"Agora error code 42",
		"sdk error: something went wrong",
		"Task failed due to unrecoverable SDK error",
		"Failed to create agora service",
		"Failed to create media node factory",
		"Failed to create video mixer",
	}
	for _, msg := range sdkErrors {
		if !wm.isUnrecoverableError(msg) {
			t.Fatalf("expected unrecoverable for SDK error: %q", msg)
		}
	}
}

func TestIsUnrecoverableError_RecoverableErrors(t *testing.T) {
	wm := &WorkerManager{}
	recoverableErrors := []string{
		"network timeout",
		"connection reset by peer",
		"temporary failure",
		"retry later",
		"worker busy",
	}
	for _, msg := range recoverableErrors {
		if wm.isUnrecoverableError(msg) {
			t.Fatalf("expected recoverable for: %q", msg)
		}
	}
}

// --- WebRecorderProxy ---

func TestNewWorkerManagerWebRecorderProxy_Defaults(t *testing.T) {
	proxy := NewWorkerManagerWebRecorderProxy(WebRecorderConfig{
		BaseURL: "http://localhost:8080",
	}, "pod-1")

	if proxy.GetPodID() != "pod-1" {
		t.Fatalf("expected pod-1, got %q", proxy.GetPodID())
	}
	if proxy.httpClient.Timeout != 30*1e9 { // 30s in nanoseconds
		t.Fatalf("expected 30s default timeout, got %v", proxy.httpClient.Timeout)
	}
	// Note: MaxRetries default is applied locally in makeHTTPRequest, not stored in config
}

func TestNewWorkerManagerWebRecorderProxy_CustomConfig(t *testing.T) {
	proxy := NewWorkerManagerWebRecorderProxy(WebRecorderConfig{
		BaseURL:    "http://recorder:9090",
		Timeout:    60,
		MaxRetries: 5,
		AuthToken:  "secret",
	}, "pod-2")

	if proxy.httpClient.Timeout != 60*1e9 {
		t.Fatalf("expected 60s timeout, got %v", proxy.httpClient.Timeout)
	}
	if proxy.config.MaxRetries != 5 {
		t.Fatalf("expected 5 max retries, got %d", proxy.config.MaxRetries)
	}
	if proxy.config.AuthToken != "secret" {
		t.Fatalf("expected auth token preserved")
	}
}
