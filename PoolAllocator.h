#pragma once

#include <cassert>
#include <cstddef>
#include <iostream>

template <typename T, std::size_t BlockCount>
class PoolAllocator {
 public:
  PoolAllocator();

  ~PoolAllocator();

  T* allocate();

  void deallocate(T* ptr);

 private:
  struct Node {
    Node* next{};
  };

  void printMemLayout();

  // create const that adds padding, if type of T is less than the size of Node, we can't align
  static constexpr size_t BLOCK_SIZE{std::max(sizeof(T), sizeof(Node))};

  alignas(BLOCK_SIZE) std::byte* m_pool{};

  void* m_free_list_head{};
};

template <typename T, std::size_t BlockCount>
void PoolAllocator<T, BlockCount>::printMemLayout() {
  Node* p{reinterpret_cast<Node*>(m_free_list_head)};
  while (p && p->next) {
    std::uintptr_t curAddr{reinterpret_cast<std::uintptr_t>(p)};
    std::uintptr_t nextAddr{reinterpret_cast<std::uintptr_t>(p->next)};

    std::cout << "Printing ADDR dif: " << nextAddr - curAddr << '\n';

    p = p->next;
  }
}

template <typename T, std::size_t BlockCount>
void PoolAllocator<T, BlockCount>::deallocate(T* ptr) {
  // though this function is called deallocate, the goal is to reinsert the block back into the
  // memory pool

  // assert(ptr != reinterpret_cast<T*>(m_free_list_head) &&
  //        "ERROR: Double deallocate of identical pointer");

  // can't really explore more options to protect against a double dealloc without compromising
  // efficiency

  Node* newHead{reinterpret_cast<Node*>(ptr)};
  newHead->next = reinterpret_cast<Node*>(m_free_list_head);
  m_free_list_head = newHead;
}

template <typename T, std::size_t BlockCount>
T* PoolAllocator<T, BlockCount>::allocate() {
  // if we try to allocate when we're empty, throw
  if (m_free_list_head == nullptr) throw std::bad_alloc();

  // get next node to assign as new head
  Node* head{reinterpret_cast<Node*>(m_free_list_head)};
  m_free_list_head = head->next;

  return reinterpret_cast<T*>(head);
}

template <typename T, std::size_t BlockCount>
PoolAllocator<T, BlockCount>::PoolAllocator() {
  constexpr std::size_t poolSize{BlockCount * BLOCK_SIZE};

  static_assert((poolSize % sizeof(Node) == 0));

  m_pool = new std::byte[poolSize];

  m_free_list_head = reinterpret_cast<void*>(m_pool);
  for (size_t i{0}; i < BlockCount - 1; ++i) {
    // calc address of current block, in bytes
    std::byte* curBytes{&m_pool[i * BLOCK_SIZE]};

    // get next block
    std::byte* nextBytes{&m_pool[(i + 1) * BLOCK_SIZE]};

    // tell compiler to treat a group of bytes as a node
    Node* curNode{reinterpret_cast<Node*>(curBytes)};

    // tell compiler to treat the next group of bytes as a node as well
    Node* next{reinterpret_cast<Node*>(nextBytes)};

    // write the address of next inside the first block of nodes
    curNode->next = next;
  }

  // for final byte, write nullptr inside so we know its the end
  std::byte* lastBytes{&m_pool[(BlockCount - 1) * BLOCK_SIZE]};
  Node* lastNode{reinterpret_cast<Node*>(lastBytes)};

  lastNode->next = nullptr;

  // printMemLayout();
}

template <typename T, std::size_t BlockCount>
PoolAllocator<T, BlockCount>::~PoolAllocator() {
  m_free_list_head = nullptr;
  delete[] m_pool;
}
