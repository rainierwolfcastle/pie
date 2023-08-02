# pie

Yet another implementation of clox from the [Crafting Interpreters](http://www.craftinginterpreters.com/) book.

## Benchmarks

### Fibonacci

Calculate the the 40th Fibonacci number using a recursive function. All times are the average over five runs and in seconds.

| C    | Python | Switch dispatch [^1] | Direct threaded dispatch      | Switch dispatch [^2] |
| ---- | ------ | -------------------- | ----------------------------- | -------------------- |
| 3.20 | 11.02  | 10.85                | 8.76                          | 8.72                 |

[^1]: Final code from the book.
[^2]: The IP variable is declared with the `register` keyword.

#### Notes

The compiler is smart enough to optimise the switch statement in the switch dispatch interpreter to make the benchmark run as fast as the direct threaded version. Or my code is bad and I've missed something. I'm surprised the compiler didn't optimise the IP variable though. Maybe it's because it's part of a struct that gets passed around and the compiler doesn't have enough context to optimise?
