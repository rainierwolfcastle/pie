# pie

Yet another implementation of clox from the [Crafting Interpreters](http://www.craftinginterpreters.com/) book.

## How to run

Open the project file in Xcode.

## Benchmarks

### Fibonacci

Calculate the the 40th Fibonacci number using a recursive function. Times are gathered using hyperfine over five runs and are in seconds.

| Type                     | Avg (mean) | Std dev | Range (min) | Range (max) |
| ------------------------ | ---------- | ------- | ----------- | ------------|
| C                        | 0.301      | 0.0027  | 0.2969      | 0.3031      |
| Python                   | 11.044     | 0.040   | 10.999      | 11.107      |
| Ruby                     | 8.561      | 0.055   | 8.508       | 8.636       |
| Switch dispatch [^1]     | 10.880     | 0.007   | 10.870      | 10.890      |

[^1]: Final code from the book.
