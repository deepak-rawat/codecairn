export interface CounterState {
    value: number;
}

export function increment(state: CounterState): number {
    return ++state.value;
}

export const createState = (): CounterState => ({ value: 0 });
