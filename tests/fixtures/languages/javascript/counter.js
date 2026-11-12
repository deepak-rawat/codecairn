export class Counter {
    value = 0;
    increment(amount = 1) {
        this.value += amount;
        return this.value;
    }
}

export const createCounter = () => new Counter();
