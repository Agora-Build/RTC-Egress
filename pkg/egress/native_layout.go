package egress

import (
	"encoding/json"
	"fmt"
)

type LayoutRegion struct {
	UID    string `json:"uid"`
	X      int    `json:"x"`
	Y      int    `json:"y"`
	Width  int    `json:"width"`
	Height int    `json:"height"`
	Z      int    `json:"z"`
}

func readNativeLayout(payload map[string]interface{}) ([]LayoutRegion, int, int, error) {
	var regions []LayoutRegion
	var width, height int
	for _, field := range []struct {
		name  string
		value interface{}
	}{
		{"regions", &regions}, {"width", &width}, {"height", &height},
	} {
		if value, exists := payload[field.name]; exists {
			data, err := json.Marshal(value)
			if err != nil {
				return nil, 0, 0, fmt.Errorf("invalid %s", field.name)
			}
			if err := json.Unmarshal(data, field.value); err != nil {
				return nil, 0, 0, fmt.Errorf("invalid %s: %w", field.name, err)
			}
		}
	}
	return regions, width, height, nil
}

func validateNativeLayout(layout string, regions []LayoutRegion, width, height int) error {
	if layout == "freestyle" {
		return nil
	}
	for _, size := range []struct {
		name  string
		value int
	}{{"width", width}, {"height", height}} {
		if size.value != 0 && (size.value < 2 || size.value > 8192 || size.value%2 != 0) {
			return fmt.Errorf("%s must be an even integer between 2 and 8192", size.name)
		}
	}
	if layout == "customized" && len(regions) == 0 {
		return fmt.Errorf("customized layout requires regions")
	}
	if layout != "customized" && len(regions) != 0 {
		return fmt.Errorf("regions require customized layout")
	}
	if len(regions) > 32 {
		return fmt.Errorf("regions must contain at most 32 entries")
	}
	canvasWidth, canvasHeight := width, height
	if canvasWidth == 0 {
		canvasWidth = 8192
	}
	if canvasHeight == 0 {
		canvasHeight = 8192
	}
	for i, region := range regions {
		if region.UID == "" || len(region.UID) > 32 {
			return fmt.Errorf("regions[%d].uid must contain 1 to 32 characters", i)
		}
		if region.X < 0 || region.Y < 0 || region.Width < 2 || region.Height < 2 ||
			region.X > canvasWidth || region.Y > canvasHeight ||
			region.Width > canvasWidth-region.X || region.Height > canvasHeight-region.Y {
			return fmt.Errorf("regions[%d] must fit within the output canvas", i)
		}
		if region.X%2 != 0 || region.Y%2 != 0 || region.Width%2 != 0 || region.Height%2 != 0 {
			return fmt.Errorf("regions[%d] coordinates and dimensions must be even for YUV420", i)
		}
		if region.Z < -2147483648 || region.Z > 2147483647 {
			return fmt.Errorf("regions[%d].z exceeds the signed 32-bit range", i)
		}
	}
	return nil
}
