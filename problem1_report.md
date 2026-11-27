# Problem 1: Multi-Stage Producer–Consumer System

## 1. System Architecture
The system implements a three-stage multi-process pipeline in C on Linux using `fork()`, anonymous shared memory, and POSIX process-shared semaphores (`pshared = 1`):

- **Parent process**: Allocates and zero-initializes the shared `shared_t` region, initializes all semaphores, launches all `P` producers, `W` workers, and `C` consumers before waiting, reaps every child while checking its exit status, verifies and prints all collected results in ascending `id` order, and destroys all semaphores and the memory mapping.
- **Producers (`0 .. P-1`)**: Each producer `p` generates `K` deterministic `item_t` records with `id = p * K + j` (`0 <= j < K`), `producer_id = p`, and `value = id + 1` outside the critical section, then inserts them into Buffer A (`T = P * K` ordinary items in total).
- **Workers (`0 .. W-1`)**: Each worker removes an `item_t` from Buffer A, immediately releases Buffer A, computes `result = (long)value * value` outside both critical sections, preserves `id` and `producer_id`, attaches its `worker_id`, and inserts the resulting `result_t` into Buffer B.
- **Consumers (`0 .. C-1`)**: Each consumer removes `result_t` records from Buffer B and records non-termination results into synchronized shared result arrays for final verification by the parent.

## 2. Shared-Memory Layout
A single `shared_t` structure resides in the `MAP_SHARED | MAP_ANONYMOUS` mapping and is zero-initialized before any `fork()`:

- **Buffer A (`input_buffer_t input`)**: Circular buffer `data[N]` of `item_t`, write/read indices `in` and `out` (initially `0`), and semaphores `mutex`, `empty`, and `full`.
- **Buffer B (`output_buffer_t output`)**: Circular buffer `data[M]` of `result_t`, write/read indices `in` and `out` (initially `0`), and semaphores `mutex`, `empty`, and `full`.
- **Saved result records**: `results[T]` (indexed by `id`), `occurrences[T]` (count per `id`), and `out_of_range_count` to detect missing, duplicate, or invalid IDs.
- **Termination state**: `producers_finished` (number of producers that have inserted all `K` items) and `workers_finished` (number of workers that have finished publishing all results), both initialized to `0`.

## 3. Semaphore Design
All six semaphores are initialized with `pshared = 1` in `init_shared()` (and `sem_wait()` is wrapped with `EINTR` retry):

| Semaphore | Initial Value | Protected State / Purpose | Wait (`sem_wait`) | Signal (`sem_post`) |
| :--- | :---: | :--- | :--- | :--- |
| `input.empty` | `N` | Available free slots in Buffer A (prevents overflow) | Producer | Worker |
| `input.full` | `0` | Available filled items in Buffer A (prevents underflow) | Worker | Producer |
| `input.mutex` | `1` | Mutual exclusion for `input.data`, `input.in`, `input.out`, and `producers_finished` | Producer, Worker | Producer, Worker |
| `output.empty` | `M` | Available free slots in Buffer B (prevents overflow) | Worker | Consumer |
| `output.full` | `0` | Available filled results in Buffer B (prevents underflow) | Consumer | Worker |
| `output.mutex` | `1` | Mutual exclusion for `output.data`, `output.in`, `output.out`, `workers_finished`, `results[]`, `occurrences[]`, and `out_of_range_count` | Worker, Consumer | Worker, Consumer |

## 4. Critical Sections
To maximize concurrency, item construction (`producer`) and square computation (`worker`) are performed strictly **outside** all critical sections. Only minimal shared-state updates occur while holding a mutex:

- **Holding `input.mutex`**:
  - *Producer*: Writes `item` to `input.data[input.in]`, advances `input.in = (input.in + 1) % N`, and (after completing its `K` items) increments `producers_finished` to check whether it is the last producer. The last producer also enqueues `W` stop records (`STOP_ID = -1`) one by one under `input.mutex`.
  - *Worker*: Copies `input.data[input.out]` to a local variable and advances `input.out = (input.out + 1) % N`.
- **Holding `output.mutex`**:
  - *Worker*: Writes `result` to `output.data[output.in]` and advances `output.in = (output.in + 1) % M`. After leaving its main loop, it increments `workers_finished` to check whether it is the last worker; the last worker enqueues `C` stop records (`STOP_ID = -1`) under `output.mutex`.
  - *Consumer*: Copies `output.data[output.out]`, advances `output.out = (output.out + 1) % M`, and—if `result.id != STOP_ID`—stores `results[id]` and increments `occurrences[id]` (or increments `out_of_range_count` if out of bounds).

## 5. Correctness Invariants
- **No overflow or underflow**: Every enqueue decrements `empty` before acquiring `mutex` and increments `full` after releasing `mutex`; every dequeue does the reverse. Thus, the number of stored entries always satisfies `0 <= count_A <= N` and `0 <= count_B <= M`, and blocked processes sleep without busy waiting.
- **No data corruption (FIFO & mutual exclusion)**: Buffer A and Buffer B use independent mutexes, so stage 1 and stage 2 execute concurrently while all slot accesses and modulo wraparound updates (`% N`, `% M`) within each buffer remain atomic and strictly FIFO.
- **No loss or duplication (exactly-once transfer)**: Producers partition IDs into disjoint ranges `[p*K, (p+1)*K - 1]`. Each ordinary item is dequeued by exactly one worker, producing one result that is dequeued and recorded under `output.mutex` by exactly one consumer. After all children exit, `verify_results()` confirms that every ID in `0 .. T-1` has `occurrences[id] == 1`, `out_of_range_count == 0`, total count equals `T`, and each row's `producer_id`, `worker_id`, and `result` are valid.

## 6. Termination Protocol
Temporary buffer emptiness does not trigger termination; instead, explicit sentinel records (`STOP_ID = -1`, disjoint from all ordinary IDs `>= 0`) are used:

1. **No premature termination**: Each producer increments `producers_finished` under `input.mutex` only after inserting all `K` ordinary items. Only the last producer (`producers_finished == P`) enqueues `W` stop records into Buffer A. Because Buffer A is FIFO, all `T` ordinary items precede all `W` stop records, so every ordinary item is claimed before workers exit. Likewise, each worker increments `workers_finished` under `output.mutex` only after inserting all of its computed results into Buffer B, and only the last worker (`workers_finished == W`) enqueues `C` stop records into Buffer B. Hence all `T` ordinary results precede the `C` stop records.
2. **No indefinite blocking (deadlock freedom)**: All three process groups are forked before the parent waits. No process holds a buffer mutex while waiting on `empty` or `full`, and workers always release `input.mutex` before computing or waiting on Buffer B. Under positive capacities (`N >= 1, M >= 1`) and fair scheduling, consumers always drain Buffer B, allowing workers to drain Buffer A and allowing every stop record to pass even through single-slot buffers (`N = 1, M = 1`). Exactly `W` stops terminate all `W` workers, and `C` stops terminate all `C` consumers, after which the parent reaps every child and frees all resources.