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

#ifndef UTIL_SPAN_H
#define UTIL_SPAN_H

#include <algorithm>
#include <cassert>
#include <iterator>
#include <limits>

#include "bandwidth.h"

namespace champsim
{
template <typename It>
std::pair<It, It> get_span(It begin, It end, bandwidth sz)
{
  assert(std::distance(begin, end) >= 0);
  assert(sz.amount_remaining() >= 0);
  auto distance = std::min(std::distance(begin, end), sz.amount_remaining());
  return {begin, std::next(begin, distance)};
}

template <typename It, typename F>
/**
 * @brief 获取一个满足条件的元素范围，同时考虑带宽限制
 * 
 * 这个函数首先使用get_span获取一个受带宽限制的元素范围，然后在这个范围内
 * 查找第一个不满足条件的元素，返回从开始到该元素的范围。
 * 
 * @param begin 范围的起始迭代器
 * @param end 范围的结束迭代器
 * @param sz 带宽限制，决定最多可以处理多少个元素
 * @param func 条件函数，用于判断元素是否满足条件
 * @return 返回一个pair，包含满足条件的元素范围的起始和结束迭代器
 */
std::pair<It, It> get_span_p(It begin, It end, bandwidth sz, F&& func)
{
  auto [span_begin, span_end] = get_span(begin, end, sz);
  return {span_begin, std::find_if_not(span_begin, span_end, std::forward<F>(func))};
}

template <typename It, typename F>
/**
 * @brief 获取一个满足条件的元素范围，不考虑带宽限制
 * 
 * 这个函数在给定的范围内查找第一个不满足条件的元素，返回从开始到该元素的范围。
 * 与带带宽限制的版本不同，这个版本不考虑带宽限制，会处理整个范围内的元素。
 * 
 * @param begin 范围的起始迭代器
 * @param end 范围的结束迭代器
 * @param func 条件函数，用于判断元素是否满足条件
 * @return 返回一个pair，包含满足条件的元素范围的起始和结束迭代器
 */
std::pair<It, It> get_span_p(It begin, It end, F&& func)
{
  return {begin, std::find_if_not(begin, end, std::forward<F>(func))};
}
} // namespace champsim

#endif
