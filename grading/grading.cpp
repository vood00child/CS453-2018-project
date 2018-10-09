/**
 * @file   grading.cpp
 * @author Sébastien Rouault <sebastien.rouault@epfl.ch>
 *
 * @section LICENSE
 *
 * Copyright © 2018 Sébastien Rouault.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * any later version. Please see https://gnu.org/licenses/gpl.html
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * @section DESCRIPTION
 *
 * Grading of the implementations.
**/

// Compile-time configuration
// #define USE_MM_PAUSE

// External headers
#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <random>
#include <thread>
extern "C" {
#include <dlfcn.h>
#include <limits.h>
#include <stdlib.h>
#include <time.h>
#include <stdio.h>
#if (defined(__i386__) || defined(__x86_64__)) && defined(USE_MM_PAUSE)
    #include <xmmintrin.h>
#endif
}

// Internal headers
namespace TM {
extern "C" {
#include <tm.h>
}
}

// -------------------------------------------------------------------------- //

/** Define a proposition as likely true.
 * @param prop Proposition
**/
#undef likely
#ifdef __GNUC__
    #define likely(prop) \
        __builtin_expect((prop) ? 1 : 0, 1)
#else
    #define likely(prop) \
        (prop)
#endif

/** Define a proposition as likely false.
 * @param prop Proposition
**/
#undef unlikely
#ifdef __GNUC__
    #define unlikely(prop) \
        __builtin_expect((prop) ? 1 : 0, 0)
#else
    #define unlikely(prop) \
        (prop)
#endif

// -------------------------------------------------------------------------- //

namespace Exception {

/** Defines a simple exception.
 * @param name   Exception name
 * @param parent Parent exception (use ::std::exception as the root)
 * @param text   Explanatory string
**/
#define EXCEPTION(name, parent, text) \
    class name: public parent { \
    public: \
        /** Return the explanatory string. \
         * @return Explanatory string \
        **/ \
        virtual char const* what() const noexcept { \
            return "grading: " text; \
        } \
    }

/** Exceptions tree.
**/
EXCEPTION(Any, ::std::exception, "exception");
    EXCEPTION(Path, Any, "path exception");
        EXCEPTION(PathResolve, Path, "unable to resolve the given path");
    EXCEPTION(Module, Any, "transaction library exception");
        EXCEPTION(ModuleLoading, Module, "unable to load a transaction library");
        EXCEPTION(ModuleSymbol, Module, "symbol not found in loaded libraries");
    EXCEPTION(Transaction, Any, "transaction manager exception");
        EXCEPTION(TransactionCreate, Module, "shared memory region creation failed");
        EXCEPTION(TransactionBegin, Module, "transaction begin failed");
        EXCEPTION(TransactionAlloc, Module, "memory allocation failed (insufficient memory)");
    EXCEPTION(TooSlow, Any, "non-reference module takes too long to process the transactions");

#undef EXCEPTION

}

// -------------------------------------------------------------------------- //

/** Transactional library class.
**/
class TransactionalLibrary final {
    friend class TransactionalMemory;
private:
    /** Function types.
    **/
    using FnCreate  = decltype(&TM::tm_create);
    using FnDestroy = decltype(&TM::tm_destroy);
    using FnStart   = decltype(&TM::tm_start);
    using FnSize    = decltype(&TM::tm_size);
    using FnAlign   = decltype(&TM::tm_align);
    using FnBegin   = decltype(&TM::tm_begin);
    using FnEnd     = decltype(&TM::tm_end);
    using FnRead    = decltype(&TM::tm_read);
    using FnWrite   = decltype(&TM::tm_write);
    using FnAlloc   = decltype(&TM::tm_alloc);
    using FnFree    = decltype(&TM::tm_free);
private:
    void*     module;     // Module opaque handler
    FnCreate  tm_create;  // Module's initialization function
    FnDestroy tm_destroy; // Module's cleanup function
    FnStart   tm_start;   // Module's start address query function
    FnSize    tm_size;    // Module's size query function
    FnAlign   tm_align;   // Module's alignment query function
    FnBegin   tm_begin;   // Module's transaction begin function
    FnEnd     tm_end;     // Module's transaction end function
    FnRead    tm_read;    // Module's shared memory read function
    FnWrite   tm_write;   // Module's shared memory write function
    FnAlloc   tm_alloc;   // Module's shared memory allocation function
    FnFree    tm_free;    // Module's shared memory freeing function
private:
    /** Solve a symbol from its name, and bind it to the given function.
     * @param name Name of the symbol to resolve
     * @param func Target function to bind (optional, to use template parameter deduction)
    **/
    template<class Signature> auto solve(char const* name) const {
        auto res = ::dlsym(module, name);
        if (unlikely(!res))
            throw Exception::ModuleSymbol{};
        return *reinterpret_cast<Signature*>(&res);
    }
    template<class Signature> void solve(char const* name, Signature& func) const {
        func = solve<Signature>(name);
    }
public:
    /** Deleted copy constructor/assignment.
    **/
    TransactionalLibrary(TransactionalLibrary const&) = delete;
    TransactionalLibrary& operator=(TransactionalLibrary const&) = delete;
    /** Loader constructor.
     * @param path  Path to the library to load
    **/
    TransactionalLibrary(char const* path) {
        { // Resolve path and load module
            char resolved[PATH_MAX];
            if (unlikely(!realpath(path, resolved)))
                throw Exception::PathResolve{};
            module = ::dlopen(resolved, RTLD_NOW | RTLD_LOCAL);
            if (unlikely(!module))
                throw Exception::ModuleLoading{};
        }
        { // Bind module's 'tm_*' symbols
            solve("tm_create", tm_create);
            solve("tm_destroy", tm_destroy);
            solve("tm_start", tm_start);
            solve("tm_size", tm_size);
            solve("tm_align", tm_align);
            solve("tm_begin", tm_begin);
            solve("tm_end", tm_end);
            solve("tm_read", tm_read);
            solve("tm_write", tm_write);
            solve("tm_alloc", tm_alloc);
            solve("tm_free", tm_free);
        }
    }
    /** Unloader destructor.
    **/
    ~TransactionalLibrary() noexcept {
        ::dlclose(module); // Close loaded module
    }
};

/** Transactional memory class.
**/
class TransactionalMemory final {
public:
    /** Opaque shared memory region handle class.
    **/
    using Shared = TM::shared_t;
    /** Transaction class.
    **/
    using TX = TM::tx_t;
private:
    TransactionalLibrary const& tl; // Bound transactional library
    Shared    shared;     // Handle of the shared memory region used
    uintptr_t start_addr; // Shared memory region start address
public:
    /** Deleted copy constructor/assignment.
    **/
    TransactionalMemory(TransactionalMemory const&) = delete;
    TransactionalMemory& operator=(TransactionalMemory const&) = delete;
    /** Bind constructor.
     * @param library Transactional library to use
     * @param align   Shared memory region required alignment
     * @param size    Size of the shared memory region to allocate
    **/
    TransactionalMemory(TransactionalLibrary const& library, size_t align, size_t size): tl{library} {
        { // Initialize shared memory region
            shared = tl.tm_create(size, align);
            if (unlikely(shared == TM::invalid_shared))
                throw Exception::TransactionCreate{};
            start_addr = reinterpret_cast<uintptr_t>(tl.tm_start(shared));
        }
    }
    /** Unbind destructor.
    **/
    ~TransactionalMemory() noexcept {
        tl.tm_destroy(shared);
    }
public:
    /** Build an address in the shared region from an offset.
     * @param ptr Offset (in bytes)
     * @return Address in the shared region
    **/
    void* address(uintptr_t ptr) const noexcept {
        return reinterpret_cast<void*>(ptr + start_addr);
    }
public:
    /** [thread-safe] Begin a new transaction on the shared memory region.
     * @return Opaque transaction ID
    **/
    auto begin() {
        auto&& res = tl.tm_begin(shared);
        if (unlikely(res == TM::invalid_tx))
            throw Exception::TransactionBegin{};
        return res;
    }
    /** [thread-safe] End the given transaction.
     * @param tx Opaque transaction ID
     * @return Whether the whole transaction is a success
    **/
    auto end(TX tx) noexcept {
        return tl.tm_end(shared, tx);
    }
    /** [thread-safe] Read operation in the given transaction, source in the shared region and target in a private region.
     * @param tx     Transaction to use
     * @param source Source start address
     * @param size   Source/target range
     * @param target Target start address
     * @return Whether the whole transaction can continue
    **/
    auto read(TX tx, void const* source, size_t size, void* target) noexcept {
        return tl.tm_read(shared, tx, source, size, target);
    }
    /** [thread-safe] Write operation in the given transaction, source in a private region and target in the shared region.
     * @param tx     Transaction to use
     * @param source Source start address
     * @param size   Source/target range
     * @param target Target start address
     * @return Whether the whole transaction can continue
    **/
    auto write(TX tx, void const* source, size_t size, void* target) noexcept {
        return tl.tm_write(shared, tx, source, size, target);
    }
    /** [thread-safe] Memory allocation operation in the given transaction, throw if no memory available.
     * @param tx     Transaction to use
     * @param size   Size to allocate
     * @param target Target start address
     * @return Whether the whole transaction can continue
    **/
    auto alloc(TX tx, size_t size, void** target) {
        auto status = tl.tm_alloc(shared, tx, size, target);
        if (unlikely(status == TM::nomem_alloc))
            throw Exception::TransactionAlloc{};
        return status == TM::success_alloc;
    }
    /** [thread-safe] Memory freeing operation in the given transaction.
     * @param tx     Transaction to use
     * @param target Target start address
     * @return Whether the whole transaction can continue
    **/
    auto free(TX tx, void* target) noexcept {
        return tl.tm_free(shared, tx, target);
    }
};

// -------------------------------------------------------------------------- //

/** Seed type.
**/
using Seed = ::std::uint_fast32_t;

/** High-performance time accounting class.
**/
class Chrono final {
public:
    /** Tick class.
    **/
    using Tick = uint_fast64_t;
    constexpr static auto invalid_tick = Tick{0xbadc0de}; // Invalid tick value
private:
    Tick total; // Total tick counter
    Tick local; // Segment tick counter
public:
    /** Tick constructor.
     * @param tick Initial number of ticks (optional)
    **/
    Chrono(Tick tick = 0) noexcept: total{tick} {
    }
private:
    /** Call a "clock" function, convert the result to the Tick type.
     * @param func "Clock" function to call
     * @return Resulting time
    **/
    static Tick convert(int (*func)(::clockid_t, struct ::timespec*)) noexcept {
        struct ::timespec buf;
        if (unlikely(func(CLOCK_MONOTONIC, &buf) < 0))
            return invalid_tick;
        auto res = static_cast<Tick>(buf.tv_nsec) + static_cast<Tick>(buf.tv_sec) * static_cast<Tick>(1000000000ul);
        if (unlikely(res == invalid_tick)) // Bad luck...
            return invalid_tick + 1;
        return res;
    }
public:
    /** Start measuring a time segment.
    **/
    void start() noexcept {
        local = convert(::clock_gettime);
    }
    /** Measure a time segment.
    **/
    auto delta() noexcept {
        return convert(::clock_gettime) - local;
    }
    /** Stop measuring a time segment, and add it to the total.
    **/
    void stop() noexcept {
        total += delta();
    }
    /** Reset the total tick counter.
    **/
    void reset() noexcept {
        total = 0;
    }
    /** Get the total tick counter.
     * @return Total tick counter
    **/
    auto get_tick() const noexcept {
        return total;
    }
    /** Get the total execution time.
     * @return Total execution time (in ns)
    **/
    auto get_time() const noexcept {
        return static_cast<double>(total) / static_cast<double>(convert(::clock_getres));
    }
};

/** Workload base class.
**/
class Workload {
protected:
    TransactionalLibrary const& tl;  // Associated transactional library
    TransactionalMemory         tm;  // Built transactional memory to use
    ::std::atomic<Chrono::Tick> sum; // Sum of the tick over all the runs
public:
    /** Deleted copy constructor/assignment.
    **/
    Workload(Workload const&) = delete;
    Workload& operator=(Workload const&) = delete;
    /** Transaction library constructor.
     * @param library Transactional library to use
     * @param align   Shared memory region required alignment
     * @param size    Size of the shared memory region to allocate
    **/
    Workload(TransactionalLibrary const& library, size_t align, size_t size): tl{library}, tm{tl, align, size}, sum{0} {
    }
    /** Virtual destructor.
    **/
    virtual ~Workload() {};
protected:
    /** [thread-safe] Take into account the given local chronometer.
     * @param chrono Local worker chronometer to take into account
    **/
    void add_tick(Chrono& chrono) noexcept {
        sum.fetch_add(chrono.get_tick(), ::std::memory_order_relaxed);
        chrono.reset();
    }
public:
    /** Return then reset the number of tick.
     * @return Sum of the worker execution ticks
    **/
    auto get_tick() noexcept {
        auto&& res = sum.load(::std::memory_order_relaxed);
        sum.store(0, ::std::memory_order_relaxed);
        return res;
    }
    /** Return then reset the number of tick as time.
     * @return Sum of the worker execution times
    **/
    auto get_time() noexcept {
        auto&& res = Chrono{sum.load(::std::memory_order_relaxed)}.get_time();
        sum.store(0, ::std::memory_order_relaxed);
        return res;
    }
public:
    /** [thread-safe] Worker full run.
     * @param seed Seed to use
     * @return Whether no inconsistency has been (passively) detected
    **/
    virtual bool run(Seed) = 0;
    /** [thread-safe] Worker full run.
     * @return Whether no inconsistency has been detected
    **/
    virtual bool check() = 0;
};

/** Bank workload class.
**/
class Bank final: public Workload {
private:
    size_t nbaccounts; // Number of accounts
    size_t nbtxperwrk; // Number of transactions per worker
    int  init_balance; // Initial account balance
    float   prob_long; // Probability of running a long, read-only control transaction
public:
    /** Bank workload constructor.
     * @param library      Transactional library to use
     * @param nbaccounts   Number of accounts
     * @param nbtxperwrk   Number of transactions per worker
     * @param init_balance Initial account balance
     * @param prob_long    Probability of running a long, read-only control transaction
    **/
    Bank(TransactionalLibrary const& library, size_t nbaccounts, size_t nbtxperwrk, int init_balance, float prob_long): Workload{library, sizeof(int), sizeof(int) * nbaccounts}, nbaccounts{nbaccounts}, nbtxperwrk{nbtxperwrk}, init_balance{init_balance}, prob_long{prob_long} {
        do {
            auto tx = tm.begin();
            auto&& init_fn = [&]() {
                for (size_t i = 0; i < nbaccounts; ++i) {
                    if (unlikely(!tm.write(tx, &init_balance, sizeof(int), tm.address(i * sizeof(int)))))
                        return false;
                }
                return true;
            };
            if (unlikely(!init_fn()))
                continue;
            if (unlikely(!tm.end(tx)))
                continue;
            break;
        } while (false);
    }
private:
    /** Long transaction, summing the balance of each account.
     * @return Whether no inconsistency has been found
    **/
    bool long_check_tx() {
        do {
            auto valid = true;
            auto tx = tm.begin();
            int sum = 0;
            auto&& read_fn = [&]() {
                for (size_t i = 0; i < nbaccounts; ++i) {
                    int local;
                    if (unlikely(!tm.read(tx, tm.address(i * sizeof(int)), sizeof(int), &local)))
                        return false;
                    if (unlikely(local < 0))
                        valid = false;
                    sum += local;
                }
                return true;
            };
            if (unlikely(!read_fn()))
                continue;
            if (unlikely(!tm.end(tx)))
                continue;
            return valid && sum == init_balance * static_cast<int>(nbaccounts);
        } while (true);
    }
public:
    virtual bool run(Seed seed) {
        ::std::minstd_rand engine{seed};
        ::std::bernoulli_distribution long_dist{prob_long};
        ::std::uniform_int_distribution<size_t> account{0, nbaccounts - 1};
        Chrono chrono;
        chrono.start();
        for (size_t cntr = 0; cntr < nbtxperwrk;) {
            if (long_dist(engine)) { // Do a long transaction
                if (unlikely(!long_check_tx()))
                    return false;
            } else { // Do a short transaction
                auto tx = tm.begin();
                auto acc_a = account(engine);
                auto acc_b = account(engine); // Of course, might be same as 'acc_a'
                int solde_a, solde_b;
                if (unlikely(!tm.read(tx, tm.address(acc_a * sizeof(int)), sizeof(int), &solde_a)))
                    continue;
                if (unlikely(!tm.read(tx, tm.address(acc_b * sizeof(int)), sizeof(int), &solde_b)))
                    continue;
                if (unlikely(solde_a < 0 || solde_b < 0)) { // Inconsistency!
                    tm.end(tx);
                    return false;
                }
                if (likely(solde_a > 0)) {
                    if (acc_a != acc_b) {
                        --solde_a;
                        ++solde_b;
                    }
                    if (unlikely(!tm.write(tx, &solde_a, sizeof(int), tm.address(acc_a * sizeof(int)))))
                        continue;
                    if (unlikely(!tm.write(tx, &solde_b, sizeof(int), tm.address(acc_b * sizeof(int)))))
                        continue;
                }
                if (unlikely(!tm.end(tx)))
                    continue;
            }
            ++cntr;
        }
        chrono.stop();
        add_tick(chrono);
        return true;
    }
    virtual bool check() {
        return long_check_tx();
    }
};

// -------------------------------------------------------------------------- //

/** Pause execution.
**/
static void pause() {
#if (defined(__i386__) || defined(__x86_64__)) && defined(USE_MM_PAUSE)
    _mm_pause();
#else
    ::std::this_thread::yield();
#endif
}

/** Pause execution for a longer time.
**/
static void long_pause() {
    ::std::this_thread::sleep_for(::std::chrono::milliseconds(200));
}

/** Tailored thread synchronization class.
**/
class Sync final {
private:
    /** Synchronization status.
    **/
    enum class Status {
        Wait,  // Workers waiting each others, run as soon as all ready
        Run,   // Workers running (still full success)
        Abort, // Workers running (>0 failure)
        Done,  // Workers done (all success)
        Fail,  // Workers done (>0 failures)
        Quit   // Workers must terminate
    };
private:
    unsigned int const        nbworkers; // Number of workers to support
    ::std::atomic<unsigned int> nbready; // Number of thread having reached that state
    ::std::atomic<Status>       status;  // Current synchronization status
public:
    /** Deleted copy constructor/assignment.
    **/
    Sync(Sync const&) = delete;
    Sync& operator=(Sync const&) = delete;
    /** Worker count constructor.
     * @param nbworkers Number of workers to support
    **/
    Sync(unsigned int nbworkers): nbworkers{nbworkers}, nbready{0}, status{Status::Done} {
    }
public:
    /** Master trigger "synchronized" execution in all threads.
    **/
    void master_notify() noexcept {
        status.store(Status::Wait, ::std::memory_order_release);
    }
    /** Master trigger termination in all threads.
    **/
    void master_join() noexcept {
        status.store(Status::Quit, ::std::memory_order_release);
    }
    /** Master wait for all workers to finish.
     * @param maxtick Maximum number of ticks to wait before exiting the process on an error
     * @return Whether all workers finished on success
    **/
    bool master_wait(Chrono::Tick maxtick) {
        Chrono chrono;
        chrono.start();
        while (true) {
            switch (status.load(::std::memory_order_relaxed)) {
            case Status::Done:
                return true;
            case Status::Fail:
                return false;
            default:
                long_pause();
                if (maxtick != Chrono::invalid_tick && chrono.delta() > maxtick)
                    throw Exception::TooSlow{};
            }
        }
    }
    /** Worker wait until next run.
     * @return Whether the worker can proceed, or quit otherwise
    **/
    bool worker_wait() noexcept {
        while (true) {
            auto res = status.load(::std::memory_order_relaxed);
            if (res == Status::Wait)
                break;
            if (res == Status::Quit)
                return false;
            pause();
        }
        auto res = nbready.fetch_add(1, ::std::memory_order_relaxed);
        if (res + 1 == nbworkers) { // Latest worker, switch to run status
            nbready.store(0, ::std::memory_order_relaxed);
            status.store(Status::Run, ::std::memory_order_release);
        } else do { // Not latest worker, wait for run status
            pause();
            auto res = status.load(::std::memory_order_relaxed);
            if (res == Status::Run || res == Status::Abort)
                break;
        } while (true);
        return true;
    }
    /** Worker notify termination of its run.
     * @param success Whether its run was a success
    **/
    void worker_notify(bool success) noexcept {
        if (!success)
            status.store(Status::Abort, ::std::memory_order_relaxed);
        auto&& res = nbready.fetch_add(1, ::std::memory_order_acq_rel);
        if (res + 1 == nbworkers) { // Latest worker, switch to done/fail status
            nbready.store(0, ::std::memory_order_relaxed);
            status.store(status.load(::std::memory_order_relaxed) == Status::Abort ? Status::Fail : Status::Done, ::std::memory_order_release);
        }
    }
};

/** Measure the arithmetic mean of the execution time of the given workload with the given transaction library.
 * @param workload  Workload instance to use
 * @param nbthreads Number of concurrent threads to use
 * @param nbrepeats Number of repetitions (keep the median)
 * @param seed      Seed to use
 * @param maxtick   Maximum number of ticks to wait before deeming a time-out
 * @return Whether no inconsistency have been *passively* detected, median execution time (in ns) (undefined if inconsistency detected)
**/
static auto measure(Workload& workload, unsigned int const nbthreads, unsigned int const nbrepeats, Seed seed, Chrono::Tick maxtick) {
    ::std::thread threads[nbthreads];
    Sync sync{nbthreads}; // "As-synchronized-as-possible" starts so that threads interfere "as-much-as-possible"
    for (unsigned int i = 0; i < nbthreads; ++i) { // Start threads
        threads[i] = ::std::thread{[&](unsigned int i) {
            try {
                size_t count = 0;
                while (true) {
                    if (!sync.worker_wait())
                        return;
                    sync.worker_notify(workload.run(seed + nbthreads * count + i));
                    ++count;
                }
            } catch (::std::exception const& err) {
                sync.worker_notify(false); // Exception in workload, since sync.* cannot throw
                ::std::cerr << "⎧ *** EXCEPTION - worker thread ***" << ::std::endl << "⎩ " << err.what() << ::std::endl;
                return;
            }
        }, i};
    }
    try {
        decltype(workload.get_time()) times[nbrepeats];
        bool res = true;
        for (unsigned int i = 0; i < nbrepeats; ++i) { // Repeat measurement
            sync.master_notify();
            if (!sync.master_wait(maxtick)) {
                res = false;
                goto join;
            }
            times[i] = workload.get_time();
        }
        ::std::nth_element(times, times + (nbrepeats >> 1), times + nbrepeats); // Partial-sort times around the median
        join: {
            sync.master_join(); // Join with threads
            for (unsigned int i = 0; i < nbthreads; ++i)
                threads[i].join();
        }
        return ::std::make_tuple(res, times[nbrepeats >> 1]);
    } catch (...) {
        for (unsigned int i = 0; i < nbthreads; ++i) // Detach threads to avoid termination due to attached thread going out of scope
            threads[i].detach();
        throw;
    }
}

/** Program entry point.
 * @param argc Arguments count
 * @param argv Arguments values
 * @return Program return code
**/
int main(int argc, char** argv) {
    try {
        if (argc < 3) {
            ::std::cout << "Usage: " << (argc > 0 ? argv[0] : "grading") << " <seed> <reference library path> <tested library path>..." << ::std::endl;
            return 1;
        }
        auto const nbworkers = []() {
            auto res = ::std::thread::hardware_concurrency();
            if (unlikely(res == 0))
                res = 16;
            return static_cast<size_t>(res);
        }();
        auto const nbtxperwrk   = 1000000ul;
        auto const nbaccounts   = 4 * nbworkers;
        auto const init_balance = 100;
        auto const prob_long    = 0.5f;
        auto const nbrepeats    = 11;
        auto const seed         = static_cast<Seed>(::std::stoul(argv[1]));
        auto const slow_factor  = 2ul;
        ::std::cout << "⎧ #worker threads:     " << nbworkers << ::std::endl;
        ::std::cout << "⎪ #TX per worker:      " << nbtxperwrk << ::std::endl;
        ::std::cout << "⎪ #repetitions:        " << nbrepeats << ::std::endl;
        ::std::cout << "⎪ Initial #accounts:   " << nbaccounts << ::std::endl;
        ::std::cout << "⎪ Initial balance:     " << init_balance << ::std::endl;
        ::std::cout << "⎪ Long TX probability: " << prob_long << ::std::endl;
        ::std::cout << "⎪ Slow trigger factor: " << slow_factor << ::std::endl;
        ::std::cout << "⎩ Seed value:          " << seed << ::std::endl;
        auto&& eval = [&](char const* path, Chrono::Tick reference) { // Library evaluation
            try {
                ::std::cout << "⎧ Evaluating '" << path << "'" << (reference == Chrono::invalid_tick ? " (reference)" : "") << "..." << ::std::endl;
                TransactionalLibrary tl{path};
                Bank bank{tl, nbaccounts, nbtxperwrk, init_balance, prob_long};
                auto maxtick = [](auto reference) {
                    if (reference == Chrono::invalid_tick)
                        return Chrono::invalid_tick;
                    reference *= slow_factor;
                    if (unlikely(reference == Chrono::invalid_tick)) // Bad luck...
                        ++reference;
                    return reference;
                }(reference);
                auto res     = measure(bank, nbworkers, nbrepeats, seed, maxtick);
                auto correct = ::std::get<0>(res) && bank.check();
                auto perf    = ::std::get<1>(res);
                if (unlikely(!correct)) {
                    ::std::cout << "⎩ Inconsistency detected!" << ::std::endl;
                } else {
                    ::std::cout << "⎪ Total user execution time: " << (perf / 1000000.) << " ms";
                    if (reference != Chrono::invalid_tick)
                        ::std::cout << " -> " << (static_cast<double>(reference) / perf) << " speedup";
                    ::std::cout << ::std::endl;
                    ::std::cout << "⎩ Average TX execution time: " << (perf / static_cast<double>(nbworkers) / static_cast<double>(nbtxperwrk)) << " ns" << ::std::endl;
                }
                return ::std::make_tuple(correct, perf);
            } catch (Exception::TooSlow const& err) { // Special case since interrupting threads may lead to corrupted state
                ::std::cerr << "⎪ *** EXCEPTION - main thread ***" << ::std::endl << "⎩ " << err.what() << ::std::endl;
                ::std::exit(1);
            } catch (::std::exception const& err) {
                ::std::cerr << "⎪ *** EXCEPTION - main thread ***" << ::std::endl << "⎩ " << err.what() << ::std::endl;
                return ::std::make_tuple(false, 0.);
            }
        };
        { // Evaluations
            auto reference = eval(argv[2], Chrono::invalid_tick);
            if (unlikely(!::std::get<0>(reference)))
                return 1;
            auto perf_ref = ::std::get<1>(reference);
            for (auto i = 3; i < argc; ++i)
                eval(argv[i], perf_ref);
        }
        return 0;
    } catch (::std::exception const& err) {
        ::std::cerr << "⎧ *** EXCEPTION - main thread ***" << ::std::endl << "⎩ " << err.what() << ::std::endl;
        return 1;
    }
}
