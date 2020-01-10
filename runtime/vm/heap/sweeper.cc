// Copyright (c) 2011, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#include "vm/heap/sweeper.h"

#include "vm/compiler/assembler/assembler.h"
#include "vm/globals.h"
#include "vm/heap/freelist.h"
#include "vm/heap/heap.h"
#include "vm/heap/pages.h"
#include "vm/heap/safepoint.h"
#include "vm/lockers.h"
#include "vm/thread_pool.h"
#include "vm/timeline.h"

namespace dart {

bool GCSweeper::SweepPage(HeapPage* page, FreeList* freelist, bool locked) {
  ASSERT(!page->is_image_page());

  // Keep track whether this page is still in use.
  intptr_t used_in_bytes = 0;

  bool is_executable = (page->type() == HeapPage::kExecutable);
  uword start = page->object_start();
  uword end = page->object_end();
  uword current = start;

  while (current < end) {
    intptr_t obj_size;
    RawObject* raw_obj = RawObject::FromAddr(current);
    ASSERT(HeapPage::Of(raw_obj) == page);
    if (raw_obj->IsMarked()) {
      // Found marked object. Clear the mark bit and update swept bytes.
      raw_obj->ClearMarkBit();
      obj_size = raw_obj->HeapSize();
      used_in_bytes += obj_size;
    } else {
      uword free_end = current + raw_obj->HeapSize();
      while (free_end < end) {
        RawObject* next_obj = RawObject::FromAddr(free_end);
        if (next_obj->IsMarked()) {
          // Reached the end of the free block.
          break;
        }
        // Expand the free block by the size of this object.
        free_end += next_obj->HeapSize();
      }
      obj_size = free_end - current;
      if (is_executable) {
        uword cursor = current;
        uword end = current + obj_size;
        while (cursor < end) {
          *reinterpret_cast<uword*>(cursor) =
              compiler::Assembler::GetBreakInstructionFiller();
          cursor += kWordSize;
        }
      } else {
#if defined(DEBUG)
        memset(reinterpret_cast<void*>(current), Heap::kZapByte, obj_size);
#endif  // DEBUG
      }
      if ((current != start) || (free_end != end)) {
        // Only add to the free list if not covering the whole page.
        if (locked) {
          freelist->FreeLocked(current, obj_size);
        } else {
          freelist->Free(current, obj_size);
        }
      }
    }
    current += obj_size;
  }
  ASSERT(current == end);

  page->set_used_in_bytes(used_in_bytes);
  return used_in_bytes != 0;  // In use.
}

intptr_t GCSweeper::SweepLargePage(HeapPage* page) {
  ASSERT(!page->is_image_page());

  intptr_t words_to_end = 0;
  RawObject* raw_obj = RawObject::FromAddr(page->object_start());
  ASSERT(HeapPage::Of(raw_obj) == page);
  if (raw_obj->IsMarked()) {
    raw_obj->ClearMarkBit();
    words_to_end = (raw_obj->HeapSize() >> kWordSizeLog2);
  }
#ifdef DEBUG
  // Array::MakeFixedLength creates trailing filler objects,
  // but they are always unreachable. Verify that they are not marked.
  uword current = RawObject::ToAddr(raw_obj) + raw_obj->HeapSize();
  uword end = page->object_end();
  while (current < end) {
    RawObject* cur_obj = RawObject::FromAddr(current);
    ASSERT(!cur_obj->IsMarked());
    intptr_t obj_size = cur_obj->HeapSize();
    memset(reinterpret_cast<void*>(current), Heap::kZapByte, obj_size);
    current += obj_size;
  }
#endif  // DEBUG
  return words_to_end;
}

class ConcurrentSweeperTask : public ThreadPool::Task {
 public:
  ConcurrentSweeperTask(Isolate* isolate,
                        PageSpace* old_space,
                        HeapPage* first,
                        HeapPage* last,
                        HeapPage* large_first,
                        HeapPage* large_last,
                        FreeList* freelist)
      : task_isolate_(isolate),
        old_space_(old_space),
        first_(first),
        last_(last),
        large_first_(large_first),
        large_last_(large_last),
        freelist_(freelist) {
    ASSERT(task_isolate_ != NULL);
    ASSERT(first_ != NULL);
    ASSERT(old_space_ != NULL);
    ASSERT(last_ != NULL);
    ASSERT(freelist_ != NULL);
    MonitorLocker ml(old_space_->tasks_lock());
    old_space_->set_tasks(old_space_->tasks() + 1);
    old_space_->set_phase(PageSpace::kSweeping);
  }

  virtual void Run() {
    bool result =
        Thread::EnterIsolateAsHelper(task_isolate_, Thread::kSweeperTask, true);
    ASSERT(result);
    {
      Thread* thread = Thread::Current();
      ASSERT(thread->BypassSafepoints());  // Or we should be checking in.
      TIMELINE_FUNCTION_GC_DURATION(thread, "ConcurrentSweep");
      GCSweeper sweeper;

      HeapPage* page = large_first_;
      HeapPage* prev_page = NULL;
      while (page != NULL) {
        HeapPage* next_page;
        if (page == large_last_) {
          // Don't access page->next(), which would be a race with mutator
          // allocating new pages.
          next_page = NULL;
        } else {
          next_page = page->next();
        }
        ASSERT(page->type() == HeapPage::kData);
        const intptr_t words_to_end = sweeper.SweepLargePage(page);
        if (words_to_end == 0) {
          old_space_->FreeLargePage(page, prev_page);
        } else {
          old_space_->TruncateLargePage(page, words_to_end << kWordSizeLog2);
          prev_page = page;
        }
        page = next_page;
      }

      page = first_;
      prev_page = NULL;
      while (page != NULL) {
        HeapPage* next_page;
        if (page == last_) {
          // Don't access page->next(), which would be a race with mutator
          // allocating new pages.
          next_page = NULL;
        } else {
          next_page = page->next();
        }
        ASSERT(page->type() == HeapPage::kData);
        bool page_in_use = sweeper.SweepPage(page, freelist_, false);
        if (page_in_use) {
          prev_page = page;
        } else {
          old_space_->FreePage(page, prev_page);
        }
        {
          // Notify the mutator thread that we have added elements to the free
          // list or that more capacity is available.
          MonitorLocker ml(old_space_->tasks_lock());
          ml.Notify();
        }
        page = next_page;
      }
    }
    // Exit isolate cleanly *before* notifying it, to avoid shutdown race.
    Thread::ExitIsolateAsHelper(true);
    // This sweeper task is done. Notify the original isolate.
    {
      MonitorLocker ml(old_space_->tasks_lock());
      old_space_->set_tasks(old_space_->tasks() - 1);
      ASSERT(old_space_->phase() == PageSpace::kSweeping);
      old_space_->set_phase(PageSpace::kDone);
      ml.NotifyAll();
    }
  }

 private:
  Isolate* task_isolate_;
  PageSpace* old_space_;
  HeapPage* first_;
  HeapPage* last_;
  HeapPage* large_first_;
  HeapPage* large_last_;
  FreeList* freelist_;
};

void GCSweeper::SweepConcurrent(Isolate* isolate,
                                HeapPage* first,
                                HeapPage* last,
                                HeapPage* large_first,
                                HeapPage* large_last,
                                FreeList* freelist) {
  bool result = Dart::thread_pool()->Run<ConcurrentSweeperTask>(
      isolate, isolate->heap()->old_space(), first, last, large_first,
      large_last, freelist);
  ASSERT(result);
}

}  // namespace dart
