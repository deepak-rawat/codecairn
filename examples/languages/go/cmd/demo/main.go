package main

import (
	"fmt"
	counter "codecairn.example/counter"
)

func main() {
	value := counter.New()
	fmt.Println(value.Increment(2))
}
