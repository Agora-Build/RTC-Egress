package utils

import (
	"os"
	"path/filepath"
	"testing"
)

func TestResolveConfigFile_ConfigFileEnv(t *testing.T) {
	// Create a temp config file
	tmpDir := t.TempDir()
	cfgPath := filepath.Join(tmpDir, "test.yaml")
	if err := os.WriteFile(cfgPath, []byte("key: value"), 0644); err != nil {
		t.Fatal(err)
	}

	t.Setenv("CONFIG_FILE", cfgPath)
	t.Setenv("CONFIG_DIR", "")

	got, err := ResolveConfigFile("test.yaml", nil)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if got != cfgPath {
		t.Fatalf("expected %q, got %q", cfgPath, got)
	}
}

func TestResolveConfigFile_ConfigFlag(t *testing.T) {
	tmpDir := t.TempDir()
	cfgPath := filepath.Join(tmpDir, "test.yaml")
	if err := os.WriteFile(cfgPath, []byte("key: value"), 0644); err != nil {
		t.Fatal(err)
	}

	t.Setenv("CONFIG_FILE", "")
	t.Setenv("CONFIG_DIR", "")

	args := []string{"--config", cfgPath}
	got, err := ResolveConfigFile("test.yaml", args)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if got != cfgPath {
		t.Fatalf("expected %q, got %q", cfgPath, got)
	}
}

func TestResolveConfigFile_ConfigDir(t *testing.T) {
	tmpDir := t.TempDir()
	cfgPath := filepath.Join(tmpDir, "my_config.yaml")
	if err := os.WriteFile(cfgPath, []byte("key: value"), 0644); err != nil {
		t.Fatal(err)
	}

	t.Setenv("CONFIG_FILE", "")
	t.Setenv("CONFIG_DIR", tmpDir)

	got, err := ResolveConfigFile("my_config.yaml", nil)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if got != cfgPath {
		t.Fatalf("expected %q, got %q", cfgPath, got)
	}
}

func TestResolveConfigFile_NotFound(t *testing.T) {
	t.Setenv("CONFIG_FILE", "")
	t.Setenv("CONFIG_DIR", "")

	_, err := ResolveConfigFile("nonexistent_config_xyz.yaml", nil)
	if err == nil {
		t.Fatal("expected error when config not found")
	}
}

func TestResolveConfigFile_PrecedenceEnvOverFlag(t *testing.T) {
	tmpDir := t.TempDir()
	envPath := filepath.Join(tmpDir, "env.yaml")
	flagPath := filepath.Join(tmpDir, "flag.yaml")
	if err := os.WriteFile(envPath, []byte("env"), 0644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(flagPath, []byte("flag"), 0644); err != nil {
		t.Fatal(err)
	}

	t.Setenv("CONFIG_FILE", envPath)
	t.Setenv("CONFIG_DIR", "")

	got, err := ResolveConfigFile("test.yaml", []string{"--config", flagPath})
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	// CONFIG_FILE env should take precedence over --config flag
	if got != envPath {
		t.Fatalf("expected CONFIG_FILE to take precedence, got %q", got)
	}
}

func TestResolveConfigFile_ConfigFileEnvDirectory(t *testing.T) {
	// CONFIG_FILE pointing to a directory should be ignored
	tmpDir := t.TempDir()
	t.Setenv("CONFIG_FILE", tmpDir)
	t.Setenv("CONFIG_DIR", "")

	_, err := ResolveConfigFile("test.yaml", nil)
	if err == nil {
		t.Fatal("expected error when CONFIG_FILE points to a directory")
	}
}

func TestResolveConfigFile_ConfigFlagEmptyValue(t *testing.T) {
	t.Setenv("CONFIG_FILE", "")
	t.Setenv("CONFIG_DIR", "")

	// --config with empty value should be skipped
	_, err := ResolveConfigFile("test.yaml", []string{"--config", ""})
	if err == nil {
		t.Fatal("expected error when --config has empty value")
	}
}

func TestResolveConfigFile_ConfigFlagAtEnd(t *testing.T) {
	t.Setenv("CONFIG_FILE", "")
	t.Setenv("CONFIG_DIR", "")

	// --config at end of args with no following value
	_, err := ResolveConfigFile("test.yaml", []string{"--config"})
	if err == nil {
		t.Fatal("expected error when --config has no value")
	}
}
