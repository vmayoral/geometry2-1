#ifndef ALT_TF_RWLOCK_H
#define ALT_TF_RWLOCK_H

/**
 * Thanks to ccbench!
 */

#include <xmmintrin.h>
#include <atomic>
#include <tbb/concurrent_vector.h>

class RWLock {
public:
  alignas(64) std::atomic<int> counter;
  // counter == -1, write locked;
  // counter == 0, not locked;
  // counter > 0, there are $counter readers who acquires read-lock.

  RWLock(const RWLock &other) = delete;
  RWLock(RWLock &&other) = delete;
  RWLock() { counter.store(0, std::memory_order_release); }

  void init() { counter.store(0, std::memory_order_release); }

  // Read lock
  void r_lock() {
    int expected, desired;
    expected = counter.load(std::memory_order_acquire);
    for (;;) {
      if (expected != -1)
        desired = expected + 1;
      else {
        expected = counter.load(std::memory_order_acquire);
        continue;
      }

      if (counter.compare_exchange_strong(
        expected, desired,
        std::memory_order_acq_rel, std::memory_order_acquire))
        return;
    }
  }

  bool r_trylock() {
    int expected, desired;
    expected = counter.load(std::memory_order_acquire);
    for (;;) {
      if (expected != -1)
        desired = expected + 1;
      else
        return false;

      if (counter.compare_exchange_strong(
        expected, desired,
        std::memory_order_acq_rel, std::memory_order_acquire))
        return true;
    }
  }

  void r_unlock() {
//    assert(isLocked());
    counter--;
  }

  void w_lock() {
    int expected, desired(-1);
    expected = counter.load(std::memory_order_acquire);
    for (;;) {
      if (expected != 0) {
        expected = counter.load(std::memory_order_acquire);
        continue;
      }
      if (counter.compare_exchange_strong(
        expected, desired,
        std::memory_order_acq_rel, std::memory_order_acquire))
        return;
    }
  }

  bool w_trylock() {
    int expected, desired(-1);
    expected = counter.load(std::memory_order_acquire);
    for (;;) {
      if (expected != 0) return false;

      if (counter.compare_exchange_strong(
        expected, desired, std::memory_order_acq_rel, std::memory_order_acquire))
        return true;
    }
  }

  void w_unlock() { counter++; }

  // Upgrade, read -> write
  void upgrade() {
    int expected, desired(-1);
    expected = counter.load(std::memory_order_acquire);
    for (;;) {
      if (expected != 1) { // only me is reading.
        expected = counter.load(std::memory_order_acquire);
        continue;
      }

      if (counter.compare_exchange_strong(
        expected, desired,
        std::memory_order_acquire, std::memory_order_acquire))
        return;
    }
  }

  bool tryupgrade() {
    int expected, desired(-1);
    expected = counter.load(std::memory_order_acquire);
    for (;;) {
      if (expected != 1) return false;

      if (counter.compare_exchange_strong(expected, desired,
                                          std::memory_order_acq_rel))
        return true;
    }
  }

  inline bool isLocked() const{
    return counter.load(std::memory_order_acquire) != 0;
  }
};

typedef std::shared_ptr<RWLock> RWLockPtr;

class ScopedSetUnLocker{
public:
  virtual void wLockIfNot(uint32_t id) = 0;
  virtual void rLockIfNot(uint32_t id) = 0;
  virtual ~ScopedSetUnLocker(){};
};

class DummySetUnLocker: public ScopedSetUnLocker{
public:
  void wLockIfNot(uint32_t id) override{};
  void rLockIfNot(uint32_t id) override{};
  ~DummySetUnLocker() override{};
};

constexpr size_t XACT_TF_MAX_NODE_SIZE = 300;

class ScopedWriteSetUnLocker : public ScopedSetUnLocker{
public:
  explicit ScopedWriteSetUnLocker(std::array<RWLock, XACT_TF_MAX_NODE_SIZE> &mutexes_)
    : mutexes(mutexes_){}

  void wLockIfNot(uint32_t id) override{
    // if not write locked, then add to write lock set.
    // upgrade is not allowed!
    if(writeLockedIdSet.find(id) == writeLockedIdSet.end()){
      if(readLockedIdSet.find(id) != readLockedIdSet.end()){
        // upgrade is not implemented!
        assert(false);
      }
      mutexes[id].w_lock();
      writeLockedIdSet.insert(id);
    }
  }

  bool tryWLockIfNot(uint32_t id){
    if(writeLockedIdSet.find(id) == writeLockedIdSet.end()){
      if(readLockedIdSet.find(id) != readLockedIdSet.end()){
        // upgrade is not implemented!
        assert(false);
      }
      bool result = mutexes[id].w_trylock();
      if(!result){
        return false;
      }
      writeLockedIdSet.insert(id);
    }
    // already locked, so success.
    return true;
  }

  void rLockIfNot(uint32_t id) override{
    // if not write locked, then add to read lock set.
    if(writeLockedIdSet.find(id) == writeLockedIdSet.end()){
      if(readLockedIdSet.find(id) == readLockedIdSet.end()){
        mutexes[id].r_lock();
        readLockedIdSet.insert(id);
      }
    }
  }

  void unlockAll(){
    for(auto id : writeLockedIdSet){
      mutexes[id].w_unlock();
    }
    for(auto id : readLockedIdSet){
      mutexes[id].r_unlock();
    }
    writeLockedIdSet.clear();
    readLockedIdSet.clear();
  }

  size_t wLockedSize() const{
    return writeLockedIdSet.size();
  }

  ~ScopedWriteSetUnLocker() override{
    unlockAll();
  }
private:
  std::set<uint32_t> writeLockedIdSet{};
  std::set<uint32_t> readLockedIdSet{};
  std::array<RWLock, XACT_TF_MAX_NODE_SIZE> &mutexes;
};

class ReadUnLocker{
public:
  explicit ReadUnLocker(RWLock &lock_)
  : lock(lock_){}

  inline void rLock(){
    lock.r_lock();
  }

  inline bool tryRLock(){
    return lock.r_trylock();
  }

  ~ReadUnLocker(){
    lock.r_unlock();
  }

private:
  RWLock &lock;
};

class WriteUnLocker {
public:
  explicit WriteUnLocker(RWLock &lock_)
    : lock(lock_) {}

  void wLock() {
    lock.w_lock();
  }

  ~WriteUnLocker() {
    lock.w_unlock();
  }

private:
  RWLock &lock;
};

class UpdateUnLocker{
public:
  explicit UpdateUnLocker(
    RWLock &lock,
    bool &lock_upgraded)
    : lock(lock), lock_upgraded(lock_upgraded){}

  ~UpdateUnLocker(){
    assert(lock.isLocked());
    if(lock_upgraded){
      lock.w_unlock();
    }else{
      lock.r_unlock();
    }
  }

private:
  RWLock &lock;
  bool &lock_upgraded;
};

#endif //ALT_TF_RWLOCK_H
