package counter

import "testing"

func TestIncrement(t *testing.T) {
	counter := New()
	if got := counter.Increment(2); got != 2 {
		t.Fatalf("Increment(2) = %d, want 2", got)
	}
}
