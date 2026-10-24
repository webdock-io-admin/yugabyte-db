// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.
//
// The following only applies to changes made to this file as part of YugabyteDB development.
//
// Portions Copyright (c) YugabyteDB, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//

#include <pthread.h>
#include <sched.h>

#include "yb/util/logging.h"
#include <gtest/gtest.h>

#include "yb/util/object_pool.h"
#include "yb/util/test_thread_holder.h"

namespace yb {

// Simple class which maintains a count of how many objects
// are currently alive.
class MyClass {
 public:
  MyClass() {
    instance_count_++;
  }

  ~MyClass() {
    instance_count_--;
  }

  static int instance_count() {
    return instance_count_;
  }

  static void ResetCount() {
    instance_count_ = 0;
  }

 private:
  static int instance_count_;
};
int MyClass::instance_count_ = 0;

TEST(TestObjectPool, TestPooling) {
  MyClass::ResetCount();
  {
    ObjectPool<MyClass> pool;
    ASSERT_EQ(0, MyClass::instance_count());
    MyClass *a = pool.Construct();
    ASSERT_EQ(1, MyClass::instance_count());
    MyClass *b = pool.Construct();
    ASSERT_EQ(2, MyClass::instance_count());
    ASSERT_TRUE(a != b);
    pool.Destroy(b);
    ASSERT_EQ(1, MyClass::instance_count());
    MyClass *c = pool.Construct();
    ASSERT_EQ(2, MyClass::instance_count());
    ASSERT_TRUE(c == b) << "should reuse instance";
    pool.Destroy(c);

    ASSERT_EQ(1, MyClass::instance_count());
  }

  ASSERT_EQ(0, MyClass::instance_count())
    << "destructing pool should have cleared instances";
}

TEST(TestObjectPool, TestScopedPtr) {
  MyClass::ResetCount();
  ASSERT_EQ(0, MyClass::instance_count());
  ObjectPool<MyClass> pool;
  {
    ObjectPool<MyClass>::scoped_ptr sptr(
      pool.make_scoped_ptr(pool.Construct()));
    ASSERT_EQ(1, MyClass::instance_count());
  }
  ASSERT_EQ(0, MyClass::instance_count());
}

TEST(TestObjectPool, PoolIndexForCpu) {
  using object_pool_internal::PoolIndexForCpu;
  ASSERT_EQ(0, PoolIndexForCpu(0, 4));
  ASSERT_EQ(3, PoolIndexForCpu(3, 4));
  // A host CPU id inside a container that sees only 4 online CPUs.
  ASSERT_LT(PoolIndexForCpu(80, 4), 4);
  ASSERT_LT(PoolIndexForCpu(123, 4), 4);
  // sched_getcpu() failed.
  ASSERT_LT(PoolIndexForCpu(-1, 4), 4);
  ASSERT_EQ(0, PoolIndexForCpu(-1, 1));
}

#if !defined(__APPLE__)
// Takes and releases an object on each CPU this process may run on. Every CPU id returned by
// sched_getcpu() must have its own pool, so an object released on a CPU is reused on that CPU.
TEST(TestObjectPool, ThreadSafePoolOnEveryCpu) {
  cpu_set_t allowed;
  CPU_ZERO(&allowed);
  ASSERT_EQ(0, sched_getaffinity(0, sizeof(allowed), &allowed));
  ASSERT_GE(base::MaxCPUIndex() + 1, CPU_COUNT(&allowed));

  MyClass::ResetCount();
  {
    ThreadSafeObjectPool<MyClass> pool;
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
      if (!CPU_ISSET(cpu, &allowed)) {
        continue;
      }
      SCOPED_TRACE(cpu);
      int seen_cpu = -1;
      MyClass* first = nullptr;
      MyClass* second = nullptr;
      TestThreadHolder holder;
      holder.AddThreadFunctor([&] {
        cpu_set_t only;
        CPU_ZERO(&only);
        CPU_SET(cpu, &only);
        if (pthread_setaffinity_np(pthread_self(), sizeof(only), &only) != 0) {
          return;
        }
        seen_cpu = sched_getcpu();
        first = pool.Take();
        pool.Release(first);
        second = pool.Take();
        pool.Release(second);
      });
      holder.JoinAll();
      ASSERT_EQ(cpu, seen_cpu);
      ASSERT_NE(nullptr, first);
      ASSERT_EQ(first, second) << "object released on CPU " << cpu << " should be reused there";
    }
  }
  ASSERT_EQ(0, MyClass::instance_count());
}
#endif // !defined(__APPLE__)

} // namespace yb
