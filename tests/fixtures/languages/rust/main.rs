mod counter;

use counter::{Counter, State};

fn describe(state: State) -> &'static str {
    match state {
        State::Ready => "ready",
        State::Busy => "busy",
    }
}

fn main() {
    let mut counter = Counter::new();
    for state in [State::Ready, State::Busy] {
        println!("{}: {}", describe(state), counter.increment(1));
    }
}
