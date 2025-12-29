# HWFQ-Scheduler

Hierarchical Weighted Fair Queueing (H-WFQ) scheduler for resource allocation and isolation in HPC storage systems.

## Overview

H-WFQ provides multi-level, work-conserving resource management for multi-tenant high-performance computing environments. It ensures resources are dynamically allocated across tenants and their processes in a fair and efficient manner while maintaining optimal system performance under heavy load.

### Key Capabilities

- **Two-level hierarchical scheduling**: System-level scheduling for tenants, tenant-level scheduling for flows
- **WF2Q+ algorithm**: Worst-Case Fair Weighted Fair Queueing with precise fairness guarantees
- **Scalability**: Supports 4,000+ tenants and millions of per-flow queues
- **Memory efficient**: < 2 GiB memory footprint for full-scale deployment
- **Work-conserving**: Unused capacity is dynamically redistributed
- **Real-time reconfiguration**: Allocation changes take effect immediately

## Architecture

The scheduler uses a calendar queue with 16 exponential groups and 2048 bins per group for O(1) amortized enqueue/dequeue operations.

## Project Structure

```
HWFQ-Scheduler/
├── include/                    # Public API headers
│   └── hwfq.h                  # Main scheduler API
├── src/                        # Implementation
│   ├── hwfq_scheduler.c        # Main hierarchical scheduler
│   ├── hwfq_config.c           # Configuration management
│   ├── hwfq_group_scheduler.c  # Single-level WFQ component
│   ├── hwfq_group_calendar.c   # Calendar queue implementation
│   ├── hwfq_flow_trie.c        # Flow ID allocation trie
│   ├── hwfq_memory.c           # Memory management
│   ├── hwfq_chunked_entries.c  # Per-owner chunked entry storage
│   └── *.h                     # Internal headers
├── tests/                      # Test suite
├── examples/                   # Usage examples
└── Makefile                    # Build configuration
```

## Modules

### Core Modules

| Module               | Source File                | Description                                                                                                                                                       |
|----------------------|----------------------------|-------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| **Main Scheduler**   | `hwfq_scheduler.c`         | Hierarchical two-level scheduler managing tenants and flows. Handles session enqueue/dequeue, in-flight tracking, timeout detection, and statistics collection.   |
| **Configuration**    | `hwfq_config.c`            | Tenant and flow lifecycle management. Validates allocations and enforces overbooking prevention for rate-based configurations.                                    |
| **Group Scheduler**  | `hwfq_group_scheduler.c`   | Reusable single-level WFQ component implementing WF2Q+ algorithm. Used at both system level (for tenants) and tenant level (for flows).                           |
| **Calendar Queue**   | `hwfq_group_calendar.c`    | Efficient scheduling data structure with 32,768 bins organized into 16 exponential groups. Provides O(1) amortized operations using bitfield-based bin selection. |
| **Flow Trie**        | `hwfq_flow_trie.c`         | 4-level hierarchical bitmap for O(1) amortized flow ID allocation. Supports up to 64M flow IDs with cache-friendly allocation patterns.                           |
| **Memory**           | `hwfq_memory.c`            | Memory management utilities. Supports custom allocators and handles internal allocations.                                                                         |
| **Chunked Entries**  | `hwfq_chunked_entries.c`   | Per-owner chunked entry storage with O(1) indexing. Allocates on demand without lock contention between owners.                                                   |

### Public APIs

**`include/hwfq.h`** - Main scheduler interface:
- Scheduler initialization and lifecycle
- Tenant configuration (add, configure, remove)
- Flow management (add, reconfigure, remove)
- Session scheduling (enqueue, dequeue, complete, cancel)
- Capacity management and statistics

## Building

```bash
# Build library and tests (debug mode)
make

# Build in release mode
make MODE=release all

# Clean build artifacts
make clean
```

The build produces:
- `build/lib/libhwfq.a` - Static library
- `build/bin/test_*` - Test executables

## Test Suite

### Running Tests

```bash
# Build and run all tests
make test

# Run individual test
./build/bin/test_fairness
```

### Test Files

| Test                                   | Description                                                                                                            |
|----------------------------------------|------------------------------------------------------------------------------------------------------------------------|
| `test_calendar.c`                      | Calendar queue testing. Validates bin selection, group transitions, and time-based operations.                          |
| `test_chunked_entries.c`               | Chunked entry storage testing. Validates per-owner allocation, indexing, and iteration.                                 |
| `test_config.c`                        | Configuration and lifecycle validation. Tests tenant/flow add/remove, parameter validation, and overbooking prevention.|
| `test_flow_trie.c`                     | Flow trie testing. Validates ID allocation, deallocation, and bitmap operations.                                        |
| `test_group_scheduler.c`               | Single-level WFQ component testing. Validates enqueue/dequeue ordering, rate/weight allocations, and virtual time.     |
| `test_fairness.c`                      | Fairness guarantees with geometric weight distributions (512:256:128:...:1). Validates proportional allocation.        |
| `test_hierarchical.c`                  | Two-level tenant/flow scheduling. Tests hierarchical fairness, capacity isolation, and multi-tenant scenarios.         |
| `test_statistics.c`                    | Statistics tracking API. Validates counter updates, wait time tracking, and per-tenant/per-flow metrics.               |
| `test_performance.c`                   | Throughput benchmarks. Measures enqueue/dequeue ops/sec and latency distribution.                                      |
| `test_stress.c`                        | High-volume stress testing. Validates stability under rapid tenant churn and weight reconfiguration.                   |
| `test_concurrency.c`                   | Multi-threaded thread safety. Tests concurrent producer/consumer operations with 4+ threads.                           |
| `test_edge_cases.c`                    | Boundary condition testing. Validates maximum tenants/flows limits and rapid queue state transitions.                  |
| `test_scale.c`                         | Large-scale validation. Tests 4,000 tenants and 100,000 flows while verifying < 2 GiB memory usage.                    |
| `test_fairness_extended.c`             | Long-running fairness validation (100,000+ cycles). Tests extreme weight ratios and temporal fairness.                 |
| `test_skew_progressive.c`              | Skewed workload testing with progressive removal of high-weight entries during continuous operation.                   |
| `test_data_export.c`                   | CSV export and validation. Generates data for external analysis and validates allocation accuracy.                     |
| `test_group_scheduler_concurrency.c`   | Group scheduler thread safety. Tests internal lock contention with 8 concurrent threads.                               |

## Key Design Features

### WF2Q+ Algorithm
The scheduler implements Worst-Case Fair Weighted Fair Queueing Plus (WF2Q+) which provides:
- Precise proportional allocation based on weights
- Bounded delay guarantees
- Work-conserving behavior (unused capacity is redistributed)

Virtual time calculation:
```
V_WF2Q+(t + τ) = max(V_WF2Q+(t) + τ, min{S_i})
```

### Calendar Queue Structure
- 16 exponential groups spanning 32,000x range of service intervals
- 2048 bins per group for finish time precision
- Two-level bitfield for O(1) empty bin detection
- Doubly-linked lists within bins sorted by finish time

### Memory Management
- Pre-allocated pools prevent runtime fragmentation
- Session pool: ~90 MB per 1M sessions
- Chunked entries: Allocated on demand per owner
- Trie overhead: ~130 KB

### Allocation Modes
- **Rate-based**: Absolute capacity guarantees (work units/sec). Overbooking prevented.
- **Weight-based**: Proportional shares. No overbooking possible.

## Resource Allocation

Tenants and flows can be configured with either:

1. **Rate allocation**: Guaranteed throughput (e.g., 1000 work units/sec)
   - System enforces `sum(rates) <= total_capacity`
   - Returns `HWFQ_ERR_OVERBOOKED` if exceeded

2. **Weight allocation**: Proportional share (e.g., weight=100)
   - Effective rate = `weight / sum(weights) * available_capacity`
   - Dynamic adjustment as tenants become active/idle

## Error Codes

| Code | Name                           | Description                                |
|------|--------------------------------|--------------------------------------------|
| 0    | `HWFQ_SUCCESS`                 | Operation completed successfully           |
| -1   | `HWFQ_ERR_INVALID_ARG`         | Invalid argument provided                  |
| -2   | `HWFQ_ERR_NO_MEMORY`           | Memory allocation failed                   |
| -3   | `HWFQ_ERR_NOT_FOUND`           | Tenant or flow not found                   |
| -4   | `HWFQ_ERR_ALREADY_EXIST`       | Tenant or flow already exists              |
| -5   | `HWFQ_ERR_OVERBOOKED`          | Rate allocation would exceed capacity      |
| -6   | `HWFQ_ERR_NO_WORK`             | No sessions available for scheduling       |
| -7   | `HWFQ_ERR_TENANT_HAS_BACKLOG`  | Cannot remove tenant with pending sessions |
| -99  | `HWFQ_ERR_INTERNAL`            | Internal error                             |
