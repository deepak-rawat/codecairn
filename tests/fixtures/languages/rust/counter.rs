pub struct Counter {
    value: i32,
}

impl Counter {
    pub fn new() -> Self {
        Self { value: 0 }
    }

    pub fn increment(&mut self, amount: i32) -> i32 {
        self.value += amount;
        self.value
    }
}

pub enum State {
    Ready,
    Busy,
}
