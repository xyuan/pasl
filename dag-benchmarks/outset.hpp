/*!
 * \file outset.hpp
 * \brief Implementation of the scalable outset data structure
 * \date 2015
 * \copyright COPYRIGHT (c) 2015 Umut Acar, Arthur Chargueraud, and
 * Michael Rainey. All rights reserved.
 * \license This project is released under the GNU Public License.
 */

#ifndef _PASL_DATA_OUTSET_H_
#define _PASL_DATA_OUTSET_H_

#include <atomic>
#include <array>
#include <type_traits>
#include <deque>

#include "tagged.hpp"
#include "microtime.hpp"
#include "perworker.hpp"

/***********************************************************************/

namespace pasl {
namespace data {
namespace outset {
  
namespace {
  
template <class T>
T* tagged_pointer_of(T* n) {
  return pasl::data::tagged::extract_value<T*>(n);
}

template <class T>
int tagged_tag_of(T* n) {
  return (int)pasl::data::tagged::extract_tag<long>(n);
}

template <class T>
T* tagged_tag_with(T* n, int t) {
  return pasl::data::tagged::create<T*, T*>(n, (long)t);
}

template <class T>
T* tagged_tag_with(int t) {
  return tagged_tag_with((T*)nullptr, t);
}

static constexpr int backoff_nb_cycles = 1l << 17;
template <class T>
bool compare_exchange(std::atomic<T>& cell, T& expected, T desired) {
  if (cell.compare_exchange_strong(expected, desired)) {
    return true;
  }
  pasl::util::microtime::wait_for(backoff_nb_cycles);
  return false;
}
  
template <class Item, int nb>
class static_cache_aligned_array {
private:
  
  static constexpr int cache_align_szb = 128;
  static constexpr int item_szb = sizeof(Item);
  
  using aligned_item_type = typename std::aligned_storage<item_szb, cache_align_szb>::type;
  
  aligned_item_type items[nb];
  
  Item& at(std::size_t i) {
    assert(i >= 0);
    assert(i < nb);
    return *reinterpret_cast<Item*>(items + i);
  }
  
public:
 
  Item& operator[](std::size_t i) {
    return at(i);
  }

  std::size_t size() const {
    return nb;
  }

  void init(Item x) {
    for (int i = 0; i < size(); i++) {
      at(i) = x;
    }
  }
  
};
  
} // end namespace

template <class Item, int capacity>
class block {
public:

  static constexpr Item* finished_tag = (Item*)nullptr;

  Item* start;
  std::atomic<Item*> head;

  block() {
    start = (Item*)malloc(capacity * sizeof(Item));
    head.store(start);
  }

  ~block() {
    free(start);
  }

  bool is_full() const {
    return head.load() >= (start + capacity);
  }
  
  void try_insert(Item x, bool& failed_because_finish, bool& failed_because_full) {
    while (true) {
      Item* orig = head.load();
      if (orig == finished_tag) {
        failed_because_finish = true;
        return;
      }
      if (orig >= (start + capacity)) {
        failed_because_full = true;
        return;
      }
      *orig = x;
      if (compare_exchange(head, orig, orig + 1)) {
        return;
      }
    }    
  }

};
  
template <class Item, int branching_factor, int block_capacity>
class node {
public:

  static constexpr int finished_tag = 1;

  using block_type = block<Item, block_capacity>;
  
  block_type items;
    
  static_cache_aligned_array<std::atomic<node*>, branching_factor> children;

  node() {
    for (int i = 0; i < branching_factor; i++) {
      children[i].store(nullptr);
    }
  }
  
};

template <class Item, int branching_factor, int block_capacity>
class tree {
public:

  using node_type = node<Item, branching_factor, block_capacity>;

private:
  
  std::atomic<node_type*> root;

public:

  tree() {
    root.store(new node_type);
  }

  template <class Random_int>
  node_type* try_insert(const Random_int& random_int) {
    node_type* new_node = nullptr;
    std::atomic<node_type*>* current = &root;
    while (true) {
      node_type* target = current->load();
      if (target == nullptr) {
        if (new_node == nullptr) {
          new_node = new node_type;
        }
        node_type* orig = nullptr;
        if (compare_exchange(*current, orig, new_node)) {
          break;
        }
        target = current->load();
      }
      if (tagged_tag_of(target) == node_type::finished_tag) {
        if (new_node != nullptr) {
          delete new_node;
          new_node = nullptr;
        }
        break;
      }
      int i = random_int(0, branching_factor);
      current = &(target->children[i]);
    }
    return new_node;
  }
    
};
  
template <class Item, int branching_factor, int block_capacity>
class outset {
public:

  static constexpr int max_nb_procs = 64;

  using tree_type = tree<Item, branching_factor, block_capacity>;
  using node_type = typename tree_type::node_type;
  using block_type = typename node_type::block_type;
  using shortcuts_type = static_cache_aligned_array<block_type*, max_nb_procs>;

  static constexpr int small_block_capacity = 16;
  using small_block_type = block<Item, small_block_capacity>;

  small_block_type items;
  
  tree_type t;
  
  std::atomic<shortcuts_type*> shortcuts;

  outset() {
    shortcuts.store(nullptr);
  }

  template <class Random_int>
  bool insert(Item x, int my_id, const Random_int& random_int) {
    if (! items.is_full()) {
      bool failed_because_finish = false;
      bool failed_because_full = false;
      items.try_insert(x, failed_because_finish, failed_because_full);
      if (failed_because_finish) {
        return false;
      }
      if (! failed_because_full) {
        return true;
      }
    }
    shortcuts_type* s = shortcuts.load();
    if (s == nullptr) {
      shortcuts_type* orig = nullptr;
      shortcuts_type* next = new shortcuts_type;
      for (int i = 0; i < next->size(); i++) {
        node_type* n = t.try_insert(random_int);
        if (n == nullptr) {
          delete next;
          return false;
        }
        (*next)[i] = &(n->items);
      }
      if (! compare_exchange(shortcuts, orig, next)) {
        delete next;
      }
      s = shortcuts.load();
    }  
    assert(s != nullptr); 
    while (true) {
      block_type* b = (*s)[my_id];
      bool failed_because_finish = false;
      bool failed_because_full = false;
      b->try_insert(x, failed_because_finish, failed_because_full);
      if (failed_because_finish) {
        return false;
      }
      if (! failed_because_full) {
        return true;
      }
      node_type* n = t.try_insert(random_int);
      if (n == nullptr) {
        return false;
      }
      (*s)[my_id] = &(n->items);
    }
  }

};
  
} // end namespace
} // end namespace
} // end namespace

/***********************************************************************/


#endif /*! _PASL_DATA_OUTSET_H_ */
