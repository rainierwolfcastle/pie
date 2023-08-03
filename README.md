# pie

Yet another implementation of clox from the [Crafting Interpreters](http://www.craftinginterpreters.com/) book.

## How to run

Open the project file in Xcode.

## Benchmarks

### Fibonacci

Calculate the the 40th Fibonacci number using a recursive function. All times are the average over five runs and in seconds.

| C    | Python | Switch dispatch [^1] | Direct threaded dispatch      |
| ---- | ------ | -------------------- | ----------------------------- |
| 3.20 | 11.02  | 10.85                | 8.71                          |

[^1]: Final code from the book.
