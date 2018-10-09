# Project of the course CS-453

Your goal is to implement a Software Transactional Memory (STM).

The *real* goal is of course to get you a hands-on experience with actual concurrent programming.


## What prior knowledge do I need?

Only basic C or C++ knowledge.

How to use the atomic libraries and the memory models of C11 and C++11 will be taught in the weekly project sessions.

### Useful resources

* [C/C++ reference](https://en.cppreference.com/w/)

  * [C11 atomic](https://en.cppreference.com/w/c/atomic)

  * [C++11 atomic](https://en.cppreference.com/w/cpp/atomic)

* [Preshing on Programming](http://preshing.com/archives/) - Stellar resources and facts about concurrent programming


## What is a STM?

* [This course](http://lpd.epfl.ch/site/education/ca_2018).

* The Art of Multiprocessor Programming - Chapter 18.

### Some implementations out there

* [TinySTM](http://www.tmware.org/tinystm.html)

* [LibLTX](https://sourceforge.net/projects/libltx)

* [stmmap](https://github.com/skaphan/stmmap)

You may read and inspire from existing STM libraries, but it must be **your own code** that carries out transactions.


## Grading

* *Correctness* is a must.

   The executed transactions must satisfy three properties (see below). No correctness = no passing grade for the project.

* *Throughput* is to go from a passing grade to the maximum grade.

   Comparison of your implementation to the reference one. Metric: #transaction *commits* per second.

### Evaluation machine

* SuperMicro 4 sockets server
* 4 Processors: AMD Opteron 12-core Processor 6172 at 2.1 Ghz. Total: 48 cores
* 32 GB of RAM
* Ubuntu 16.04.5 LTS (GNU/Linux 4.4.0-128-generic x86_64)
* `g++` and `gcc` versions 5.4.0 20160609
* `clang` and `clang++` versions 3.8.0-2ubuntu4

## How to write my own STM?

1. Clone/download this repository.

2. Make a local copy of/rename the `template` directory with your SCIPER number (e.g. `$ cp -r template 123456`). Be aware `make` is sensitive to spaces in the path/names.

3. Complete or completely rewrite (your copy of) the template with **your own code**.

   1. Complete/rewrite your code; only the interface should be kept identical.

   2. Compile and test locally with: `path/to/directory/grading$ make build-libs run`.

   3. Send your code to the *measurement server* for testing on the *evaluation machine*.

   4. Repeat any of the previous steps until you are satisfied with correctness and performance.

### Sending your code for measurement

1. Zip your modified copy of the `template` directory.

2. Send your code for evaluation with the `submit.py` script.
```
usage: submit.py [-h] --uuid UUID [--host HOST] [--port PORT] zippath

positional arguments:
  zippath      Path to a zip file containing your library code

optional arguments:
  -h, --help   show this help message and exit
  --uuid UUID  Secret user unique identifier
  --host HOST  Server hostname or IPv4
  --port PORT  Server TCP port
```

You can submit code as often as you want until the deadline.

Only the (correct) submission with the highest speedup will be stored persistently and considered for grading, so no need to resubmit your best implementation just before the deadline.
The TAs may still ask you to provide a copy of your best implementation though (possibly after the deadline), so do not delete anything before the end of the semester.

### List of structures and functions to implement

The student may write her/his library either in C11 (and above) or C++11 (and above).

First iteration (deadline 2018/11/23 23:59:59):

* `struct shared_t`

* `shared_t tm_create(size_t, size_t)`

* `void tm_destroy(shared_t)`

* `void* tm_start(shared_t)`

* `size_t tm_size(shared_t)`

* `size_t tm_align(shared_t)`

* `tx_t tm_begin(shared_t)`

* `bool tm_end(shared_t, tx_t)`

* `bool tm_read(shared_t, tx_t, void const*, size_t, void*)`

* `bool tm_write(shared_t, tx_t, void const*, size_t, void*)`

Second iteration, adding dynamic memory management (deadline 2018/12/20 23:59:59):

* `alloc_t tm_alloc(shared_t, tx_t, size_t, void**)`

* `bool tm_free(shared_t, tx_t, void*)`

See the interface specification below for more information.


## Interface specification

### Overview and properties

To use this *Software Transactional Memory* (STM) library, the *user* (e.g. the `grading` tool) first creates a new shared memory region.
A **shared memory region** is a non-empty set of shared memory segments.
Shared memory region creation and destruction are respectively managed by `tm_create` and `tm_destroy`.
The content of the shared memory region is *only* accessed from inside a transaction, and *solely* by the use of the functions mentioned below.

A **transaction** consists of a sequence of `tm_read`, `tm_write`, `tm_alloc`, `tm_free` operations in a shared memory region, enclosed between a call to `tm_begin` and a call to `tm_end`.
A transaction is executed on one and only one shared memory region.
A transaction either *commits* its speculative updates to the shared memory region when `tm_end` is reached, or *aborts* its execution (discarding its speculative updates) at any time (see the reference).
When a transaction is aborted, the *user* (i.e. the `grading` tool for this project) is responsible for retrying the *same* transaction (i.e. *going back* to the same `tm_begin` call site).

Transactions executed on the same shared region must satisfy three properties:

* **Atomicity**

   All speculative memory updates of a transaction are either committed or discarded as a unit.

* **Consistency**

   The memory operations of a transaction take place in (program) order.
   Transactions are committed one transaction at a time.

* **Isolation**

   No speculative memory update is visible outside of their transaction, until their transaction commits.

### Reference

Create (i.e. allocate + init) a new shared memory region, with one first, non-free-able allocated segment of the requested size and alignment.

* `shared_t tm_create(size_t size, size_t align);`

| Parameter | Description |
| :-------- | :---------- |
| `size` | Size of the first shared segment of memory to allocate (in bytes), must be a positive multiple of the alignment |
| `align` | Alignment (in bytes, must be a power of 2) that the shared memory region must support |

**Return:** Opaque shared memory region handle, `invalid_shared` on failure.

> **NB:** the requested alignment in that function will be the alignment assumed in every subsequent memory operation.

&nbsp;

Destroy (i.e. clean-up + free) a given shared memory region.

* `void tm_destroy(shared_t shared);`

| Parameter | Description |
| :-------- | :---------- |
| `shared` | Shared memory region handle to destroy |

> **NB:** no concurrent call for the same shared memory region.

> **NB:** it is guaranteed that the associated shared memory region has not been destroyed yet.

> **NB:** no transaction is running on the shared memory region when this function is called.

&nbsp;

Return the start address of the first allocated segment in the shared memory region.

* `void* tm_start(shared_t shared);`

| Parameter | Description |
| :-------- | :---------- |
| `shared` | Shared memory region to query |

**Return:** Start address of the first allocated segment

> **NB:** of course, the returned address must be aligned on the shared region alignment

> **NB:** this function never fails: it must always return the address of the first allocated segment, which is not free-able

&nbsp;

Return the size (in bytes) of the first allocated segment in the shared memory region.

* `size_t tm_size(shared_t shared);`

| Parameter | Description |
| :-------- | :---------- |
| `shared` | Shared memory region to query |

**Return:** First allocated segment size (in bytes)

> **NB:** of course, the returned size must be aligned on the shared region alignment

> **NB:** this function never fails: it must always return the size of the first allocated segment, which is not free-able

&nbsp;

Return the alignment (in bytes) of the memory accesses on given shared memory region.

* `size_t tm_align(shared_t shared);`

| Parameter | Description |
| :-------- | :---------- |
| `shared` | Shared memory region to query |

**Return:** Alignment used globally (in bytes)

&nbsp;

Begin a new transaction on the given shared memory region.

* `tx_t tm_begin(shared_t shared);`

| Parameter | Description |
| :-------- | :---------- |
| `shared` | Shared memory region to start a transaction on |

**Return:** Opaque transaction identifier

> **NB:** this function can be called concurrently

&nbsp;

End the given transaction.

* `bool tm_end(shared_t shared, tx_t tx);`

| Parameter | Description |
| :-------- | :---------- |
| `shared` | Shared memory region associated with the transaction |
| `tx` | Transaction to end |

**Return:** Whether the whole transaction committed

> **NB:** this function can be called concurrently, concurrent calls must be made with at least a different `shared` parameter or a different `tx` parameter

> **NB:** this function will not be called by the *user* (e.g. the `grading` tool) when any of `tm_read`, `tm_write`, `tm_alloc`, `tm_free` notifies that the transaction was aborted

&nbsp;

Read operation in the given transaction, source in the shared region and target in a private region.

* `bool tm_read(shared_t shared, tx_t tx, void const* source, size_t size, void* target);`

| Parameter | Description |
| :-------- | :---------- |
| `shared` | Shared memory region associated with the transaction |
| `tx` | Transaction to use |
| `source` | Source start address (in the shared region) |
| `size` | Length to copy (in bytes) |
| `target` | Target start address (in a private region) |

**Return:** Whether the whole transaction can continue

> **NB:** this function can be called concurrently, concurrent calls must be made with at least a different `shared` parameter or a different `tx` parameter

> **NB:** private buffer only writable for the duration of the call

> **NB:** the length must be a positive multiple of the shared memory region's alignment

&nbsp;

Write operation in the given transaction, source in a private region and target in the shared region.

* `bool tm_write(shared_t shared, tx_t tx, void const* source, size_t size, void* target);`

| Parameter | Description |
| :-------- | :---------- |
| `shared` | Shared memory region associated with the transaction |
| `tx` | Transaction to use |
| `source` | Source start address (in the shared region) |
| `size` | Length to copy (in bytes) |
| `target` | Transaction to use |

**Return:** Whether the whole transaction can continue

> **NB:** this function can be called concurrently, concurrent calls must be made with at least a different `shared` parameter or a different `tx` parameter

> **NB:** private buffer only readable for the duration of the call

> **NB:** the length must be a positive multiple of the shared memory region's alignment

&nbsp;

Memory allocation in the given transaction.

* `alloc_t tm_alloc(shared_t shared, tx_t tx, size_t size, void** target);`

| Parameter | Description |
| :-------- | :---------- |
| `shared` | Shared memory region associated with the transaction |
| `tx` | Transaction to use |
| `size` | Allocation requested size (in bytes) |
| `target` | Pointer in private memory receiving the address of the first byte of the newly allocated, aligned segment |

**Return:** One of: `success_alloc` (allocation was successful and transaction can continue), `abort_alloc` (transaction was aborted) and `nomem_alloc` (memory allocation failed)

> **NB:** this function can be called concurrently, concurrent calls must be made with at least a different `shared` parameter or a different `tx` parameter

> **NB:** the size must be a positive multiple of the shared memory region's alignment

> **NB:** value of `*target` is defined only if `success_alloc` was returned, and undefined otherwise

> **NB:** when `nomem_alloc` is returned, the transaction is not aborted

&nbsp;

Memory freeing in the given transaction.

* `bool tm_free(shared_t shared, tx_t tx, void* target);`

| Parameter | Description |
| :-------- | :---------- |
| `shared` | Shared memory region associated with the transaction |
| `tx` | Transaction to use |
| `target` | Address of the first byte of the previously allocated segment to deallocate |

**Return:** Whether the whole transaction can continue

> **NB:** this function can be called concurrently, concurrent calls must be made with at least a different `shared` parameter or a different `tx` parameter

> **NB:** this function must not be called with `target` as the first allocated segment (the address returned by `tm_start`)
