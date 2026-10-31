package counter

type Counter struct {
	Value int
}

func (c *Counter) Increment(amount int) int {
	c.Value += amount
	return c.Value
}

func New() *Counter {
	return &Counter{}
}
