package egress

import (
	"encoding/json"
	"fmt"
	"net/url"
	"strings"
)

func readStreamOutput(payload map[string]interface{}) (string, string, int, error) {
	var destination, token string
	timeout := 5000
	for _, field := range []struct {
		name  string
		value interface{}
	}{
		{"output_url", &destination}, {"output_token", &token}, {"output_timeout_ms", &timeout},
	} {
		if value, exists := payload[field.name]; exists {
			data, err := json.Marshal(value)
			if err != nil {
				return "", "", 0, fmt.Errorf("invalid %s", field.name)
			}
			if err := json.Unmarshal(data, field.value); err != nil {
				return "", "", 0, fmt.Errorf("invalid %s", field.name)
			}
		}
	}
	return destination, token, timeout, nil
}

func validateStreamOutput(cmd, layout, destination, token string, timeout int) error {
	if cmd != "rtmp" && cmd != "whip" {
		return nil
	}
	if layout == "freestyle" {
		return fmt.Errorf("native streaming does not support freestyle layout")
	}
	parsed, err := url.Parse(destination)
	if err != nil || parsed.Hostname() == "" || parsed.Fragment != "" || strings.ContainsAny(destination, "\r\n\t ") {
		return fmt.Errorf("output_url must be a valid streaming destination")
	}
	if cmd == "rtmp" && parsed.Scheme != "rtmp" && parsed.Scheme != "rtmps" {
		return fmt.Errorf("RTMP output_url must use rtmp or rtmps")
	}
	if cmd == "whip" && parsed.Scheme != "http" && parsed.Scheme != "https" {
		return fmt.Errorf("WHIP output_url must use http or https")
	}
	if len(destination) > 1024 {
		return fmt.Errorf("output_url must be at most 1024 characters")
	}
	if len(token) > 512 || strings.ContainsAny(token, "\r\n\t ") {
		return fmt.Errorf("output_token must contain at most 512 characters without whitespace")
	}
	if cmd == "rtmp" && token != "" {
		return fmt.Errorf("output_token is only supported for WHIP")
	}
	if timeout < 1000 || timeout > 30000 {
		return fmt.Errorf("output_timeout_ms must be between 1000 and 30000")
	}
	return nil
}
