import { increment, createState } from "./counter";

export function CounterView() {
    const state = createState();
    return <button onClick={() => increment(state)}>Increment</button>;
}
