# Turbo Lox

Yet another implementation of clox from the [Crafting Interpreters](http://www.craftinginterpreters.com/) book.

## How to run

Open the project file in Xcode.

## Benchmarks

### Fibonacci

Calculate the the 40th Fibonacci number using a recursive function. Times are gathered using hyperfine over five runs and are in seconds.

| Type                                  | Avg (mean) | Std dev | Range (min) | Range (max) |
| ------------------------------------- | ---------- | ------- | ----------- | ------------|
| C                                     | 0.301      | 0.0027  | 0.2969      | 0.3031      |
| Python                                | 11.044     | 0.040   | 10.999      | 11.107      |
| Ruby                                  | 8.561      | 0.055   | 8.508       | 8.636       |
| Clox [^1]                             | 10.880     | 0.007   | 10.870      | 10.890      |
| Clox - Optimised switch dispatch [^2] | 10.746     | 0.024   | 10.725      | 10.788      |
| Clox - No NaN boxing                  | 12.048     | 0.093   | 11.904      | 12.127      |
| Clox - New instructions [^3]          | 10.104     | 0.027   | 10.074      | 10.136      |

[^1]: Final code from the book.
[^2]: https://github.com/rainierwolfcastle/pie/tree/switch-dispatch-speedups
[^3]: https://github.com/rainierwolfcastle/pie/tree/new-instructions

### Sieve

Find prime numbers up to 100,000,000. Times are gathered using hyperfine over five runs and are in seconds.

| Type                                  | Avg (mean) | Std dev | Range (min) | Range (max) |
| ------------------------------------- | ---------- | ------- | ----------- | ------------|
| C                                     | 0.8714     | 0.0025  | 0.869       | 0.8753      |
| Python                                | 11.143     | 0.091   | 11.021      | 11.233      |
| Ruby                                  | 12.037     | 0.041   | 11.855      | 12.214      |
| Clox [^1]                             | 10.940     | 0.007   | 10.895      | 11.004      |

[^1]: Final code from the book with basic array support.

