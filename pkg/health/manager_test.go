package health

import "testing"

func TestCalculatePodStatus(t *testing.T) {
	hm := &HealthManager{}

	if got := hm.calculatePodStatus(50, 50); got != "healthy" {
		t.Fatalf("expected healthy, got %s", got)
	}
	if got := hm.calculatePodStatus(80, 50); got != "degraded" {
		t.Fatalf("expected degraded for high CPU, got %s", got)
	}
	if got := hm.calculatePodStatus(50, 85); got != "degraded" {
		t.Fatalf("expected degraded for high memory, got %s", got)
	}
	if got := hm.calculatePodStatus(95, 20); got != "unhealthy" {
		t.Fatalf("expected unhealthy for very high CPU, got %s", got)
	}
	if got := hm.calculatePodStatus(10, 92); got != "unhealthy" {
		t.Fatalf("expected unhealthy for very high memory, got %s", got)
	}
}

// Boundary tests at exact thresholds
func TestCalculatePodStatus_BoundaryAt75(t *testing.T) {
	hm := &HealthManager{}

	// Exactly 75% should be healthy (threshold is > 75)
	if got := hm.calculatePodStatus(75, 50); got != "healthy" {
		t.Fatalf("expected healthy at exactly 75%% CPU, got %s", got)
	}
	if got := hm.calculatePodStatus(50, 75); got != "healthy" {
		t.Fatalf("expected healthy at exactly 75%% memory, got %s", got)
	}

	// Just above 75% should be degraded
	if got := hm.calculatePodStatus(75.1, 50); got != "degraded" {
		t.Fatalf("expected degraded above 75%% CPU, got %s", got)
	}
	if got := hm.calculatePodStatus(50, 75.1); got != "degraded" {
		t.Fatalf("expected degraded above 75%% memory, got %s", got)
	}
}

func TestCalculatePodStatus_BoundaryAt90(t *testing.T) {
	hm := &HealthManager{}

	// Exactly 90% should be degraded (threshold is > 90)
	if got := hm.calculatePodStatus(90, 50); got != "degraded" {
		t.Fatalf("expected degraded at exactly 90%% CPU, got %s", got)
	}
	if got := hm.calculatePodStatus(50, 90); got != "degraded" {
		t.Fatalf("expected degraded at exactly 90%% memory, got %s", got)
	}

	// Just above 90% should be unhealthy
	if got := hm.calculatePodStatus(90.1, 50); got != "unhealthy" {
		t.Fatalf("expected unhealthy above 90%% CPU, got %s", got)
	}
	if got := hm.calculatePodStatus(50, 90.1); got != "unhealthy" {
		t.Fatalf("expected unhealthy above 90%% memory, got %s", got)
	}
}

func TestCalculatePodStatus_ZeroValues(t *testing.T) {
	hm := &HealthManager{}
	if got := hm.calculatePodStatus(0, 0); got != "healthy" {
		t.Fatalf("expected healthy at 0%% usage, got %s", got)
	}
}

func TestCalculatePodStatus_BothHigh(t *testing.T) {
	hm := &HealthManager{}
	// Both above 90 -> unhealthy (CPU check hits first)
	if got := hm.calculatePodStatus(95, 95); got != "unhealthy" {
		t.Fatalf("expected unhealthy when both high, got %s", got)
	}
}
