package egress

import (
	"encoding/json"
	"testing"

	"github.com/AgoraIO/RTC-Egress/pkg/queue"
)

func nativeLayoutPayload() map[string]interface{} {
	return map[string]interface{}{
		"channel": "layouttest", "access_token": "testtoken123", "workerUid": float64(999),
		"layout": "customized", "users": []string{"speaker", "guest"},
		"width": float64(320), "height": float64(180),
		"regions": []interface{}{
			map[string]interface{}{"uid": "speaker", "x": 0, "y": 0, "width": 320, "height": 180, "z": -1},
			map[string]interface{}{"uid": "guest", "x": 160, "y": 90, "width": 160, "height": 90, "z": 10},
		},
	}
}

func TestNativeLayoutSurvivesWorkerSerialization(t *testing.T) {
	msg, err := buildUDSMessageFromQueueTask(&queue.Task{
		ID: "layouttask", Cmd: "record", Action: "start", Payload: nativeLayoutPayload(),
	})
	if err != nil {
		t.Fatal(err)
	}
	encoded, err := json.Marshal(msg)
	if err != nil {
		t.Fatal(err)
	}
	var output map[string]interface{}
	if err := json.Unmarshal(encoded, &output); err != nil {
		t.Fatal(err)
	}
	regions, ok := output["regions"].([]interface{})
	if !ok || len(regions) != 2 {
		t.Fatalf("custom regions were lost: %s", encoded)
	}
	if output["width"] != float64(320) || output["height"] != float64(180) {
		t.Fatalf("canvas dimensions were lost: %s", encoded)
	}
}

func TestNativeLayoutRejectsInvalidRegionsBeforeQueueing(t *testing.T) {
	if err := ValidateStartTaskRequest(&TaskRequest{RequestID: "layouttest", Cmd: "record", Action: "start", Payload: nativeLayoutPayload()}); err != nil {
		t.Fatalf("valid layout rejected: %v", err)
	}
	for _, scenario := range []struct {
		name   string
		change func(map[string]interface{})
	}{
		{"missing", func(p map[string]interface{}) { delete(p, "regions") }},
		{"empty", func(p map[string]interface{}) { p["regions"] = []interface{}{} }},
		{"outofcanvas", func(p map[string]interface{}) { p["regions"].([]interface{})[1].(map[string]interface{})["x"] = 320 }},
		{"negative", func(p map[string]interface{}) { p["regions"].([]interface{})[0].(map[string]interface{})["x"] = -2 }},
		{"fractional", func(p map[string]interface{}) {
			p["regions"].([]interface{})[0].(map[string]interface{})["width"] = 12.5
		}},
		{"unaligned", func(p map[string]interface{}) { p["regions"].([]interface{})[0].(map[string]interface{})["x"] = 1 }},
		{"nouid", func(p map[string]interface{}) {
			delete(p["regions"].([]interface{})[0].(map[string]interface{}), "uid")
		}},
		{"invalidcanvas", func(p map[string]interface{}) { p["width"] = "320" }},
	} {
		t.Run(scenario.name, func(t *testing.T) {
			payload := nativeLayoutPayload()
			scenario.change(payload)
			err := ValidateStartTaskRequest(&TaskRequest{RequestID: "layouttest", Cmd: "record", Action: "start", Payload: payload})
			if err == nil {
				t.Fatal("invalid layout was accepted")
			}
		})
	}
}
