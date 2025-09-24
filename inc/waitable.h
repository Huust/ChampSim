/*
 *    Copyright 2023 The ChampSim Contributors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef WAITABLE_H
#define WAITABLE_H

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>

#include "chrono.h"
#include "util/type_traits.h"

namespace champsim
{
/*
 * champsim::waitable是一个非常关键的抽象，它的名字本身就揭示了其作用：“一个需要等待才能就绪的数据”。
 * 在离散事件模拟器（Discrete-event simulator）如ChampSim中，任何操作都不是瞬时完成的，
 * 都需要消耗时间（延迟）。waitable就是用来封装一个带有时间延迟的数据的工具。
 *
 * 它内部主要包含两部分信息：
 * 数据本身 (Data): 例如，我们计算出的下一个PTE的物理地址，或者最终的物理页号。
 * 就绪时间 (Ready Time): 这个数据在未来的哪一个仿真周期才能被使用。
 *
 * 为什么需要它？
 * 当ptw.cc中finish_step或finish_last_step被调用时，它们会计算出下一步的结果。
 * 但这个结果并不能在当前周期马上被使用，因为它需要模拟两个延迟：
 * penalty: 可能存在的、来自vmem的缺页惩罚。
 * HIT_LATENCY: PTW内部处理和计算的流水线延迟。
 * waitable将“结果”和“结果的可用时间”绑定在一起。在operate函数的主循环中，
 * 代码会使用is_ready这个lambda来检查mshr_entry.data.is_ready_at(current_time)。
 * 只有当当前仿真时间大于等于waitable对象中记录的就绪时间时，这个MSHR才能被handle_fill处理或被返回给TLB。
 *
 * 总结：waitable是ChampSim中模拟时间延迟的核心机制。它确保了在PTW的流水线中，
 * 每一个步骤都必须在付出了相应的时间代价后，其结果才能对后续步骤可见。
 */
template <typename T>
class waitable
{
public:
  using value_type = T;
  using time_type = champsim::chrono::clock::time_point;

private:
  T m_value;
  std::optional<time_type> event_cycle{};

  // a time value that is greater than all possible times
  constexpr static time_type time_sentinel = time_type::max();

public:
  waitable() = default;
  explicit waitable(T val) : m_value(std::move(val)) {}
  explicit waitable(T val, time_type cycle) : m_value(std::move(val)), event_cycle(cycle) {}

  void ready_at(time_type cycle);
  void ready_by(time_type cycle);

  template <typename F>
  auto map(F&& func);

  template <typename F>
  auto and_then(F&& func);

  // template <typename F>
  // auto or_else(F&& func);

  // template <typename F>
  // auto map_or(F&& func);

  // template <typename F>
  // auto map_or_else(F&& func);

  // template <typename F>
  // auto conjunction(F&& func);

  // template <typename F>
  // auto disjunction(F&& func);

  void reset();

  bool is_ready_at(time_type cycle) const;
  bool has_unknown_readiness() const;

  auto& operator*();
  auto& operator*() const;
  auto operator->();
  auto operator->() const;
  auto& value();
  auto& value() const;
};
} // namespace champsim

template <typename T>
void champsim::waitable<T>::ready_at(time_type cycle)
{
  event_cycle = cycle;
}

template <typename T>
void champsim::waitable<T>::ready_by(time_type cycle)
{
  event_cycle = std::min(cycle, event_cycle.value_or(time_sentinel));
}

template <typename T>
template <typename F>
auto champsim::waitable<T>::map(F&& func)
{
  auto new_value = std::invoke(std::forward<F>(func), std::move(m_value));
  if (event_cycle.has_value())
    return waitable<decltype(new_value)>{new_value, *event_cycle};
  else
    return waitable<decltype(new_value)>{new_value};
}

template <typename T>
template <typename F>
auto champsim::waitable<T>::and_then(F&& func)
{
  static_assert(champsim::is_specialization_v<std::invoke_result_t<F, T>, waitable>);
  return std::invoke(std::forward<F>(func), std::move(m_value));
}

template <typename T>
void champsim::waitable<T>::reset()
{
  event_cycle.reset();
}

template <typename T>
bool champsim::waitable<T>::is_ready_at(time_type cycle) const
{
  return event_cycle.value_or(time_sentinel) <= cycle;
}

template <typename T>
bool champsim::waitable<T>::has_unknown_readiness() const
{
  return !event_cycle.has_value();
}

template <typename T>
auto& champsim::waitable<T>::operator*()
{
  return m_value;
}

template <typename T>
auto& champsim::waitable<T>::operator*() const
{
  return m_value;
}

template <typename T>
auto champsim::waitable<T>::operator->()
{
  return &(operator*());
}

template <typename T>
auto champsim::waitable<T>::operator->() const
{
  return &(operator*());
}

template <typename T>
auto& champsim::waitable<T>::value()
{
  return m_value;
}

template <typename T>
auto& champsim::waitable<T>::value() const
{
  return m_value;
}

#endif
