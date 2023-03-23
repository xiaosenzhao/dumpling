#include <algorithm>
#include <iostream>
#include <thread>
#include <vector>

template <typename T>
class Singleton {
 public:
  static T* GetInstance() {
    static T instance;
    return &instance;
  }

  Singleton() {}
  ~Singleton() {}
  Singleton(const Singleton&) = delete;
  Singleton& operator=(const Singleton&) = delete;
};

template <typename T>
struct ObjectPoolBlockMaxSize {
  static const size_t value = 64 * 1024;
};

template <typename T>
struct ObjectPoolBlockMaxItem {
  static const size_t value = 256;
};

template <typename T>
struct ObjectPoolFreeChunkMaxItem {
  static size_t value() { return 256; }
};

template <typename T>
struct ObjectPoolValidator {
  static bool Validate(const T*) { return true; }
};

template <typename T, size_t NITEM>
struct ObjectPoolFreeChunk {
  size_t free_num;
  T* ptrs[NITEM];
};

struct ObjectPoolInfo {
  size_t local_pool_num;
  size_t block_group_num;
  size_t block_num;
  size_t item_num;
  size_t block_item_num;
  size_t free_chunk_item_num;
  size_t total_size;
  size_t free_item_num;
};

static const size_t kOPMaxBlockNGroup = 65536;
static const size_t kOPGroupNBlockNBit = 16;
static const size_t kOPGroupNBlock = (1UL << kOPGroupNBlockNBit);
static const size_t kOPInitialFreeListSize = 1024;

template <typename T>
class ObjectPoolBlockItemNum {
  static const size_t N1 = ObjectPoolBlockMaxSize<T>::value / sizeof(T);
  static const size_t N2 = (N1 < 1 ? 1 : N1);

 public:
  static const size_t value = (N2 > ObjectPoolBlockMaxItem<T>::value ? ObjectPoolBlockMaxItem<T>::value : N2);
};

template <typename T>
class ObjectPool : public Singleton<ObjectPool<T>> {
 public:
  static const size_t kBlockNItem = ObjectPoolBlockItemNum<T>::value;
  static const size_t kFreeChunkNItem = kBlockNItem;

  typedef ObjectPoolFreeChunk<T, kFreeChunkNItem> FreeChunk;
  typedef ObjectPoolFreeChunk<T, 0> DynamicFreeChunk;

  struct Block {
    char items[sizeof(T) * kBlockNItem];
    size_t nitem;
    Block() : nitem(0) {}
  };

  struct BlockGroup {
    std::atomic<size_t> nblock;
    std::atomic<Block*> blocks[kOPGroupNBlock];

    BlockGroup() : nblock(0) { memset(static_cast<void*>(blocks), 0, sizeof(std::atomic<Block*>) * kOPGroupNBlock); }
  };

  // 每个线程都有一个池子
  class LocalPool {
   public:
    explicit LocalPool(ObjectPool* pool) : pool_(pool), cur_block_(nullptr), cur_block_index_(0) {
      cur_free_.nfree = 0;
    }

    ~LocalPool() {
      if (cur_free_.nfree != 0) {
        pool_->PushFreeChunk(cur_free_);
      }
      pool_->ClearFromDestructorOfLocalPool();
    }

    static void DeleteLocalPool(void* arg) { delete static_cast<LocalPool*>(arg); }

    template <typename... Targs>
    T* get(Targs&&... args) {
      if (cur_free_.free_num > 0) {
        global_free_num_.fetch_sub(1, std::memory_order_relaxed);
        return cur_free_.ptrs[--cur_free_.free_num];
      }
      // 从全局 global 获取一个 FreeChunk
      if (pool_->PopFreeChunk(cur_free_)) {
        global_free_num_.fetch_sub(1, std::memory_order_relaxed);
        return cur_free_.ptrs[--cur_free_.free_num];
      }
      // 从本地 block 中分配内存
      if (cur_block_ != nullptr && cur_block_->item_num < kBlockNItem) {
        T* obj = new ((T*)cur_block_->items + cur_block_->item_num) T(std::forward<Targs>(args)...);
        if (!ObjectPoolValidator<T>::validate(obj)) {
          obj->~T();
          return nullptr;
        }
        ++cur_block_->item_num;
        return obj;
      }
      // 从全局 block 中分配内存
      cur_block_ = AddBlock(&cur_block_index_);
      if (cur_block_ != nullptr) {
        T* obj = new ((T*)cur_block_->items + cur_block_->item_num) T(std::forward<Targs>(args)...);
        if (!ObjectPoolValidator<T>::validate(obj)) {
          obj->~T();
          return nullptr;
        }
        ++cur_block_->item_num;
        return obj;
      }
      return nullptr;
    }

    int ReturnObject(T* ptr) {
      // 归还到本地 free list
      if (cur_free_.free_num < ObjectPool::FreeChunkItemNum()) {
        cur_free_.ptrs[cur_free_.free_num++] = ptr;
        global_free_num_.fetch_add(1, std::memory_order_relaxed);
        return 0;
      }
      // 本地 free list 满了，先把本地 free list 归还到 global
      if (pool_->PushFreeChunk(cur_free_)) {
        cur_free_.free_num = 1;
        cur_free_.ptrs[0] = ptr;
        global_free_num_.fetch_add(1, std::memory_order_relaxed);
        return 0;
      }
      return -1;
    }

   private:
    ObjectPool* pool_;
    Block* cur_block_;
    size_t cur_block_index_;
    FreeChunk cur_free_;
  };

  T* GetObject() {
    LocalPool* lp = GetOrNewLocalPool();
    if (likely(lp != nullptr)) {
      return lp->get();
    }
    return nullptr;
  }

  template <typename A1>
  T* GetObject(const A1& arg1) {
    LocalPool* lp = GetOrNewLocalPool();
    if (likely(lp != nullptr)) {
      return lp->get(arg1);
    }
    return nullptr;
  }

  template <typename A1, typename A2>
  T* GetObject(const A1& arg1, const A2& arg2) {
    LocalPool* lp = GetOrNewLocalPool();
    if (likely(lp != nullptr)) {
      return lp->get(arg1, arg2);
    }
    return nullptr;
  }

  int ReturnObject(T* ptr) {
    LocalPool* lp = GetOrNewLocalPool();
    if (likely(lp != nullptr)) {
      return lp->ReturnObject(ptr);
    }
    return -1;
  }

  void ClearObjects() {
    LocalPool* lp = local_pool_;
    if (lp != nullptr) {
      local_pool_ = nullptr;
      // thread_atexit_cancel(LocalPool::DeleteLocalPool, lp);
      delete lp;
    }
  }

  static size_t FreeChunkItemNum() {
    const size_t n = ObjectPoolBlockMaxItem<T>::value();
    return (n < kFreeChunkNItem ? n : kFreeChunkNItem);
  }

  ObjectPoolInfo DescribeObjects() const {
    ObjectPoolInfo info;
    info.local_pool_num = local_num_.load(std::memory_order_relaxed);
    info.block_group_num = group_num_.load(std::memory_order_relaxed);
    info.block_num = 0;
    info.item_num = 0;
    info.free_chunk_item_num = FreeChunkItemNum();
    info.block_item_num = kBlockNItem;
    info.free_item_num = global_free_num_.load(std::memory_order_relaxed);

    for (size_t i = 0; i < info.block_group_num; ++i) {
      BlockGroup* bg = block_groups_[i].load(std::memory_order_consume);
      if (bg == nullptr) {
        break;
      }
      size_t block_num = std::min(bg->block_num.load(std::memory_order_relaxed), kOPGroupNBlock);
      info.block_num += block_num;
      for (size_t j = 0; j < block_num; ++j) {
        Block* b = bg->blocks[j].load(std::memory_order_consume);
        if (b != nullptr) {
          info.item_num += b->item_num;
        }
      }
    }
    info.total_size = info.block_num * info.block_item_num * sizeof(T);
    return info;
  }

 private:
  ObjectPool() { free_chunks_.reserve(kOPInitialFreeListSize); }

  ~ObjectPool() = default;

  static Block* AddBlock(size_t* index) {
    Block* const new_block = new (std::nothrow) Block;
    if (new_block == nullptr) {
      return nullptr;
    }

    size_t group_num;
    do {
      group_num = group_num_.load(std::memory_order_acquire);
      if (group_num >= 1) {
        BlockGroup* const g = block_groups_[group_num - 1].load(std::memory_order_consume);
        const size_t block_index = g->block_num.fetch_add(1, std::memory_order_relaxed);
        if (block_index < kOPGroupNBlock) {
          g->blocks[block_index].store(new_block, std::memory_order_release);
          *index = (group_num - 1) * kOPGroupNBlock + block_index;
          return new_block;
        }
        g->block_num.fetch_sub(1, std::memory_order_relaxed);
      }
    } while (AddBlockGroup(group_num));

    // Fail to add block group
    delete new_block;
    return nullptr;
  }

  static bool AddBlockGroup(size_t old_group_num) {
    BlockGroup* bg = nullptr;
    std::scoped_lock<std::mutex> lock(block_group_mutex_);
    const size_t group_num = group_num_.load(std::memory_order_acquire);
    if (group_num != old_group_num) {
      // 其它线程获取锁，并增加了block group
      return true;
    }
    if (group_num < kOPMaxBlockNGroup) {
      bg = new (std::nothrow) BlockGroup;
      if (bg != nullptr) {
        block_groups_[group_num].store(bg, std::memory_order_release);
        group_num_.store(group_num + 1, std::memory_order_release);
      }
    }
    return bg != nullptr;
  }

  LocalPool* GetOrNewLocalPool() {
    LocalPool* lp = local_pool_;
    if (likely(lp != nullptr)) {
      return lp;
    }

    lp = new (std::nothrow) LocalPool(this);
    if (lp == nullptr) {
      return nullptr;
    }
    std::scoped_lock<std::mutex> lock(change_thread_mutex_);
    local_pool_ = lp;
    // thread_atexit(LocalPool::DeleteLocalPool, lp);
    local_num_.fetch_add(1, std::memory_order_relaxed);
    return lp;
  }

  void ClearFromDestructorOfLocalPool() {
    local_pool_ = nullptr;
    // 没有有效线程，什么也不做
    if (local_num_.fetch_sub(1, std::memory_order_relaxed) != 1) {
      return;
    }
  }

 private:
  bool PopFreeChunk(FreeChunk& c) {
    if (free_chunks_.empty()) {
      return false;
    }
    free_chunks_mutex_.lock();
    if (free_chunks_.empty()) {
      free_chunks_mutex_.unlock();
      return false;
    }
    DynamicFreeChunk* p = free_chunks_.back();
    free_chunks_.pop_back();
    free_chunks_.unlock();
    c.free_num = p->free_num;
    memcpy(c.ptrs, p->ptrs, sizeof(*p->ptrs) * p->free_num);
    free(p);
    return true;
  }

  bool PushFreeChunk(const FreeChunk& c) {
    DynamicFreeChunk* p =
        static_cast<DynamicFreeChunk*>(malloc(offsetof(DynamicFreeChunk, ptrs) + sizeof(*c.ptrs) * c.free_num));
    if (p == nullptr) {
      return false;
    }
    p->free_num = c.free_num;
    memcpy(p->ptrs, c.ptrs, sizeof(*c.ptrs) * c.free_num);
    std::scoped_lock<std::mutex> lock(free_chunks_mutex_);
    free_chunks_.emplace_back(p);
    return true;
  }

  static thread_local LocalPool* local_pool_;
  static std::atomic<long> local_num_;
  static std::atomic<size_t> group_num_;
  static std::mutex block_group_mutex_;
  static std::mutex change_thread_mutex_;
  static std::atomic<BlockGroup*> block_groups_[kOPMaxBlockNGroup];

  std::vector<DynamicFreeChunk*> free_chunks_;
  std::mutex free_chunks_mutex_;
  static std::atomic<size_t> global_free_num_;
};

template <typename T>
const size_t ObjectPool<T>::kFreeChunkNItem;

template <typename T>
thread_local typename ObjectPool<T>::LocalPool* ObjectPool<T>::local_pool_ = nullptr;

template <typename T>
std::atomic<long> ObjectPool<T>::local_num_ = 0;

template <typename T>
std::atomic<size_t> ObjectPool<T>::group_num_ = 0;

template <typename T>
std::atomic<typename ObjectPool<T>::BlockGroup*> ObjectPool<T>::block_groups_[kOPMaxBlockNGroup] = {};

template <typename T>
std::atomic<size_t> ObjectPool<T>::global_free_num_ = 0;

inline std::ostream& operator<<(std::ostream& os, const ObjectPoolInfo& info) {
  return os << "local_pool_num: " << info.local_pool_num << "\nblocal_group_num: " << info.block_group_num
            << "\nblock_num: " << info.block_num << "\nitem_num: " << info.item_num
            << "\nblock_item_num: " << info.block_item_num << "\nfree_chunk_item_num: " << info.free_chunk_item_num
            << "\ntotal_size: " << info.total_size << "\nfree_num: " << info.free_item_num;
}

