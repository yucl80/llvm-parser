#include <atomic>
#include <future>
#include <mutex>
#include <pthread.h>
#include <shared_mutex>
#include <thread>
#include <time.h>

int posix_write_write_race = 0;
int posix_read_write_race = 0;
int cpp_write_write_race = 0;
int mixed_api_race = 0;
int different_locks_race = 0;
volatile int volatile_race = 0;
int posix_mutex_protected = 0;
int posix_rwlock_protected = 0;
int posix_spinlock_protected = 0;
int cpp_mutex_protected = 0;
int cpp_shared_mutex_protected = 0;
int cpp_lock_guard_protected = 0;
int cpp_unique_lock_protected = 0;
int async_race = 0;
int read_only_global = 7;
thread_local int thread_local_counter = 0;
std::atomic<int> atomic_counter{0};

pthread_mutex_t posix_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t posix_mutex_a = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t posix_mutex_b = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t posix_mutex_c = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t posix_mutex_d = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t posix_timed_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_rwlock_t posix_rwlock = PTHREAD_RWLOCK_INITIALIZER;
pthread_spinlock_t posix_spinlock;
std::mutex cpp_mutex;
std::shared_mutex cpp_shared_mutex;
std::recursive_mutex cpp_recursive_mutex;
pthread_mutex_t wrapper_mutex = PTHREAD_MUTEX_INITIALIZER;
int wrapped_mutex_protected = 0;
struct SplitFields {
    int left;
    int right;
} split_fields{};
int mixed_atomic = 0;
int beginthreadex_race = 0;

extern "C" unsigned _beginthreadex(void *, unsigned,
                                    unsigned (*)(void *), void *, unsigned,
                                    unsigned *);

void *posix_write_worker_one(void *) {
    ++posix_write_write_race;
    return nullptr;
}

void *posix_write_worker_two(void *) {
    ++posix_write_write_race;
    return nullptr;
}

void *posix_store_worker(void *) {
    posix_read_write_race = 1;
    return nullptr;
}

void *posix_load_worker(void *) {
    return reinterpret_cast<void *>(
        static_cast<unsigned long>(posix_read_write_race));
}

void cpp_write_worker_one() {
    ++cpp_write_write_race;
}

void cpp_write_worker_two() {
    ++cpp_write_write_race;
}

void async_worker_one() {
    ++async_race;
}

void async_worker_two() {
    ++async_race;
}

void posix_mixed_api_worker(void *) {
    ++mixed_api_race;
}

void *posix_mixed_api_entry(void *argument) {
    posix_mixed_api_worker(argument);
    return nullptr;
}

void cpp_mixed_api_worker() {
    ++mixed_api_race;
}

void *different_lock_worker_one(void *) {
    pthread_mutex_lock(&posix_mutex_a);
    ++different_locks_race;
    pthread_mutex_unlock(&posix_mutex_a);
    return nullptr;
}

void *different_lock_worker_two(void *) {
    pthread_mutex_lock(&posix_mutex_b);
    ++different_locks_race;
    pthread_mutex_unlock(&posix_mutex_b);
    return nullptr;
}

void *volatile_worker_one(void *) {
    ++volatile_race;
    return nullptr;
}

void *volatile_worker_two(void *) {
    ++volatile_race;
    return nullptr;
}

void *posix_mutex_worker_one(void *) {
    pthread_mutex_lock(&posix_mutex);
    ++posix_mutex_protected;
    pthread_mutex_unlock(&posix_mutex);
    return nullptr;
}

void *posix_mutex_worker_two(void *) {
    pthread_mutex_lock(&posix_mutex);
    ++posix_mutex_protected;
    pthread_mutex_unlock(&posix_mutex);
    return nullptr;
}

void *posix_rwlock_worker_one(void *) {
    pthread_rwlock_wrlock(&posix_rwlock);
    ++posix_rwlock_protected;
    pthread_rwlock_unlock(&posix_rwlock);
    return nullptr;
}

void *posix_rwlock_worker_two(void *) {
    pthread_rwlock_wrlock(&posix_rwlock);
    ++posix_rwlock_protected;
    pthread_rwlock_unlock(&posix_rwlock);
    return nullptr;
}

void *posix_rwlock_reader(void *) {
    pthread_rwlock_rdlock(&posix_rwlock);
    const int value = posix_rwlock_protected;
    pthread_rwlock_unlock(&posix_rwlock);
    return reinterpret_cast<void *>(static_cast<unsigned long>(value));
}

void *posix_spinlock_worker_one(void *) {
    pthread_spin_lock(&posix_spinlock);
    ++posix_spinlock_protected;
    pthread_spin_unlock(&posix_spinlock);
    return nullptr;
}

void *posix_spinlock_worker_two(void *) {
    pthread_spin_lock(&posix_spinlock);
    ++posix_spinlock_protected;
    pthread_spin_unlock(&posix_spinlock);
    return nullptr;
}

void cpp_mutex_worker_one() {
    cpp_mutex.lock();
    ++cpp_mutex_protected;
    cpp_mutex.unlock();
}

void cpp_mutex_worker_two() {
    cpp_mutex.lock();
    ++cpp_mutex_protected;
    cpp_mutex.unlock();
}

void cpp_lock_guard_worker_one() {
    std::lock_guard<std::mutex> guard(cpp_mutex);
    ++cpp_lock_guard_protected;
}

void cpp_lock_guard_worker_two() {
    std::lock_guard<std::mutex> guard(cpp_mutex);
    ++cpp_lock_guard_protected;
}

void cpp_unique_lock_worker_one() {
    std::unique_lock<std::mutex> guard(cpp_mutex);
    ++cpp_unique_lock_protected;
}

void cpp_unique_lock_worker_two() {
    std::unique_lock<std::mutex> guard(cpp_mutex);
    ++cpp_unique_lock_protected;
}

void self_deadlock_with_lock_guards() {
    std::lock_guard<std::mutex> first(cpp_mutex);
    std::lock_guard<std::mutex> second(cpp_mutex);
}

void cpp_shared_mutex_writer() {
    cpp_shared_mutex.lock();
    ++cpp_shared_mutex_protected;
    cpp_shared_mutex.unlock();
}

void cpp_shared_mutex_reader() {
    cpp_shared_mutex.lock_shared();
    (void)cpp_shared_mutex_protected;
    cpp_shared_mutex.unlock_shared();
}

void wrapper_lock(pthread_mutex_t *mutex) {
    pthread_mutex_lock(mutex);
}

void wrapper_unlock(pthread_mutex_t *mutex) {
    pthread_mutex_unlock(mutex);
}

void cpp_wrapped_mutex_worker_one() {
    wrapper_lock(&wrapper_mutex);
    ++wrapped_mutex_protected;
    wrapper_unlock(&wrapper_mutex);
}

void cpp_wrapped_mutex_worker_two() {
    wrapper_lock(&wrapper_mutex);
    ++wrapped_mutex_protected;
    wrapper_unlock(&wrapper_mutex);
}

void cpp_recursive_self_lock() {
    cpp_recursive_mutex.lock();
    cpp_recursive_mutex.lock();
    cpp_recursive_mutex.unlock();
    cpp_recursive_mutex.unlock();
}

void split_field_left_worker() {
    ++split_fields.left;
}

void split_field_right_worker() {
    ++split_fields.right;
}

void mixed_atomic_worker() {
    __atomic_fetch_add(&mixed_atomic, 1, __ATOMIC_RELAXED);
}

void mixed_non_atomic_worker() {
    ++mixed_atomic;
}

unsigned beginthreadex_worker(void *) {
    ++beginthreadex_race;
    return 0;
}

void beginthreadex_peer_worker() {
    ++beginthreadex_race;
}

void atomic_counter_worker_one() {
    atomic_counter.fetch_add(1, std::memory_order_relaxed);
}

void atomic_counter_worker_two() {
    atomic_counter.fetch_add(1, std::memory_order_relaxed);
}

void thread_local_worker_one() {
    ++thread_local_counter;
}

void thread_local_worker_two() {
    ++thread_local_counter;
}

void read_only_worker_one() {
    (void)read_only_global;
}

void read_only_worker_two() {
    (void)read_only_global;
}

void lock_order_ab() {
    pthread_mutex_lock(&posix_mutex_a);
    pthread_mutex_lock(&posix_mutex_b);
    pthread_mutex_unlock(&posix_mutex_b);
    pthread_mutex_unlock(&posix_mutex_a);
}

void lock_order_ba() {
    pthread_mutex_lock(&posix_mutex_b);
    pthread_mutex_lock(&posix_mutex_a);
    pthread_mutex_unlock(&posix_mutex_a);
    pthread_mutex_unlock(&posix_mutex_b);
}

void consistent_lock_order_one() {
    pthread_mutex_lock(&posix_mutex_c);
    pthread_mutex_lock(&posix_mutex_d);
    pthread_mutex_unlock(&posix_mutex_d);
    pthread_mutex_unlock(&posix_mutex_c);
}

void consistent_lock_order_two() {
    pthread_mutex_lock(&posix_mutex_c);
    pthread_mutex_lock(&posix_mutex_d);
    pthread_mutex_unlock(&posix_mutex_d);
    pthread_mutex_unlock(&posix_mutex_c);
}

void balanced_timed_lock() {
    timespec deadline{};
    pthread_mutex_timedlock(&posix_timed_mutex, &deadline);
    pthread_mutex_unlock(&posix_timed_mutex);
}

void balanced_mutex_loop(int count) {
    for (int i = 0; i < count; ++i) {
        pthread_mutex_lock(&posix_mutex);
        pthread_mutex_unlock(&posix_mutex);
    }
}

void unlock_without_lock() {
    pthread_mutex_unlock(&posix_mutex_a);
}

void unbalanced_unlock(bool condition) {
    if (condition)
        pthread_mutex_lock(&posix_mutex_a);
    pthread_mutex_unlock(&posix_mutex_a);
}

void lock_not_released() {
    pthread_mutex_lock(&posix_mutex_a);
}

void self_deadlock() {
    pthread_mutex_lock(&posix_mutex_a);
    pthread_mutex_lock(&posix_mutex_a);
}

int main() {
    pthread_spin_init(&posix_spinlock, PTHREAD_PROCESS_PRIVATE);
    _beginthreadex(nullptr, 0, beginthreadex_worker, nullptr, 0, nullptr);
    pthread_t threads[15]{};
    pthread_create(&threads[0], nullptr, posix_write_worker_one, nullptr);
    pthread_create(&threads[1], nullptr, posix_write_worker_two, nullptr);
    pthread_create(&threads[2], nullptr, posix_store_worker, nullptr);
    pthread_create(&threads[3], nullptr, posix_load_worker, nullptr);
    pthread_create(&threads[4], nullptr, posix_mixed_api_entry, nullptr);
    pthread_create(&threads[5], nullptr, different_lock_worker_one, nullptr);
    pthread_create(&threads[6], nullptr, different_lock_worker_two, nullptr);
    pthread_create(&threads[7], nullptr, volatile_worker_one, nullptr);
    pthread_create(&threads[8], nullptr, volatile_worker_two, nullptr);
    pthread_create(&threads[9], nullptr, posix_mutex_worker_one, nullptr);
    pthread_create(&threads[10], nullptr, posix_mutex_worker_two, nullptr);
    pthread_create(&threads[11], nullptr, posix_rwlock_worker_one, nullptr);
    pthread_create(&threads[12], nullptr, posix_rwlock_worker_two, nullptr);
    pthread_create(&threads[13], nullptr, posix_spinlock_worker_one, nullptr);
    pthread_create(&threads[14], nullptr, posix_rwlock_reader, nullptr);

    for (pthread_t thread : threads)
        pthread_join(thread, nullptr);

    std::thread cpp_threads[] = {
        std::thread(cpp_write_worker_one),
        std::thread(cpp_write_worker_two),
        std::thread(cpp_mixed_api_worker),
        std::thread(posix_spinlock_worker_two, nullptr),
        std::thread(cpp_mutex_worker_one),
        std::thread(cpp_mutex_worker_two),
        std::thread(cpp_lock_guard_worker_one),
        std::thread(cpp_lock_guard_worker_two),
        std::thread(cpp_unique_lock_worker_one),
        std::thread(cpp_unique_lock_worker_two),
        std::thread(cpp_shared_mutex_writer),
        std::thread(cpp_shared_mutex_reader),
        std::thread(cpp_wrapped_mutex_worker_one),
        std::thread(cpp_wrapped_mutex_worker_two),
        std::thread(split_field_left_worker),
        std::thread(split_field_right_worker),
        std::thread(mixed_atomic_worker),
        std::thread(mixed_non_atomic_worker),
        std::thread(beginthreadex_peer_worker),
        std::thread(atomic_counter_worker_one),
        std::thread(atomic_counter_worker_two),
        std::thread(thread_local_worker_one),
        std::thread(thread_local_worker_two),
        std::thread(read_only_worker_one),
        std::thread(read_only_worker_two),
    };
    for (std::thread &thread : cpp_threads)
        thread.join();

    std::future<void> async_one =
        std::async(std::launch::async, async_worker_one);
    std::future<void> async_two =
        std::async(std::launch::async, async_worker_two);
    async_one.get();
    async_two.get();

    pthread_spin_destroy(&posix_spinlock);
    return 0;
}
