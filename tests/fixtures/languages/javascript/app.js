import { createCounter } from "./counter.js";

export function runDemo() {
    const counter = createCounter();
    return counter.increment(2);
}

console.log(runDemo());
