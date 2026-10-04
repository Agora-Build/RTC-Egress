package egress

import (
	"encoding/json"
	"testing"

	"github.com/AgoraIO/RTC-Egress/pkg/queue"
)

func streamPayload(cmd string) map[string]interface{} {
	payload := map[string]interface{}{
		"channel": "streamtest", "access_token": "testtoken123", "workerUid": float64(999),
		"layout": "spotlight", "users": []string{"speaker", "guest"},
	}
	if cmd == "rtmp" {
		payload["output_url"] = "rtmp://127.0.0.1:1935/live/test"
	} else {
		payload["output_url"] = "http://127.0.0.1:8889/test/whip"
		payload["output_token"] = "fixture-token"
	}
	return payload
}

func TestStreamReconnectValidation(t *testing.T) {
	for _, cmd := range []string{"rtmp", "whip"} {
		for _, scenario := range []struct {
			field string
			value interface{}
		}{
			{"output_reconnect_attempts", -1}, {"output_reconnect_attempts", 21},
			{"output_reconnect_attempts", 1.5}, {"output_reconnect_attempts", "5"},
			{"output_reconnect_attempts", nil},
			{"output_reconnect_delay_ms", 99}, {"output_reconnect_delay_ms", 10001},
			{"output_reconnect_delay_ms", nil},
		} {
			payload := streamPayload(cmd)
			payload[scenario.field] = scenario.value
			if err := ValidateStartTaskRequest(&TaskRequest{RequestID: "retrytest", Cmd: cmd, Action: "start", Payload: payload}); err == nil {
				t.Errorf("%s accepted %s=%v", cmd, scenario.field, scenario.value)
			}
		}
	}
}

func TestStreamReconnectSettingsReachWorker(t *testing.T) {
	for _, attempts := range []int{0, 5, 20} {
		payload := streamPayload("rtmp")
		if attempts != 5 {
			payload["output_reconnect_attempts"] = attempts
			payload["output_reconnect_delay_ms"] = 100
		}
		msg, err := buildUDSMessageFromQueueTask(&queue.Task{ID: "retrytest", Cmd: "rtmp", Action: "start", Payload: payload})
		if err != nil {
			t.Fatal(err)
		}
		encoded, err := json.Marshal(msg)
		if err != nil {
			t.Fatal(err)
		}
		var fields map[string]interface{}
		if err := json.Unmarshal(encoded, &fields); err != nil {
			t.Fatal(err)
		}
		if fields["output_reconnect_attempts"] != float64(attempts) {
			t.Fatalf("retry setting lost in worker payload: %s", encoded)
		}
		wantDelay := float64(100)
		if attempts == 5 {
			wantDelay = 1000
		}
		if fields["output_reconnect_delay_ms"] != wantDelay {
			t.Fatalf("retry delay lost in worker payload: %s", encoded)
		}
	}
}

func TestStreamDestinationValidation(t *testing.T) {
	for _, cmd := range []string{"rtmp", "whip"} {
		payload := streamPayload(cmd)
		if err := ValidateStartTaskRequest(&TaskRequest{RequestID: "streamtest", Cmd: cmd, Action: "start", Payload: payload}); err != nil {
			t.Fatalf("valid %s destination rejected: %v", cmd, err)
		}
		for _, scenario := range []struct {
			name   string
			change func(map[string]interface{})
		}{
			{"missing", func(p map[string]interface{}) { delete(p, "output_url") }},
			{"invalidurl", func(p map[string]interface{}) { p["output_url"] = "not-a-url" }},
			{"wrongprotocol", func(p map[string]interface{}) { p["output_url"] = "file:///tmp/stream" }},
			{"tokeninjection", func(p map[string]interface{}) { p["output_token"] = "bad\r\nHeader: value" }},
			{"timeout", func(p map[string]interface{}) { p["output_timeout_ms"] = -1 }},
			{"freestyle", func(p map[string]interface{}) { p["layout"] = "freestyle" }},
		} {
			t.Run(cmd+"/"+scenario.name, func(t *testing.T) {
				payload := streamPayload(cmd)
				scenario.change(payload)
				if err := ValidateStartTaskRequest(&TaskRequest{RequestID: "streamtest", Cmd: cmd, Action: "start", Payload: payload}); err == nil {
					t.Fatal("invalid streaming destination accepted")
				}
			})
		}
	}
}
