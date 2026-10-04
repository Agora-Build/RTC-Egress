package egress

import "testing"

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
