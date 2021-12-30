////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2021 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
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
/// @author Andrey Abramov
/// @author Vasiliy Nabatchikov
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <locale>
#include <mutex>
#include <vector>

#include "analysis/analyzer.hpp"
#include "utils/compression.hpp"
#include "utils/object_pool.hpp"
#include "utils/range.hpp"

#include "Containers.h"
#include "IResearchAnalyzerFeature.h"
#include "IResearchViewSort.h"
#include "IResearchViewStoredValues.h"
#include "IResearchCompression.h"

namespace arangodb {
namespace velocypack {

class Builder;         // forward declarations
struct ObjectBuilder;  // forward declarations
class Slice;           // forward declarations

}  // namespace velocypack
}  // namespace arangodb

namespace arangodb {
namespace iresearch {

////////////////////////////////////////////////////////////////////////////////
/// @brief enum of possible ways to store values in the view
////////////////////////////////////////////////////////////////////////////////
enum class ValueStorage : uint32_t {
  NONE = 0,  // do not store values in the view
  ID,        // only store value existance
  VALUE      // store full value in the view
};

struct FieldMeta {
  // can't use FieldMeta as value type since it's incomplete type so far
  typedef UnorderedRefKeyMap<char, UniqueHeapInstance<FieldMeta>> Fields;

  struct Analyzer {
    Analyzer();  // identity analyzer
    Analyzer(AnalyzerPool::ptr const& pool, std::string&& shortName) noexcept
        : _pool(pool), _shortName(std::move(shortName)) {}

    operator bool() const noexcept { return false == !_pool; }

    AnalyzerPool::ptr _pool;
    std::string _shortName;  // vocbase-independent short analyzer name
  };

  struct AnalyzerComparer {
    using is_transparent = void;

    bool operator()(AnalyzerPool::ptr const& lhs,
                    AnalyzerPool::ptr const& rhs) const noexcept {
      return lhs->name() < rhs->name();
    }

    bool operator()(AnalyzerPool::ptr const& lhs,
                    irs::string_ref const& rhs) const noexcept {
      return lhs->name() < rhs;
    }

    bool operator()(irs::string_ref const& lhs,
                    AnalyzerPool::ptr const& rhs) const noexcept {
      return lhs < rhs->name();
    }
  };

  struct Mask {
    explicit Mask(bool mask = false) noexcept
        : _analyzers(mask),
          _fields(mask),
          _includeAllFields(mask),
          _trackListPositions(mask),
          _storeValues(mask) {}

    bool _analyzers;
    bool _fields;
    bool _includeAllFields;
    bool _trackListPositions;
    bool _storeValues;
  };

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief return default IResearchLinkMeta values
  ////////////////////////////////////////////////////////////////////////////////
  static const FieldMeta& DEFAULT();

  FieldMeta();
  FieldMeta(FieldMeta const&) = default;
  FieldMeta(FieldMeta&&) = default;

  FieldMeta& operator=(FieldMeta const&) = default;
  FieldMeta& operator=(FieldMeta&&) = default;

  bool operator==(FieldMeta const& rhs) const noexcept;
  bool operator!=(FieldMeta const& rhs) const noexcept {
    return !(*this == rhs);
  }

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief initialize FieldMeta with values from a JSON description
  ///        return success or set 'errorField' to specific field with error
  ///        on failure state is undefined
  /// @param server underlying application server
  /// @param slice input definition
  /// @param errorField field causing error (out-param)
  /// @param defaultVocbase fallback vocbase for analyzer name normalization
  ///                       nullptr == do not normalize
  /// @param defaults inherited defaults
  /// @param mask if set reflects which fields were initialized from JSON
  /// @param referencedAnalyzers analyzers referenced in this link
  ////////////////////////////////////////////////////////////////////////////////
  bool init(arangodb::application_features::ApplicationServer& server,
            velocypack::Slice const& slice, std::string& errorField,
            irs::string_ref const defaultVocbase,
            FieldMeta const& defaults = DEFAULT(), Mask* mask = nullptr,
            std::set<AnalyzerPool::ptr, AnalyzerComparer>* referencedAnalyzers =
                nullptr);

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief fill and return a JSON description of a FieldMeta object
  ///        do not fill values identical to ones available in 'ignoreEqual'
  ///        or (if 'mask' != nullptr) values in 'mask' that are set to false
  ///        elements are appended to an existing object
  ///        return success or set TRI_set_errno(...) and return false
  /// @param server underlying application server
  /// @param builder output buffer
  /// @param defaultVocbase fallback vocbase for analyzer name normalization
  ///                       nullptr == do not normalize
  /// @param ignoreEqual values to ignore if equal
  /// @param defaultVocbase fallback vocbase
  /// @param mask if set reflects which fields were initialized from JSON
  ////////////////////////////////////////////////////////////////////////////////
  bool json(arangodb::application_features::ApplicationServer& server,
            arangodb::velocypack::Builder& builder,
            FieldMeta const* ignoreEqual = nullptr,
            TRI_vocbase_t const* defaultVocbase = nullptr,
            Mask const* mask = nullptr) const;

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief amount of memory in bytes occupied by this FieldMeta
  ////////////////////////////////////////////////////////////////////////////////
  size_t memory() const noexcept;

  std::vector<Analyzer> _analyzers;  // analyzers to apply to every field
  size_t _primitiveOffset;
  Fields
      _fields;  // explicit list of fields to be indexed with optional overrides
  ValueStorage _storeValues{
      ValueStorage::NONE};  // how values should be stored inside the view
  bool _includeAllFields{
      false};  // include all fields or only fields listed in '_fields'
  bool _trackListPositions{
      false};  // append relative offset in list to attribute name (as opposed
               // to without offset)
};

////////////////////////////////////////////////////////////////////////////////
/// @brief metadata describing how to process a field in a collection
////////////////////////////////////////////////////////////////////////////////
struct IResearchLinkMeta : public FieldMeta {
  struct Mask : public FieldMeta::Mask {
    explicit Mask(bool mask = false) noexcept
        : FieldMeta::Mask(mask),
          _analyzerDefinitions(mask),
          _sort(mask),
          _storedValues(mask),
          _sortCompression(mask),
          _collectionName(mask),
          _version(mask) {}

    bool _analyzerDefinitions;
    bool _sort;
    bool _storedValues;
    bool _sortCompression;
    bool _collectionName;
    bool _version;
  };

  std::set<AnalyzerPool::ptr, FieldMeta::AnalyzerComparer> _analyzerDefinitions;
  IResearchViewSort _sort;  // sort condition associated with the link
  IResearchViewStoredValues
      _storedValues;  // stored values associated with the link
  irs::type_info::type_id _sortCompression{getDefaultCompression()};
  uint32_t _version;  // the version of the iresearch interface e.g. which how
                      // data is stored in iresearch (default == 0)

  /// @brief Linked collection name. Stored here for cluster deployment only.
  /// For sigle server collection could be renamed so can`t store it here or
  /// syncronisation will be needed. For cluster rename is not possible so
  /// there is no problem but solved recovery issue - we will be able to index
  /// _id attribute without doing agency request for collection name
  std::string _collectionName;

  // NOTE: if adding fields don't forget to modify the comparison operator !!!
  // NOTE: if adding fields don't forget to modify IResearchLinkMeta::Mask !!!
  // NOTE: if adding fields don't forget to modify IResearchLinkMeta::Mask
  // constructor !!! NOTE: if adding fields don't forget to modify the init(...)
  // function !!! NOTE: if adding fields don't forget to modify the json(...)
  // function !!! NOTE: if adding fields don't forget to modify the memory()
  // function !!!

  IResearchLinkMeta();
  IResearchLinkMeta(IResearchLinkMeta const& other) = default;
  IResearchLinkMeta(IResearchLinkMeta&& other) noexcept = default;

  IResearchLinkMeta& operator=(IResearchLinkMeta&& other) = default;
  IResearchLinkMeta& operator=(IResearchLinkMeta const& other) = default;

  bool operator==(IResearchLinkMeta const& rhs) const noexcept;
  bool operator!=(IResearchLinkMeta const& rhs) const noexcept {
    return !(*this == rhs);
  }

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief return default IResearchLinkMeta values
  ////////////////////////////////////////////////////////////////////////////////
  static const IResearchLinkMeta& DEFAULT();

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief initialize IResearchLinkMeta with values from a JSON description
  ///        return success or set 'errorField' to specific field with error
  ///        on failure state is undefined
  /// @param slice definition
  /// @param readAnalyzerDefinition allow reading analyzer definitions instead
  ///                               of just name
  /// @param erroField field causing error (out-param)
  /// @param defaultVocbase fallback vocbase for analyzer name normalization
  ///                       nullptr == do not normalize
  /// @param defaults inherited defaults
  /// @param mask if set reflects which fields were initialized from JSON
  ////////////////////////////////////////////////////////////////////////////////
  bool init(application_features::ApplicationServer& server, VPackSlice slice,
            bool readAnalyzerDefinition, std::string& errorField,
            irs::string_ref const defaultVocbase = irs::string_ref::NIL,
            IResearchLinkMeta const& defaults = DEFAULT(),
            Mask* mask = nullptr);

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief fill and return a JSON description of a IResearchLinkMeta object
  ///        do not fill values identical to ones available in 'ignoreEqual'
  ///        or (if 'mask' != nullptr) values in 'mask' that are set to false
  ///        elements are appended to an existing object
  ///        return success or set TRI_set_errno(...) and return false
  /// @param builder output buffer (out-param)
  /// @param writeAnalyzerDefinition output full analyzer definition instead of
  /// just name
  /// @param ignoreEqual values to ignore if equal
  /// @param defaultVocbase fallback vocbase for analyzer name normalization
  ///                       nullptr == do not normalize
  /// @param mask if set reflects which fields were initialized from JSON
  ////////////////////////////////////////////////////////////////////////////////
  bool json(arangodb::application_features::ApplicationServer& server,
            arangodb::velocypack::Builder& builder,
            bool writeAnalyzerDefinition,
            IResearchLinkMeta const* ignoreEqual = nullptr,
            TRI_vocbase_t const* defaultVocbase = nullptr,
            Mask const* mask = nullptr) const;

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief amount of memory in bytes occupied by this IResearchLinkMeta
  ////////////////////////////////////////////////////////////////////////////////
  size_t memory() const noexcept;
};  // IResearchLinkMeta

struct InvertedIndexFieldMeta {
  ////////////////////////////////////////////////////////////////////////////////
  /// @brief initialize InvertedIndexFieldMeta with values from a JSON
  /// description
  ///        return success or set 'errorField' to specific field with error
  ///        on failure state is undefined
  /// @param server underlying application server
  /// @param slice input definition
  /// @param readAnalyzerDefinition allow reading analyzer definitions instead
  ///                               of just name
  /// @param errorField field causing error (out-param)
  /// @param defaultVocbase fallback vocbase for analyzer name normalization
  ///                       nullptr == do not normalize
  ////////////////////////////////////////////////////////////////////////////////
  bool init(arangodb::application_features::ApplicationServer& server,
            velocypack::Slice const& slice, bool readAnalyzerDefinition,
            std::string& errorField, irs::string_ref const defaultVocbase);

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief fill and return a JSON description
  /// @param server underlying application server
  /// @param builder output buffer
  /// @param writeAnalyzerDefinition output full analyzer definition instead of
  /// just name
  /// @param defaultVocbase fallback vocbase for analyzer name normalization
  ///                       nullptr == do not normalize
  /// @param defaultVocbase fallback vocbase
  ////////////////////////////////////////////////////////////////////////////////
  bool json(arangodb::application_features::ApplicationServer& server,
            arangodb::velocypack::Builder& builder,
            bool writeAnalyzerDefinition,
            TRI_vocbase_t const* defaultVocbase = nullptr) const;

  bool hasExtra() const noexcept;

  size_t extraFieldsIdx() const noexcept;

  struct FieldRecord {
    FieldRecord(std::vector<basics::AttributeName> const& path,
                FieldMeta::Analyzer&& a);

    std::string toString() const;

    bool isIdentical(std::vector<basics::AttributeName> const& path,
                     irs::string_ref analyzerName) const noexcept;

    /// @brief attribute path
    std::vector<basics::AttributeName> attribute;
    /// @brief array sub-path in case of expansion (maybe empty)
    std::vector<basics::AttributeName> expansion;
    /// @brief analyzer to apply
    FieldMeta::Analyzer analyzer;
  };

  using Fields = std::vector<FieldRecord>;

  bool operator==(InvertedIndexFieldMeta const& other) const noexcept;

  static bool matchesFieldsDefinition(InvertedIndexFieldMeta const& meta,
                                      VPackSlice other);

  std::set<AnalyzerPool::ptr, FieldMeta::AnalyzerComparer> _analyzerDefinitions;
  Fields _fields;
  IResearchViewSort _sort;  // sort condition associated with the link
  IResearchViewStoredValues
      _storedValues;  // stored values associated with the link
  irs::type_info::type_id _sortCompression{getDefaultCompression()};
  irs::string_ref _collectionName;
  uint32_t _version;  // the version of the iresearch interface e.g. which how
                      // data is stored in iresearch (default == 0)
};                    // InvertedIndexFieldMeta

}  // namespace iresearch
}  // namespace arangodb
