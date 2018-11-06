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
#include <cstddef>
#include <cstdlib>
#include <cstring>
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
namespace STM {
#include <tm.hpp>
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

// Whether to enable more safety checks
constexpr static auto assert_mode = false;

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
        EXCEPTION(TransactionAlign, Transaction, "incorrect alignment detected before transactional operation");
        EXCEPTION(TransactionReadOnly, Transaction, "tried to write/alloc/free using a read-only transaction");
        EXCEPTION(TransactionCreate, Transaction, "shared memory region creation failed");
        EXCEPTION(TransactionBegin, Transaction, "transaction begin failed");
        EXCEPTION(TransactionAlloc, Transaction, "memory allocation failed (insufficient memory)");
        EXCEPTION(TransactionRetry, Transaction, "transaction aborted and can be retried");
        EXCEPTION(TransactionNotLastSegment, Transaction, "trying to deallocate the first segment");
    EXCEPTION(Shared, Any, "operation in shared memory exception");
        EXCEPTION(SharedAlign, Shared, "address in shared memory is not properly aligned for the specified type");
        EXCEPTION(SharedOverflow, Shared, "index is past array length");
        EXCEPTION(SharedDoubleAlloc, Shared, "(probable) double allocation detected before transactional operation");
        EXCEPTION(SharedDoubleFree, Shared, "double free detected before transactional operation");
    EXCEPTION(TooSlow, Any, "non-reference module takes too long to process the transactions");

#undef EXCEPTION

}

// -------------------------------------------------------------------------- //

/** Non-copyable helper base class.
**/
class NonCopyable {
public:
    /** Deleted copy constructor/assignment.
    **/
    NonCopyable(NonCopyable const&) = delete;
    NonCopyable& operator=(NonCopyable const&) = delete;
protected:
    /** Protected default constructor, to make sure class is not directly instantiated.
    **/
    NonCopyable() = default;
};

// -------------------------------------------------------------------------- //

/** Transactional library management class.
**/
class TransactionalLibrary final: private NonCopyable {
    friend class TransactionalMemory;
private:
    /** Function types.
    **/
    using FnCreate  = decltype(&STM::tm_create);
    using FnDestroy = decltype(&STM::tm_destroy);
    using FnStart   = decltype(&STM::tm_start);
    using FnSize    = decltype(&STM::tm_size);
    using FnAlign   = decltype(&STM::tm_align);
    using FnBegin   = decltype(&STM::tm_begin);
    using FnEnd     = decltype(&STM::tm_end);
    using FnRead    = decltype(&STM::tm_read);
    using FnWrite   = decltype(&STM::tm_write);
    using FnAlloc   = decltype(&STM::tm_alloc);
    using FnFree    = decltype(&STM::tm_free);
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

/** One shared memory region management class.
**/
class TransactionalMemory final: private NonCopyable {
private:
    /** Check whether the given alignment is a power of 2
    **/
    constexpr static bool is_power_of_two(size_t align) noexcept {
        return align != 0 && (align & (align - 1)) == 0;
    }
public:
    /** Opaque shared memory region handle class.
    **/
    using Shared = STM::shared_t;
    /** Transaction class alias.
    **/
    using TX = STM::tx_t;
private:
    TransactionalLibrary const& tl; // Bound transactional library
    Shared shared;     // Handle of the shared memory region used
    void*  start_addr; // Shared memory region first segment's start address
    size_t start_size; // Shared memory region first segment's size (in bytes)
    size_t alignment;  // Shared memory region alignment (in bytes)
public:
    /** Bind constructor.
     * @param library Transactional library to use
     * @param align   Shared memory region required alignment
     * @param size    Size of the shared memory region to allocate
    **/
    TransactionalMemory(TransactionalLibrary const& library, size_t align, size_t size): tl{library}, start_size{size}, alignment{align} {
        if (unlikely(assert_mode && (!is_power_of_two(align) || size % align != 0)))
            throw Exception::TransactionAlign{};
        { // Initialize shared memory region
            shared = tl.tm_create(size, align);
            if (unlikely(shared == STM::invalid_shared))
                throw Exception::TransactionCreate{};
            start_addr = tl.tm_start(shared);
        }
    }
    /** Unbind destructor.
    **/
    ~TransactionalMemory() noexcept {
        tl.tm_destroy(shared);
    }
public:
    /** [thread-safe] Return the start address of the first shared segment.
     * @return Address of the first allocated shared region
    **/
    auto get_start() const noexcept {
        return start_addr;
    }
    /** [thread-safe] Return the size of the first shared segment.
     * @return Size in the first allocated shared region (in bytes)
    **/
    auto get_size() const noexcept {
        return start_size;
    }
    /** [thread-safe] Get the shared memory region global alignment.
     * @return Global alignment (in bytes)
    **/
    auto get_align() const noexcept {
        return alignment;
    }
public:
    /** [thread-safe] Begin a new transaction on the shared memory region.
     * @param ro Whether the transaction is read-only
     * @return Opaque transaction ID, 'STM::invalid_tx' on failure
    **/
    auto begin(bool ro) const noexcept {
        return tl.tm_begin(shared, ro);
    }
    /** [thread-safe] End the given transaction.
     * @param tx Opaque transaction ID
     * @return Whether the whole transaction is a success
    **/
    auto end(TX tx) const noexcept {
        return tl.tm_end(shared, tx);
    }
    /** [thread-safe] Read operation in the given transaction, source in the shared region and target in a private region.
     * @param tx     Transaction to use
     * @param source Source start address
     * @param size   Source/target range
     * @param target Target start address
     * @return Whether the whole transaction can continue
    **/
    auto read(TX tx, void const* source, size_t size, void* target) const noexcept {
        return tl.tm_read(shared, tx, source, size, target);
    }
    /** [thread-safe] Write operation in the given transaction, source in a private region and target in the shared region.
     * @param tx     Transaction to use
     * @param source Source start address
     * @param size   Source/target range
     * @param target Target start address
     * @return Whether the whole transaction can continue
    **/
    auto write(TX tx, void const* source, size_t size, void* target) const noexcept {
        return tl.tm_write(shared, tx, source, size, target);
    }
    /** [thread-safe] Memory allocation operation in the given transaction, throw if no memory available.
     * @param tx     Transaction to use
     * @param size   Size to allocate
     * @param target Target start address
     * @return Allocation status
    **/
    auto alloc(TX tx, size_t size, void** target) const noexcept {
        return tl.tm_alloc(shared, tx, size, target);
    }
    /** [thread-safe] Memory freeing operation in the given transaction.
     * @param tx     Transaction to use
     * @param target Target start address
     * @return Whether the whole transaction can continue
    **/
    auto free(TX tx, void* target) const noexcept {
        return tl.tm_free(shared, tx, target);
    }
};

/** One transaction over a shared memory region management class.
**/
class Transaction final: private NonCopyable {
public:
    // Just to make explicit the meaning of the associated boolean
    constexpr static auto read_write = false;
    constexpr static auto read_only  = true;
private:
    TransactionalMemory const& tm; // Bound transactional memory
    STM::tx_t tx; // Opaque transaction handle
    bool aborted; // Transaction was aborted
    bool is_ro;   // Whether the transaction is read-only (solely for assertion)
public:
    /** Deleted copy constructor/assignment.
    **/
    Transaction(Transaction const&) = delete;
    Transaction& operator=(Transaction const&) = delete;
    /** Begin constructor.
     * @param tm Transactional memory to bind
     * @param ro Whether the transaction is read-only
    **/
    Transaction(TransactionalMemory const& tm, bool ro): tm{tm}, tx{tm.begin(ro)}, aborted{false}, is_ro{ro} {
        if (unlikely(tx == STM::invalid_tx))
            throw Exception::TransactionBegin{};
    }
    /** End destructor.
    **/
    ~Transaction() {
        if (likely(!aborted))
            tm.end(tx);
    }
public:
    /** [thread-safe] Return the bound transactional memory instance.
     * @return Bound transactional memory instance
    **/
    auto const& get_tm() const noexcept {
        return tm;
    }
public:
    /** [thread-safe] Read operation in the bound transaction, source in the shared region and target in a private region.
     * @param source Source start address
     * @param size   Source/target range
     * @param target Target start address
    **/
    void read(void const* source, size_t size, void* target) {
        if (unlikely(!tm.read(tx, source, size, target))) {
            aborted = true;
            throw Exception::TransactionRetry{};
        }
    }
    /** [thread-safe] Write operation in the bound transaction, source in a private region and target in the shared region.
     * @param source Source start address
     * @param size   Source/target range
     * @param target Target start address
    **/
    void write(void const* source, size_t size, void* target) {
        if (unlikely(assert_mode && is_ro))
            throw Exception::TransactionReadOnly{};
        if (unlikely(!tm.write(tx, source, size, target))) {
            aborted = true;
            throw Exception::TransactionRetry{};
        }
    }
    /** [thread-safe] Memory allocation operation in the bound transaction, throw if no memory available.
     * @param size Size to allocate
     * @return Target start address
    **/
    void* alloc(size_t size) {
        if (unlikely(assert_mode && is_ro))
            throw Exception::TransactionReadOnly{};
        void* target;
        switch (tm.alloc(tx, size, &target)) {
        case STM::Alloc::success:
            return target;
        case STM::Alloc::nomem:
            throw Exception::TransactionAlloc{};
        default: // STM::Alloc::abort
            aborted = true;
            throw Exception::TransactionRetry{};
        }
    }
    /** [thread-safe] Memory freeing operation in the bound transaction.
     * @param target Target start address
    **/
    void free(void* target) {
        if (unlikely(assert_mode && is_ro))
            throw Exception::TransactionReadOnly{};
        if (unlikely(!tm.free(tx, target))) {
            aborted = true;
            throw Exception::TransactionRetry{};
        }
    }
};

/** Shared read/write helper class.
 * @param Type Specified type (array)
**/
template<class Type> class Shared {
protected:
    Transaction& tx; // Bound transaction
    Type* address; // Address in shared memory
public:
    /** Binding constructor.
     * @param tx      Bound transaction
     * @param address Address to bind to
    **/
    Shared(Transaction& tx, void* address): tx{tx}, address{reinterpret_cast<Type*>(address)} {
        if (unlikely(assert_mode && reinterpret_cast<uintptr_t>(address) % tx.get_tm().get_align() != 0))
            throw Exception::SharedAlign{};
        if (unlikely(assert_mode && reinterpret_cast<uintptr_t>(address) % alignof(Type) != 0))
            throw Exception::SharedAlign{};
    }
public:
    /** Get the address in shared memory.
     * @return Address in shared memory
    **/
    auto get() const noexcept {
        return address;
    }
public:
    /** Read operation.
     * @return Private copy of the content at the shared address
    **/
    Type read() const {
        Type res;
        tx.read(address, sizeof(Type), &res);
        return res;
    }
    operator Type() const {
        return read();
    }
    /** Write operation.
     * @param source Private content to write at the shared address
    **/
    void write(Type const& source) const {
        tx.write(&source, sizeof(Type), address);
    }
    void operator=(Type const& source) const {
        return write(source);
    }
public:
    /** Address of the first byte after the entry.
     * @return First byte after the entry
    **/
    void* after() const noexcept {
        return address + 1;
    }
};
template<class Type> class Shared<Type*> {
protected:
    Transaction& tx; // Bound transaction
    Type** address; // Address in shared memory
public:
    /** Binding constructor.
     * @param tx      Bound transaction
     * @param address Address to bind to
    **/
    Shared(Transaction& tx, void* address): tx{tx}, address{reinterpret_cast<Type**>(address)} {
        if (unlikely(assert_mode && reinterpret_cast<uintptr_t>(address) % tx.get_tm().get_align() != 0))
            throw Exception::SharedAlign{};
        if (unlikely(assert_mode && reinterpret_cast<uintptr_t>(address) % alignof(Type*) != 0))
            throw Exception::SharedAlign{};
    }
public:
    /** Get the address in shared memory.
     * @return Address in shared memory
    **/
    auto get() const noexcept {
        return address;
    }
public:
    /** Read operation.
     * @return Private copy of the content at the shared address
    **/
    Type* read() const {
        Type* res;
        tx.read(address, sizeof(Type*), &res);
        return res;
    }
    operator Type*() const {
        return read();
    }
    /** Write operation.
     * @param source Private content to write at the shared address
    **/
    void write(Type* source) const {
        tx.write(&source, sizeof(Type*), address);
    }
    void operator=(Type* source) const {
        return write(source);
    }
    /** Allocate and write operation.
     * @param size Size to allocate (defaults to size of the underlying class)
     * @return Private copy of the just-written content at the shared address
    **/
    Type* alloc(size_t size = 0) const {
        if (unlikely(assert_mode && read() != nullptr))
            throw Exception::SharedDoubleAlloc{};
        auto addr = tx.alloc(size > 0 ? size: sizeof(Type));
        write(reinterpret_cast<Type*>(addr));
        return reinterpret_cast<Type*>(addr);
    }
    /** Free and write operation.
    **/
    void free() const {
        if (unlikely(assert_mode && read() == nullptr))
            throw Exception::SharedDoubleFree{};
        tx.free(read());
        write(nullptr);
    }
public:
    /** Address of the first byte after the entry.
     * @return First byte after the entry
    **/
    void* after() const noexcept {
        return address + 1;
    }
};
template<class Type> class Shared<Type[]> {
protected:
    Transaction& tx; // Bound transaction
    Type* address; // Address of the first element in shared memory
public:
    /** Binding constructor.
     * @param tx      Bound transaction
     * @param address Address to bind to
    **/
    Shared(Transaction& tx, void* address): tx{tx}, address{reinterpret_cast<Type*>(address)} {
        if (unlikely(assert_mode && reinterpret_cast<uintptr_t>(address) % tx.get_tm().get_align() != 0))
            throw Exception::SharedAlign{};
        if (unlikely(assert_mode && reinterpret_cast<uintptr_t>(address) % alignof(Type) != 0))
            throw Exception::SharedAlign{};
    }
public:
    /** Get the address in shared memory.
     * @return Address in shared memory
    **/
    auto get() const noexcept {
        return address;
    }
public:
    /** Read operation.
     * @param index Index to read
     * @return Private copy of the content at the shared address
    **/
    Type read(size_t index) const {
        Type res;
        tx.read(address + index, sizeof(Type), &res);
        return res;
    }
    /** Write operation.
     * @param index  Index to write
     * @param source Private content to write at the shared address
    **/
    void write(size_t index, Type const& source) const {
        tx.write(tx, &source, sizeof(Type), address + index);
    }
public:
    /** Reference a cell.
     * @param index Cell to reference
     * @return Shared on that cell
    **/
    Shared<Type> operator[](size_t index) const {
        return Shared<Type>{tx, address + index};
    }
    /** Address of the first byte after the entry.
     * @param length Length of the array
     * @return First byte after the entry
    **/
    void* after(size_t length) const noexcept {
        return address + length;
    }
};
template<class Type, size_t n> class Shared<Type[n]> {
protected:
    Transaction& tx; // Bound transaction
    Type* address; // Address of the first element in shared memory
public:
    /** Binding constructor.
     * @param tx      Bound transaction
     * @param address Address to bind to
    **/
    Shared(Transaction& tx, void* address): tx{tx}, address{reinterpret_cast<Type*>(address)} {
        if (unlikely(assert_mode && reinterpret_cast<uintptr_t>(address) % tx.get_tm().get_align() != 0))
            throw Exception::SharedAlign{};
        if (unlikely(assert_mode && reinterpret_cast<uintptr_t>(address) % alignof(Type) != 0))
            throw Exception::SharedAlign{};
    }
public:
    /** Get the address in shared memory.
     * @return Address in shared memory
    **/
    auto get() const noexcept {
        return address;
    }
public:
    /** Read operation.
     * @param index Index to read
     * @return Private copy of the content at the shared address
    **/
    Type read(size_t index) const {
        if (unlikely(assert_mode && index >= n))
            throw Exception::SharedOverflow{};
        Type res;
        tx.read(address + index, sizeof(Type), &res);
        return res;
    }
    /** Write operation.
     * @param index  Index to write
     * @param source Private content to write at the shared address
    **/
    void write(size_t index, Type const& source) const {
        if (unlikely(assert_mode && index >= n))
            throw Exception::SharedOverflow{};
        tx.write(tx, &source, sizeof(Type), address + index);
    }
public:
    /** Reference a cell.
     * @param index Cell to reference
     * @return Shared on that cell
    **/
    Shared<Type> operator[](size_t index) const {
        if (unlikely(assert_mode && index >= n))
            throw Exception::SharedOverflow{};
        return Shared<Type>{tx, address + index};
    }
    /** Address of the first byte after the array.
     * @return First byte after the array
    **/
    void* after() const noexcept {
        return address + n;
    }
};

// -------------------------------------------------------------------------- //

/** Seed type.
**/
using Seed = ::std::uint_fast32_t;

/** Workload base class.
**/
class Workload {
protected:
    TransactionalLibrary const& tl;  // Associated transactional library
    TransactionalMemory         tm;  // Built transactional memory to use
public:
    /** Deleted copy constructor/assignment.
    **/
    Workload(Workload const&) = delete;
    Workload& operator=(Workload const&) = delete;
    /** Transactional memory constructor.
     * @param library Transactional library to use
     * @param align   Shared memory region required alignment
     * @param size    Size of the shared memory region to allocate
    **/
    Workload(TransactionalLibrary const& library, size_t align, size_t size): tl{library}, tm{tl, align, size} {}
    /** Virtual destructor.
    **/
    virtual ~Workload() {};
public:
    /** [thread-safe] Worker full run.
     * @param seed Seed to use
     * @return Whether no inconsistency has been (passively) detected
    **/
    virtual bool run(Seed) const = 0;
    /** [thread-safe] Worker full run.
     * @return Whether no inconsistency has been detected
    **/
    virtual bool check() const = 0;
};

/** Bank workload class.
**/
class Bank final: public Workload {
public:
    /** Account balance class alias.
    **/
    using Balance = intptr_t;
    static_assert(sizeof(Balance) >= sizeof(void*), "Balance class is too small");
private:
    /** Shared segment of accounts class.
    **/
    class AccountSegment final {
    private:
        /** Dummy structure for size and alignment retrieval.
        **/
        struct Dummy {
            size_t  dummy0;
            void*   dummy1;
            Balance dummy2;
            Balance dummy3[];
        };
    public:
        /** Get the segment size for a given number of accounts.
         * @param nbaccounts Number of accounts per segment
         * @return Segment size (in bytes)
        **/
        constexpr static auto size(size_t nbaccounts) noexcept {
            return sizeof(Dummy) + nbaccounts * sizeof(Balance);
        }
        /** Get the segment alignment for a given number of accounts.
         * @return Segment size (in bytes)
        **/
        constexpr static auto align() noexcept {
            return alignof(Dummy);
        }
    public:
        Shared<size_t>         count; // Number of allocated accounts in this segment
        Shared<AccountSegment*> next; // Next allocated segment
        Shared<Balance>       parity; // Segment balance correction for when deleting an account
        Shared<Balance[]>   accounts; // Amount of money on the accounts (undefined if not allocated)
    public:
        /** Deleted copy constructor/assignment.
        **/
        AccountSegment(AccountSegment const&) = delete;
        AccountSegment& operator=(AccountSegment const&) = delete;
        /** Binding constructor.
         * @param tx      Associated pending transaction
         * @param address Block base address
        **/
        AccountSegment(Transaction& tx, void* address): count{tx, address}, next{tx, count.after()}, parity{tx, next.after()}, accounts{tx, parity.after()} {}
    };
private:
    size_t  nbtxperwrk;    // Number of transactions per worker
    size_t  nbaccounts;    // Initial number of accounts and number of accounts per segment
    size_t  expnbaccounts; // Expected total number of accounts
    Balance init_balance;  // Initial account balance
    float   prob_long;     // Probability of running a long, read-only control transaction
    float   prob_alloc;    // Probability of running an allocation/deallocation transaction, knowing a long transaction won't run
public:
    /** Bank workload constructor.
     * @param library       Transactional library to use
     * @param nbtxperwrk    Number of transactions per worker
     * @param nbaccounts    Initial number of accounts and number of accounts per segment
     * @param expnbaccounts Initial number of accounts and number of accounts per segment
     * @param init_balance  Initial account balance
     * @param prob_long     Probability of running a long, read-only control transaction
     * @param prob_alloc    Probability of running an allocation/deallocation transaction, knowing a long transaction won't run
    **/
    Bank(TransactionalLibrary const& library, size_t nbtxperwrk, size_t nbaccounts, size_t expnbaccounts, Balance init_balance, float prob_long, float prob_alloc): Workload{library, AccountSegment::align(), AccountSegment::size(nbaccounts)}, nbtxperwrk{nbtxperwrk}, nbaccounts{nbaccounts}, expnbaccounts{expnbaccounts}, init_balance{init_balance}, prob_long{prob_long}, prob_alloc{prob_alloc} {
        do {
            try {
                Transaction tx{tm, Transaction::read_write};
                AccountSegment segment{tx, tm.get_start()};
                segment.count = nbaccounts;
                for (size_t i = 0; i < nbaccounts; ++i)
                    segment.accounts[i] = init_balance;
                break;
            } catch (Exception::TransactionRetry const&) {
                continue;
            }
        } while (true);
    }
private:
    /** Long read-only transaction, summing the balance of each account.
     * @param count Loosely-updated number of accounts
     * @return Whether no inconsistency has been found
    **/
    bool long_tx(size_t& nbaccounts) const {
        do {
            try {
                auto count = 0ul;
                auto sum   = Balance{0};
                auto start = tm.get_start();
                Transaction tx{tm, Transaction::read_only};
                while (start) {
                    AccountSegment segment{tx, start};
                    decltype(count) segment_count = segment.count;
                    count += segment_count;
                    sum += segment.parity;
                    for (decltype(count) i = 0; i < segment_count; ++i) {
                        Balance local = segment.accounts[i];
                        if (unlikely(local < 0))
                            return false;
                        sum += local;
                    }
                    start = segment.next;
                }
                nbaccounts = count;
                return sum == static_cast<Balance>(init_balance * count);
            } catch (Exception::TransactionRetry const&) {
                continue;
            }
        } while (true);
    }
    /** Account (de)allocation transaction, adding accounts with initial balance or removing them.
     * @param trigger Trigger level that will decide whether to allocate or deallocate
     * @return Whether no inconsistency has been found
    **/
    bool alloc_tx(size_t trigger) const {
        do {
            try {
                auto count = 0ul;
                auto start = tm.get_start();
                void* prev = nullptr;
                Transaction tx{tm, Transaction::read_write};
                while (true) {
                    AccountSegment segment{tx, start};
                    decltype(count) segment_count = segment.count;
                    count += segment_count;
                    decltype(start) segment_next = segment.next;
                    if (!segment_next) {
                        if (count > trigger && likely(count > 2)) { // Deallocate
                            --segment_count;
                            auto new_parity = segment.parity.read() + segment.accounts[segment_count] - init_balance;
                            if (segment_count > 0) { // Just "deallocate" account
                                segment.count = segment_count;
                                segment.parity = new_parity;
                            } else { // Deallocate segment
                                if (unlikely(assert_mode && prev == nullptr))
                                    throw Exception::TransactionNotLastSegment{};
                                AccountSegment prev_segment{tx, prev};
                                prev_segment.next.free();
                                prev_segment.parity = prev_segment.parity.read() + new_parity;
                            }
                        } else { // Allocate
                            if (segment_count < nbaccounts) { // Just "allocate" account
                                segment.accounts[segment_count] = init_balance;
                                segment.count = segment_count + 1;
                            } else {
                                AccountSegment next_segment{tx, segment.next.alloc(AccountSegment::size(nbaccounts))};
                                next_segment.count = 1;
                                next_segment.accounts[0] = init_balance;
                            }
                        }
                        return true;
                    }
                    prev  = start;
                    start = segment_next;
                }
            } catch (Exception::TransactionRetry const&) {
                continue;
            }
        } while (true);
    }
    /** Short read-write transaction, transferring one unit from an account to an account (potentially the same).
     * @param send_id Index of the sender account
     * @param recv_id Index of the receiver account (potentially same as source)
     * @return Whether no inconsistency has been found
    **/
    bool short_tx(size_t send_id, size_t recv_id) const {
        do {
            try {
                auto start = tm.get_start();
                Transaction tx{tm, Transaction::read_write};
                void* send_ptr = nullptr;
                void* recv_ptr = nullptr;
                // Get the account pointers in shared memory
                while (true) {
                    AccountSegment segment{tx, start};
                    size_t segment_count = segment.count;
                    if (!send_ptr) {
                        if (send_id < segment_count) {
                            send_ptr = segment.accounts[send_id].get();
                            if (recv_ptr)
                                break;
                        } else {
                            send_id -= segment_count;
                        }
                    }
                    if (!recv_ptr) {
                        if (recv_id < segment_count) {
                            recv_ptr = segment.accounts[recv_id].get();
                            if (send_ptr)
                                break;
                        } else {
                            recv_id -= segment_count;
                        }
                    }
                    start = segment.next;
                    if (!start) // Current segment is the last segment
                        return true; // At least one account does not exist => do nothing
                }
                // Transfer the money if enough fund
                Shared<Balance> sender{tx, send_ptr};
                Shared<Balance> recver{tx, recv_ptr};
                auto send_val = sender.read();
                if (send_val > 0) {
                    sender = send_val - 1;
                    recver = recver.read() + 1;
                }
                return true;
            } catch (Exception::TransactionRetry const&) {
                continue;
            }
        } while (true);
    }
public:
    virtual bool run(Seed seed) const {
        ::std::minstd_rand engine{seed};
        ::std::bernoulli_distribution long_dist{prob_long};
        ::std::bernoulli_distribution alloc_dist{prob_alloc};
        ::std::gamma_distribution<float> alloc_trigger(expnbaccounts, 1);
        size_t count = nbaccounts;
        for (size_t cntr = 0; cntr < nbtxperwrk; ++cntr) {
            if (long_dist(engine)) { // Do a long transaction
                if (unlikely(!long_tx(count)))
                    return false;
            } else if (alloc_dist(engine)) { // Do an allocation transaction
                if (unlikely(!alloc_tx(alloc_trigger(engine))))
                    return false;
            } else { // Do a short transaction
                ::std::uniform_int_distribution<size_t> account{0, count - 1};
                if (unlikely(!short_tx(account(engine), account(engine))))
                    return false;
            }
        }
        return true;
    }
    virtual bool check() const {
        size_t dummy;
        return long_tx(dummy);
    }
};

// -------------------------------------------------------------------------- //

/** Time accounting class.
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
    Chrono(Tick tick = 0) noexcept: total{tick} {}
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
    /** Get the resolution of the clock used.
     * @return Resolution (in ns), 'invalid_tick' for unknown
    **/
    static auto get_resolution() noexcept {
        return convert(::clock_getres);
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
};

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
    Sync(unsigned int nbworkers): nbworkers{nbworkers}, nbready{0}, status{Status::Done} {}
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
        decltype(::std::declval<Chrono>().get_tick()) times[nbrepeats];
        bool res = true;
        for (unsigned int i = 0; i < nbrepeats; ++i) { // Repeat measurement
            Chrono chrono;
            chrono.start();
            sync.master_notify();
            if (!sync.master_wait(maxtick)) {
                res = false;
                goto join;
            }
            chrono.stop();
            times[i] = chrono.get_tick();
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
            ::std::cout << "Usage: " << (argc > 0 ? argv[0] : "grading") << " [--dynamic] <seed> <reference library path> <tested library path>..." << ::std::endl;
            return 1;
        }
        bool dynamic; // Use dynamic memory allocation
        if (::std::strcmp(argv[1], "--dynamic") == 0) {
            dynamic = true;
            { // Pop the argument
                argv[1] = argv[0];
                --argc;
                ++argv;
            }
        } else {
            dynamic = false;
        }
        auto const nbworkers = []() {
            auto res = ::std::thread::hardware_concurrency();
            if (unlikely(res == 0))
                res = 16;
            return static_cast<size_t>(res);
        }();
        auto const nbtxperwrk    = 400000ul / nbworkers;
        auto const nbaccounts    = 32 * nbworkers;
        auto const expnbaccounts = 1024 * nbworkers;
        auto const init_balance  = 100ul;
        auto const prob_long     = dynamic ? 0.05f : 0.5f;
        auto const prob_alloc    = dynamic ? 0.2f : 0.f;
        auto const nbrepeats     = 7;
        auto const seed          = static_cast<Seed>(::std::stoul(argv[1]));
        auto const clk_res       = Chrono::get_resolution();
        auto const slow_factor   = 2ul;
        ::std::cout << "⎧ #worker threads:     " << nbworkers << ::std::endl;
        ::std::cout << "⎪ #TX per worker:      " << nbtxperwrk << ::std::endl;
        ::std::cout << "⎪ #repetitions:        " << nbrepeats << ::std::endl;
        ::std::cout << "⎪ Initial #accounts:   " << nbaccounts << ::std::endl;
        ::std::cout << "⎪ Expected #accounts:  " << expnbaccounts << ::std::endl;
        ::std::cout << "⎪ Initial balance:     " << init_balance << ::std::endl;
        ::std::cout << "⎪ Long TX probability: " << prob_long << ::std::endl;
        ::std::cout << "⎪ Allocation TX prob.: " << prob_alloc << ::std::endl;
        ::std::cout << "⎪ Slow trigger factor: " << slow_factor << ::std::endl;
        ::std::cout << "⎪ Clock resolution:    ";
        if (unlikely(clk_res == Chrono::invalid_tick)) {
            ::std::cout << "<unknown>" << ::std::endl;
        } else {
            ::std::cout << clk_res << " ns" << ::std::endl;
        }
        ::std::cout << "⎩ Seed value:          " << seed << ::std::endl;
        auto&& eval = [&](char const* path, Chrono::Tick reference) { // Library evaluation
            try {
                ::std::cout << "⎧ Evaluating '" << path << "'" << (reference == Chrono::invalid_tick ? " (reference)" : "") << "..." << ::std::endl;
                TransactionalLibrary tl{path};
                Bank bank{tl, nbtxperwrk, nbaccounts, expnbaccounts, init_balance, prob_long, prob_alloc};
                auto maxtick = [](auto reference) {
                    if (reference == Chrono::invalid_tick)
                        return Chrono::invalid_tick;
                    reference *= slow_factor;
                    if (unlikely(reference == Chrono::invalid_tick)) // Bad luck...
                        ++reference;
                    return reference;
                }(reference);
                decltype(measure(bank, nbworkers, nbrepeats, seed, maxtick)) res;
                try {
                    res = measure(bank, nbworkers, nbrepeats, seed, maxtick);
                } catch (Exception::TooSlow const& err) { // Special case since interrupting threads may lead to corrupted state
                    ::std::cerr << "⎪ *** EXCEPTION - main thread ***" << ::std::endl << "⎩ " << err.what() << ::std::endl;
                    ::std::quick_exit(2);
                }
                auto correct = ::std::get<0>(res) && bank.check();
                auto perf    = static_cast<double>(::std::get<1>(res));
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
