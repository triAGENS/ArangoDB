////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2019 ArangoDB GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Kaveh Vahedipour
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGODB_REST_SERVER_METRICS_H
#define ARANGODB_REST_SERVER_METRICS_H 1

#include <atomic>
#include <map>
#include <iostream>
#include <string>
#include <memory>
#include <variant>
#include <vector>
#include <unordered_map>
#include <cassert>
#include <cmath>
#include <limits>

#include "Basics/VelocyPackHelper.h"
#include "Logger/LogMacros.h"
#include "counter.h"

class Counter;

template<typename Scale> class Histogram;

class Metric {
public:
  Metric(std::string const& name, std::string const& help);
  virtual ~Metric();
  std::string const& help() const;
  std::string const& name() const;
  virtual void toPrometheus(std::string& result) const = 0;
  void header(std::string& result) const;
protected:
  std::string const _name;
  std::string const _help;
};

struct Metrics {

  enum Type {COUNTER, HISTOGRAM};

  using counter_type = gcl::counter::simplex<uint64_t, gcl::counter::atomicity::full>;
  using hist_type = gcl::counter::simplex_array<uint64_t, gcl::counter::atomicity::full>;
  using buffer_type = gcl::counter::buffer<uint64_t>;

};


/**
 * @brief Counter functionality
 */
class Counter : public Metric {
public:
  Counter(uint64_t const& val, std::string const& name, std::string const& help);
  Counter(Counter const&) = delete;
  ~Counter();
  std::ostream& print (std::ostream&) const;
  Counter& operator++();
  Counter& operator++(int);
  Counter& operator+=(uint64_t const&);
  Counter& operator=(uint64_t const&);
  void count();
  void count(uint64_t);
  uint64_t load() const;
  void store(uint64_t const&);
  void push();
  virtual void toPrometheus(std::string&) const override;
private:
  mutable Metrics::counter_type _c;
  mutable Metrics::buffer_type _b;
};


template<typename T> class Gauge : public Metric {
public:
  Gauge() = delete;
  Gauge(T const& val, std::string const& name, std::string const& help)
    : Metric(name, help) {
    _g.store(val);
  }
  Gauge(Gauge const&) = delete;
  ~Gauge() = default;
  std::ostream& print (std::ostream&) const;
  Gauge<T>& operator+=(T const& t) {
    _g.store(_g + t);
    return *this;
  }
  Gauge<T>& operator-=(T const& t) {
    _g.store(_g - t);
    return *this;
  }
  Gauge<T>& operator*=(T const& t) {
    _g.store(_g * t);
    return *this;
  }
  Gauge<T>& operator/=(T const& t) {
    TRI_ASSERT(t != T(0));
    _g.store(_g / t);
    return *this;
  }
  Gauge<T>& operator=(T const& t) {
    _g.store(t);
    return *this;
  }
  T load() const {
    return _g.load();
  };
  virtual void toPrometheus(std::string& result) const override {
    result += "#TYPE " + name() + " gauge\n";
    result += "#HELP " + name() + " " + help() + "\n";
    result += name() + " " + std::to_string(load()) + "\n";
  };
private:
  std::atomic<T> _g;
};

std::ostream& operator<< (std::ostream&, Metrics::hist_type const&);

template<typename T>
struct scale_t {
public:

  using value_type = T;

  scale_t(T const& low, T const& high, size_t n) :
    _low(low), _high(high) {
    _delim.resize(n);
  }
  /**
   * @brief number of buckets
   */
  size_t n() const {
    return _delim.size();
  }
  /**
   * @brief number of buckets
   */
  T low() const {
    return _low;
  }
  /**
   * @brief number of buckets
   */
  T high() const {
    return _high;
  }
  /**
   * @brief number of buckets
   */
  std::vector<T> const& delim() const {
    return _delim;
  }

protected:
  T _low, _high;
  std::vector<T> _delim;
};

template<typename T>
struct log_scale_t : public scale_t<T> {
public:

  using value_type = T;

  log_scale_t(T const& base, T const& low, T const& high, size_t n) :
    scale_t<T>(low, high, n), _base(base) { // Assertions
    double nn = -1.0*n;
    for (auto& i : this->_delim) {
      i = (high-low) * std::pow(base, ++nn) + low;
    }
    _div = this->_delim.front() - low;
    _ldiv = logf(_div);
  }
  /**
   * @brief index for val
   * @param val value
   * @return    index
   */
  size_t pos(T const& val) const {
    return static_cast<size_t>(std::floor(logf((val - this->_low)/_div)/logf(_base)));
  }
private:
  T _base, _div, _ldiv;
};

template<typename T>
struct lin_scale_t : public scale_t<T> {
public:

  using value_type = T;

  lin_scale_t(T const& low, T const& high, size_t n) :
    scale_t<T>(low, high, n) { // Assertions
    this->_delim.resize(n);
    _div = (high - low) / (T)n;
    T le = low;
    for (auto& i : this->_delim) {
      i = le;
      le += _div;
    }
  }
  /**
   * @brief index for val
   * @param val value
   * @return    index
   */
  size_t pos(T const& val) const {
    return static_cast<size_t>(std::floor((val - this->_low)/ _div));
  }
private:
  T _base, _div;
};


/**
 * @brief Histogram functionality
 */
template<typename Scale> class Histogram : public Metric {

public:

  using value_type = typename Scale::value_type;

  Histogram() = delete;

  Histogram (Scale scale, std::string const& name, std::string const& help = "")
    : Metric(name, help), _c(Metrics::hist_type(scale.n())), _scale(scale),
      _lowr(std::numeric_limits<value_type>::max()),
      _highr(std::numeric_limits<value_type>::min()),
      _n(scale.n()-1) {}

  ~Histogram() = default;

  void records(value_type const& val) {
    if(val < _lowr) {
      _lowr = val;
    } else if (val > _highr) {
      _highr = val;
    }
  }

  size_t pos(value_type const& t) const {
    return _scale.pos(t);
  }

  void count(value_type const& t) {
    count(t, 1);
  }

  void count(value_type const& t, uint64_t n) {
    if (t < _scale.low()) {
      _c[0] += n;
    } else if (t >= _scale.high()) {
      _c[_n] += n;
    } else {
      _c[pos(t)] += n;
    }
    records(t);
  }

  value_type const& low() const { return _scale.low(); }
  value_type const& high() const { return _scale.high(); }

  Metrics::hist_type::value_type& operator[](size_t n) {
    return _c[n];
  }

  std::vector<uint64_t> load() const {
    std::vector<uint64_t> v(size());
    for (size_t i = 0; i < size(); ++i) {
      v[i] = load(i);
    }
    return v;
  }

  uint64_t load(size_t i) const { return _c.load(i); };

  size_t size() const { return _c.size(); }

  virtual void toPrometheus(std::string& result) const override {
    result += "#TYPE " + name() + " histogram\n";
    result += "#HELP " + name() + " " + help() + "\n";
    value_type sum(0);
    for (size_t i = 0; i < size(); ++i) {
      uint64_t n = load(i);
      sum += n;
      result += name() + "_bucket{le=\"" + std::to_string(_scale.delim()[i]) + "\"} " +
        std::to_string(n) + "\n";
    }
    result += name() + "_count " + std::to_string(sum) + "\n";
  }

  std::ostream& print(std::ostream& o) const {
//    o << "_div: " << _div << ", _c: " << _c << ", _r: [" << _lowr << ", " << _highr << "] " << name();
    return o;
  }

private:
  Metrics::hist_type _c;
  Scale _scale;
  value_type _lowr, _highr;
  size_t _n;

};

std::ostream& operator<< (std::ostream&, Metrics::counter_type const&);
template<typename T>
std::ostream& operator<<(std::ostream& o, Histogram<T> const& h) {
  return h.print(o);
}


#endif
