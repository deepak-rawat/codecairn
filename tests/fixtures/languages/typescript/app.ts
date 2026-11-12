import { createState, increment } from "./counter";

export function runDemo(): number {
    const state = createState();
    return increment(state);
}

console.log(runDemo());
