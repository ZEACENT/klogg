---
title: "Combining search expressions"
date: 2021-05-26T01:46:00+03:00
anchor: "boolean_combination"
weight: 25
---

## When regular expression syntax is not enough

Regular expressions are very powerful. However, sometimes their syntax 
becomes too complex and cumbersome. This is the case when you want to 
search for “this, but not that”. Writing single regular expression for
such pattern requires some look-ahead magic. Things become a lot easier
if search pattern can be combined from several simple regular expressions
using boolean logic operators `and`, `or` and `not`. Compare 
```
(?=.*word1)(?=.*word2)(?!.*word3)
```
with 
```
word1 and word2 and not(word3)
```

Matching boolean combination of regular expressions has several steps.
Firstly, a pattern has to be split into sub-patterns and transformed into
symbolic expression that can be repeatedly evaluated. Pattern that looks
like this:
```
"pattern 1" and ("pattern 2" or "pattern 3") and not("pattern 4")
```
is translated into the following symbolic boolean expression 
```
p1 and (p2 or p3) and not(p4)
```
where `p1`, `p2`, `p3` and `p4` are variables that correspond to sub-patterns.


Then for each line of text all sub-patterns are run through regex
engine to determine which patterns match the input string.
This can be expressed as a table:
```
p1 = true
p2 = false
p3 = true
p4 = false
```

After that boolean expression is evaluated to get final verdict for the line.


First two steps were easy to implement both for Hyperscan and Qt regular expressions
engines. Hyperscan has a big advantage here as it compiles a set of regular
expressions into a single database and matches them all at once, so it does not 
take too much time to match all sub-patterns.

Although that could be a fun experiment, I decided not to write general boolean
expressions parser and evaluator from scratch. For the first prototype of the whole
idea I used [LibBoolEE](https://github.com/xstreck1/LibBoolEE). It is very easy
to integrate and use. Code looks something like this:
```
    LibBoolEE::Vals vals = { { "A", true }, { "B", false } };
    return LibBoolEE::resolve("A|B&B", vals); // returns 1
```

This was good enough for the proof of concept. However, `LibBoolEE::resolve` parses
expression each time, that causes big performance degradation. Expression is evaluated for each line from the opened file, so it has to be very fast. To achieve best
results an expression has to be parsed only once and transformed into some form that
can be repeatedly executed with different arguments.

Next thing to try was a scripting language that can be embedded into C++
application. First one was javascript engine provided by Qt library. The results
were better but still not very impressive. Then I tried LuaJit with the help of 
[sol2](https://github.com/ThePhD/sol2) and [Jinx](https://github.com/JamesBoer/Jinx)
that claimed to be suitable for realtime applications like games. Both
of them performed better than `QJsEngine` but still not great. 

After some more research I found [The Great C++ Mathematical Expression Parser Benchmark](https://github.com/ArashPartow/math-parser-benchmark-project). This is a benchmark of different implementations of open source math expression parsers and evaluators written in C++. [Exprtk](http://www.partow.net/programming/exprtk/) library seemed to be a good 
choice. And indeed for my task it performed much better than general purpose embedded
scripting engines. 

However, for some reason it felt that search speed could be improved. I enjoy making _Klogg_ 
as fast as possible, so I looked at reports of `perf` tool. First thing that
caught my attention was a lot of calls to `tolower` function. Exprtk is case-insensitive
by default and uses several maps and sets for its internal state. Switching to 
case-sensitive mode provided visible speedup. After that top functions in perf report
were `std::map` insert and lookup functions. I tried switching
from `std::map` to `std::unordered_map`. That did make things better, but still
considerable amount of time was being spent in `std::unordered_map` implementation. 
So next natural thing to try was switching to some faster hash table implementation. 
I tried [robin_hood unordered map](https://github.com/martinus/robin-hood-hashing)
and was quite impressed by it. That was the final step of performance tuning.

So now _Klogg_ has quite fast boolean expression combination mode!








