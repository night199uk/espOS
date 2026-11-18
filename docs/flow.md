# The data-flow graph

`espos_flow` turns "read a pin, scale it, publish it" from thirty lines of C
into a few lines of wiring. It gives you SensESP-style nodes such as
`Poll<T>`, joined with `connect_to()` or `>>`
([Migrating from SensESP](migration-from-sensesp.md)). It is built on the C
API and does not replace it: a graph makes the same calls underneath.

It comes in two halves, and you can use either without the other:

* **The runtime** (`espos_flow.h`, plain C) — one task, a timer wheel, a
  cross-task mailbox. If all you want is "call this every 500 ms, and let me
  hand work to that task from an interrupt", stop here.
* **The graph** (`espos_flow/flow.hpp`, C++) — typed nodes wired into chains,
  with no `new`, no `std::function` and no allocation after start-up.

```cmake
idf_component_register(SRCS main.cpp PRIV_REQUIRES espos_core espos_flow)
```

A firmware that does not name it links nothing.

## The one threading rule

espOS documents a different callback context per component today — the
writer's task for a config change, the stream task for a Signal K update, the
`esp_timer` task for a health tick ([concepts.md](concepts.md), "Which task
calls you back"). Every one is a separate set of rules and a separate chance
to touch a variable from two tasks at once.

A graph replaces all of them with one rule:

> **Everything in a graph runs on the flow task. Every value that enters a
> graph from anywhere else enters through a `Mailbox`.**

So a node's transform, a `Poll`'s read function, a `Sink`'s write and every
`emit()` happen on one task, one at a time, never concurrently. **No node
needs a lock, because no node is ever re-entered.**

In exchange, nothing on that task may block. A callback that sleeps stops
every timer in the firmware. Work that must block — an I²C transaction with a
long conversion time, an HTTP request — belongs on its own task, which posts
its result back:

```c++
// On your own task, after the blocking read:
mailbox.post(value);        // returns immediately, never blocks
```

`CONFIG_ESPOS_FLOW_CHECK_TASK` (default on) turns a violation into an
immediate abort naming the node *and* the offending task, instead of a
corrupted value that surfaces as an impossible reading next season.

## The runtime, in C

```c
#include "espos_flow.h"

static void sample(void *arg) { /* runs on the flow task; must not block */ }

void app_main(void)
{
    ESP_ERROR_CHECK(espos_start(NULL));
    espos_flow_timer_t t;
    ESP_ERROR_CHECK(espos_flow_every(500, sample, NULL, &t));
    ESP_ERROR_CHECK(espos_flow_start());
}
```

| Call | What it does |
|---|---|
| `espos_flow_start()` / `_stop()` | create and delete the loop task; idempotent |
| `espos_flow_every(ms, cb, arg, &h)` | run `cb` every `ms`; no drift, missed periods skipped |
| `espos_flow_after(ms, cb, arg, &h)` | run `cb` once; `ms` 0 means "next pass" |
| `espos_flow_cancel(h)` | cancel; safe from inside the timer's own callback |
| `espos_flow_post(cb, arg)` | run `cb` on the loop, from any task; never blocks |
| `espos_flow_post_from_isr(cb, arg, &woken)` | the same from an interrupt |
| `espos_flow_run_until_idle(ms)` | do the loop's work here, for tests and before deep sleep |
| `espos_flow_now_ms()` | monotonic milliseconds since the loop's epoch |
| `espos_flow_stats(&s)` | posts, drops, timers fired, queue peak, edges used |

Two things about the clock are worth internalising. `espos_flow_now_ms()` is
**not** [`espos_time`](time.md): `espos_time` answers "what time is it" and
reads 0 until something syncs it; this answers "how long since" and is always
usable. And it is `uint32_t`, so it wraps every 49.7 days — **subtract, never
compare**:

```c
if ((uint32_t)(now - then) > 5000) { /* correct across the wrap */ }
if (now > then + 5000)             { /* wrong: fails once every 49.7 days */ }
```

Every comparison inside `espos_flow` is modular, and the wheel is host-tested
across the wrap for exactly this reason (`test/host/espos_flow_test`).

### Timing guarantees

A periodic timer's next deadline is computed from the deadline it just met,
not from when the callback finished, so a slow callback does not turn a
1000 ms timer into a 1050 ms one. If the loop was blocked long enough to miss
whole periods, the missed ones are **dropped**, not fired back to back — a
catch-up burst is never what a sensor poll wants.

### When the mailbox fills

`espos_flow_post()` never blocks. Past `CONFIG_ESPOS_FLOW_MAILBOX_DEPTH` the
post is refused with `ESP_ERR_NO_MEM`, counted in `stats.dropped`, and health
condition `flowMailbox` is raised **WARN, once** ([health.md](health.md)).

Dropping is deliberate. Blocking the producer would push the stall upstream,
possibly into an interrupt, and a full mailbox means the loop is behind or a
producer is too fast — neither is fixed by making the producer wait. If drops
appear, the fix is usually a slower producer or a faster consumer, not a
bigger queue.

## The graph, in C++

```c++
#include "espos_flow/flow.hpp"
using namespace espos::flow;

static float read_depth();                    // your sensor read
static void publish(float m);                 // espos_sk_publish_number(...)
static float linear(float v, float m, float b) { return v * m + b; }

using DepthPoll = Poll<float, float (*)()>;
using Cal       = Lambda<float, float, decltype(&linear), float, float>;
using Out       = Sink<float, void (*)(float)>;

static Graph g;

extern "C" void app_main()
{
    ESP_ERROR_CHECK(espos_start(nullptr));

    auto& depth = g.make<DepthPoll>("depth", 500, read_depth);
    auto& cal   = g.make<Cal>("cal", linear, 1.7007f, -0.165f);
    auto& out   = g.make<Out>("out", publish);

    depth >> cal >> out;      // the four lines of wiring

    depth.start();
    g.start();
}
```

That is SensESP's `analog_input` example, and the correspondence is exact:
`RepeatSensor<float>(500, …)` is `Poll`, `Linear(1.7007, -0.1650)` is the
`Lambda` with two parameters, `SKOutputFloat` is the `Sink`.

### The nodes

| Node | What it is | SensESP |
|---|---|---|
| `Value<T>` | a producer anything may `set()` | `ObservableValue` |
| `Poll<T>(id, ms, fn)` | read every `ms`, emit the result | `RepeatSensor<T>` |
| `Ticker(id, ms)` | a heartbeat with a tick count | `onRepeat` |
| `Constant<T>` | a fixed value, emitted on demand | — |
| `Lambda<In,Out,Fn,Ps...>` | arbitrary arithmetic with live parameters | `LambdaTransform` |
| `Sink<T>(id, fn)` | the end of a chain | `SKOutput*` |
| `Join<Ts...>` | the one multi-input node | `Join`/`Zip`/`connect_from` |
| `Mailbox<T,Depth>` | the way in from another task or an ISR | — |
| `Transform<In,Out>`, `Symmetric<T>` | base classes for your own nodes | same |

### Chains read left to right

`connect_to()` returns the **sink**, so a chain reads in the order the data
moves. `operator>>` is the same call:

```c++
a.connect_to(b).connect_to(c);
a >> b >> c;                     // identical
```

Fan-out serves consumers in the order they were connected.

### Conversions are free

The edge trampoline does the cast, so a `float` producer feeds an `int32_t`
consumer with no adapter node:

```c++
Value<float> v("v");
Sink<int32_t, RecordInt> out("out", RecordInt{});
v.connect_to(out);               // 3.7f arrives as 3
```

### "Sometimes there is nothing to say"

A callable returning `std::optional<Out>` emits only when engaged. This is how
you write "ignore an implausible reading" without inventing a magic value that
every downstream node has to know about:

```c++
struct Plausible {
    std::optional<float> operator()(float v, float limit) const {
        if (v > limit) return std::nullopt;    // nothing reaches the sink
        return v;
    }
};
```

`Poll` accepts the same shape, for a sensor that is still warming up or a read
that failed.

### `Join` is the only multi-input node

SensESP grew `Join`, `Zip` and `connect_from` as three answers to one
question, each with its own age and completeness rules. There is one here:

```c++
Join<float, bool> j("j", 2000, Policy::kAll);   // max age 2 s
depth.connect_to(j.in<0>());
valid.connect_to(j.in<1>());
j >> consumer;                                   // emits std::tuple<float,bool>
```

* `Policy::kAll` — emit once every slot has been fed **since the last emit**,
  and every value is younger than `max_age_ms`. One output per complete set of
  inputs. This is what stops a fresh depth being paired with a heading from a
  minute ago.
* `Policy::kAny` — emit on every input, carrying whatever the other slots last
  held, provided they are all present and still fresh. "Publish the whole
  state whenever any part of it changes."

`max_age_ms` of 0 disables the age rule, for something that genuinely changes
once a day.

### Getting values in from elsewhere

Anything outside the flow task — an interrupt, a driver callback, a task that
just finished a blocking read — enters through a `Mailbox`:

```c++
static Mailbox<int32_t, 8> pulses("pulses");    // 8-deep ring

static void IRAM_ATTR on_edge(void *)           // an ISR
{
    bool woken = false;
    pulses.post_from_isr(++count, &woken);
    if (woken) portYIELD_FROM_ISR();
}
```

The value is copied into the ring and emitted **on the flow task**, so
everything downstream obeys the threading rule without knowing where it came
from. A full ring drops and counts (`mailbox.dropped()`), for the same reason
the flow mailbox does.

### Who owns a node

Two styles, both without `new` in your code and both without SensESP's
`ConfigItem(T*)` ownership trap (SensESP #893), where a raw pointer was handed
to a registry that neither owned it nor outlived it:

```c++
// 1. The graph owns it, for the life of the firmware.
auto& p = g.make<DepthPoll>("depth", 500, read_depth);

// 2. Your struct owns it; the graph never has to.
struct Bme280 {
    Poll<float, ReadTemp> temp{"temp", 1000, ReadTemp{}};
    Sink<float, Publish>  out{"tout", Publish{}};
    explicit Bme280(Graph& g) { temp >> out; g.adopt(temp); g.adopt(out); }
};
```

Nothing in a graph is ever deleted, which is why an edge can be a plain
pointer and why neither style can dangle.

### Failures are loud, and early

Both static pools fail at **wiring time**, not silently at runtime:

* the edge pool (`CONFIG_ESPOS_FLOW_MAX_EDGES`, one edge per `connect_to()`)
* the node arena for `make<T>()` (`ESPOS_FLOW_ARENA_BYTES`)

Each aborts with a log line naming the node and the numbers. A graph that
silently lost an edge would look exactly like a sensor that stopped working,
and finding that months later costs far more than failing at boot.

The timer table (`CONFIG_ESPOS_FLOW_MAX_TIMERS`) returns `ESP_ERR_NO_MEM` with
a log line instead, because a `Poll` that could not arm is recoverable.

## Configuration

| Option | Default | What it costs |
|---|---|---|
| `ESPOS_FLOW_TASK_STACK` | 6144 | one stack, from internal RAM; a chain is call depth |
| `ESPOS_FLOW_TASK_PRIO` | 5 | above housekeeping, below the network stack |
| `ESPOS_FLOW_MAILBOX_DEPTH` | 32 | 8 bytes each |
| `ESPOS_FLOW_MAX_TIMERS` | 32 | ~24 bytes each |
| `ESPOS_FLOW_MAX_EDGES` | 96 | ~12 bytes each |
| `ESPOS_FLOW_CHECK_TASK` | y | one comparison per `emit()` |
| `ESPOS_FLOW_HEALTH` | y | the `flowMailbox` report |

## Flash

The bookkeeping that does not depend on a value type — id, title, list link,
wiring — lives in the non-template `NodeBase`, compiled once. A new value type
duplicates `emit()` and `set()` and nothing else. The common instantiations
(`float`, `double`, `int32_t`, `bool`, `std::string`, `std::optional<float>`)
are declared `extern template` and defined in one translation unit, so a
firmware links one copy of each rather than one per file. SensESP #339 is the
cautionary tale: there, template instantiation was the largest single
contributor to image size.

## Testing a graph

`espos_flow_run_until_idle()` does the loop's work on the calling task, so a
test posts, runs, and asserts, with nothing racing and nothing sleeping:

```c++
espos_flow_adopt_loop();       // this task IS the loop; refused once one runs
mailbox.post(1.5f);
espos_flow_run_until_idle(1000);
// assert on what the sink recorded
```

`espos_flow_adopt_loop()` is refused once `espos_flow_start()` has created the
real loop, so it cannot be used to subvert the one-task rule — it only names
the truth while there is no loop task. Starting the loop revokes any borrow
that is still held; after `espos_flow_start()` returns, only the flow task is
the loop. `espos_flow_run_until_idle()` drains pending work before a deep
sleep; it works only while the real loop is stopped and returns false while
the loop is running.

`test/host/espos_flow_test` runs the wheel and the graph on the linux target
under Unity ([development.md](development.md)).

## The node library

The transforms named above — `Linear`, `MovingAverage`, `Median`,
`Hysteresis`, `Debounce`, `Frequency`, `Curve` and the rest — are built on
exactly the pieces described here, and they live in
`espos_flow/transforms/`. The marine arithmetic they call is a separate,
dependency-free component, `espos_formulas`. Both are documented in
[Transforms and formulas](transforms.md); nothing in this page changes when
you use them.

Two of those headers ask for a little more than the runtime provides on its
own, and get it only when the firmware also builds the component in question:
`Param` binds a node's constant to a config namespace so the web UI can edit
it, which needs `espos_config`, and the time-stamping transforms need
`espos_time` for a wall clock. Both are optional dependencies — the headers
compile either way, and a firmware without `espos_config` simply gets a
parameter that is a plain constant.
