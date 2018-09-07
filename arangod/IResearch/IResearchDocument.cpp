//////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2017 EMC Corporation
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
/// Copyright holder is EMC Corporation
///
/// @author Andrey Abramov
/// @author Vasiliy Nabatchikov
////////////////////////////////////////////////////////////////////////////////

#include "IResearchCommon.h"
#include "IResearchDocument.h"
#include "IResearchViewMeta.h"
#include "IResearchKludge.h"
#include "Misc.h"

#include "analysis/token_streams.hpp"
#include "analysis/token_attributes.hpp"

#include "Basics/StaticStrings.h"
#include "Basics/VelocyPackHelper.h"
#include "Logger/LogMacros.h"

#include "utils/log.hpp"
#include "utils/numeric_utils.hpp"

#define Swap8Bytes(val) \
 ( (((val) >> 56) & UINT64_C(0x00000000000000FF)) | (((val) >> 40) & UINT64_C(0x000000000000FF00)) | \
   (((val) >> 24) & UINT64_C(0x0000000000FF0000)) | (((val) >>  8) & UINT64_C(0x00000000FF000000)) | \
   (((val) <<  8) & UINT64_C(0x000000FF00000000)) | (((val) << 24) & UINT64_C(0x0000FF0000000000)) | \
   (((val) << 40) & UINT64_C(0x00FF000000000000)) | (((val) << 56) & UINT64_C(0xFF00000000000000)) )

NS_LOCAL

// ----------------------------------------------------------------------------
// --SECTION--                                       FieldIterator dependencies
// ----------------------------------------------------------------------------

enum AttributeType : uint8_t {
  AT_REG = arangodb::basics::VelocyPackHelper::AttributeBase,  // regular attribute
  AT_KEY = arangodb::basics::VelocyPackHelper::KeyAttribute,   // _key
  AT_REV = arangodb::basics::VelocyPackHelper::RevAttribute,   // _rev
  AT_ID = arangodb::basics::VelocyPackHelper::IdAttribute,     // _id
  AT_FROM = arangodb::basics::VelocyPackHelper::FromAttribute, // _from
  AT_TO = arangodb::basics::VelocyPackHelper::ToAttribute      // _to
}; // AttributeType

static_assert(
  arangodb::iresearch::adjacencyChecker<AttributeType>::checkAdjacency<
    AT_TO, AT_FROM, AT_ID, AT_REV, AT_KEY, AT_REG
  >(), "Values are not adjacent"
);

irs::string_ref const CID_FIELD("@_CID");
irs::string_ref const RID_FIELD("@_REV");
irs::string_ref const PK_COLUMN("@_PK");

// wrapper for use objects with the IResearch unbounded_object_pool
template<typename T>
struct AnyFactory {
  typedef std::shared_ptr<T> ptr;

  template<typename... Args>
  static ptr make(Args&&... args) {
    return std::make_shared<T>(std::forward<Args>(args)...);
  }
}; // AnyFactory

size_t const DEFAULT_POOL_SIZE = 8; // arbitrary value
irs::unbounded_object_pool<AnyFactory<std::string>> BufferPool(DEFAULT_POOL_SIZE);
irs::unbounded_object_pool<AnyFactory<irs::string_token_stream>> StringStreamPool(DEFAULT_POOL_SIZE);
irs::unbounded_object_pool<AnyFactory<irs::null_token_stream>> NullStreamPool(DEFAULT_POOL_SIZE);
irs::unbounded_object_pool<AnyFactory<irs::boolean_token_stream>> BoolStreamPool(DEFAULT_POOL_SIZE);
irs::unbounded_object_pool<AnyFactory<irs::numeric_token_stream>> NumericStreamPool(DEFAULT_POOL_SIZE);
irs::flags NumericStreamFeatures{ irs::granularity_prefix::type() };

// appends the specified 'value' to 'out'
inline void append(std::string& out, size_t value) {
  auto const size = out.size(); // intial size
  out.resize(size + 21); // enough to hold all numbers up to 64-bits
  auto const written = sprintf(&out[size], IR_SIZE_T_SPECIFIER, value);
  out.resize(size + written);
}

inline bool keyFromSlice(
    VPackSlice keySlice,
    irs::string_ref& key
) {
  // according to Helpers.cpp, see
  // `transaction::helpers::extractKeyFromDocument`
  // `transaction::helpers::extractRevFromDocument`
  // `transaction::helpers::extractIdFromDocument`
  // `transaction::helpers::extractFromFromDocument`
  // `transaction::helpers::extractToFromDocument`

  switch (keySlice.type()) {
    case VPackValueType::SmallInt: // system attribute
      switch (AttributeType(keySlice.head())) { // system attribute type
        case AT_REG:
          return false;
        case AT_KEY:
          key = arangodb::StaticStrings::KeyString;
          break;
        case AT_REV:
          key = arangodb::StaticStrings::RevString;
          break;
        case AT_ID:
          return false; // not supported
        case AT_FROM:
          key = arangodb::StaticStrings::FromString;
          break;
        case AT_TO:
          key = arangodb::StaticStrings::ToString;
          break;
        default:
          return false;
      }
      return true;
    case VPackValueType::String: // regular attribute
      key = arangodb::iresearch::getStringRef(keySlice);
      return true;
    default: // unsupported
      return false;
  }
}

inline bool canHandleValue(
    VPackSlice const& value,
    arangodb::iresearch::IResearchLinkMeta const& context
) noexcept {
  switch (value.type()) {
    case VPackValueType::None:
    case VPackValueType::Illegal:
      return false;
    case VPackValueType::Null:
    case VPackValueType::Bool:
    case VPackValueType::Array:
    case VPackValueType::Object:
    case VPackValueType::Double:
      return true;
    case VPackValueType::UTCDate:
    case VPackValueType::External:
    case VPackValueType::MinKey:
    case VPackValueType::MaxKey:
      return false;
    case VPackValueType::Int:
    case VPackValueType::UInt:
    case VPackValueType::SmallInt:
      return true;
    case VPackValueType::String:
      return !context._analyzers.empty();
    case VPackValueType::Binary:
    case VPackValueType::BCD:
    case VPackValueType::Custom:
    default:
      return false;
  }
}

// returns 'context' in case if can't find the specified 'field'
inline arangodb::iresearch::IResearchLinkMeta const* findMeta(
    irs::string_ref const& key,
    arangodb::iresearch::IResearchLinkMeta const* context
) {
  TRI_ASSERT(context);

  auto const* meta = context->_fields.findPtr(key);
  return meta ? meta->get() : context;
}

inline bool inObjectFiltered(
    std::string& buffer,
    arangodb::iresearch::IResearchLinkMeta const*& context,
    arangodb::iresearch::IteratorValue const& value
) {
  irs::string_ref key;

  if (!keyFromSlice(value.key, key)) {
    return false;
  }

  auto const* meta = findMeta(key, context);

  if (meta == context) {
    return false;
  }

  buffer.append(key.c_str(), key.size());
  context = meta;

  return canHandleValue(value.value, *context);
}

inline bool inObject(
    std::string& buffer,
    arangodb::iresearch::IResearchLinkMeta const*& context,
    arangodb::iresearch::IteratorValue const& value
) {
  irs::string_ref key;

  if (!keyFromSlice(value.key, key)) {
    return false;
  }

  buffer.append(key.c_str(), key.size());
  context = findMeta(key, context);

  return canHandleValue(value.value, *context);
}

inline bool inArrayOrdered(
    std::string& buffer,
    arangodb::iresearch::IResearchLinkMeta const*& context,
    arangodb::iresearch::IteratorValue const& value
) {
  buffer += arangodb::iresearch::NESTING_LIST_OFFSET_PREFIX;
  append(buffer, value.pos);
  buffer += arangodb::iresearch::NESTING_LIST_OFFSET_SUFFIX;

  return canHandleValue(value.value, *context);
}

inline bool inArray(
    std::string& /*buffer*/,
    arangodb::iresearch::IResearchLinkMeta const*& context,
    arangodb::iresearch::IteratorValue const& value
) noexcept {
  return canHandleValue(value.value, *context);
}

typedef bool(*Filter)(
  std::string& buffer,
  arangodb::iresearch::IResearchLinkMeta const*& context,
  arangodb::iresearch::IteratorValue const& value
);

Filter const valueAcceptors[] = {
  &inObjectFiltered, // type == Object, nestListValues == false, includeAllValues == false
  &inObject,         // type == Object, nestListValues == false, includeAllValues == true
  &inObjectFiltered, // type == Object, nestListValues == true , includeAllValues == false
  &inObject,         // type == Object, nestListValues == true , includeAllValues == true
  &inArray,          // type == Array , nestListValues == flase, includeAllValues == false
  &inArray,          // type == Array , nestListValues == flase, includeAllValues == true
  &inArrayOrdered,   // type == Array , nestListValues == true,  includeAllValues == false
  &inArrayOrdered    // type == Array , nestListValues == true,  includeAllValues == true
};

inline Filter getFilter(
  VPackSlice value,
  arangodb::iresearch::IResearchLinkMeta const& meta
) noexcept {
  TRI_ASSERT(arangodb::iresearch::isArrayOrObject(value));

  return valueAcceptors[
    4 * value.isArray()
      + 2 * meta._trackListPositions
      + meta._includeAllFields
   ];
}

void setNullValue(
    VPackSlice const& value,
    std::string& name,
    arangodb::iresearch::Field& field
) {
  TRI_ASSERT(value.isNull());

  // mangle name
  arangodb::iresearch::kludge::mangleNull(name);

  // init stream
  auto stream = NullStreamPool.emplace();
  stream->reset();

  // set field properties
  field._name = name;
  field._analyzer = stream;
  field._features = &irs::flags::empty_instance();
}

void setBoolValue(
    VPackSlice const& value,
    std::string& name,
    arangodb::iresearch::Field& field
) {
  TRI_ASSERT(value.isBool());

  // mangle name
  arangodb::iresearch::kludge::mangleBool(name);

  // init stream
  auto stream = BoolStreamPool.emplace();
  stream->reset(value.getBool());

  // set field properties
  field._name = name;
  field._analyzer = stream;
  field._features = &irs::flags::empty_instance();
}

void setNumericValue(
    VPackSlice const& value,
    std::string& name,
    arangodb::iresearch::Field& field
) {
  TRI_ASSERT(value.isNumber());

  // mangle name
  arangodb::iresearch::kludge::mangleNumeric(name);

  // init stream
  auto stream = NumericStreamPool.emplace();
  stream->reset(value.getNumber<double>());

  // set field properties
  field._name = name;
  field._analyzer = stream;
  field._features = &NumericStreamFeatures;
}

bool setStringValue(
    VPackSlice const& value,
    std::string& name,
    arangodb::iresearch::Field& field,
    arangodb::iresearch::IResearchAnalyzerFeature::AnalyzerPool::ptr const& pool
) {
  TRI_ASSERT(value.isString());

  if (!pool) {
    LOG_TOPIC(WARN, arangodb::iresearch::TOPIC)
      << "got nullptr analyzer factory";

    return false;
  }

  // it's important to unconditionally mangle name
  // since we unconditionally unmangle it in 'next'
  arangodb::iresearch::kludge::mangleStringField(name, *pool);

  // init stream
  auto analyzer = pool->get();

  if (!analyzer) {
    LOG_TOPIC(WARN, arangodb::iresearch::TOPIC)
      << "got nullptr from analyzer factory, name '" << pool->name() <<  "'";
    return false;
  }

  // init stream
  analyzer->reset(arangodb::iresearch::getStringRef(value));

  // set field properties
  field._name = name;
  field._analyzer =  analyzer;
  field._features = &(pool->features());

  return true;
}

void setIdValue(
    uint64_t& value,
    irs::token_stream& analyzer
) {
#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  auto& sstream = dynamic_cast<irs::string_token_stream&>(analyzer);
#else
  auto& sstream = static_cast<irs::string_token_stream&>(analyzer);
#endif

  sstream.reset(arangodb::iresearch::DocumentPrimaryKey::encode(value));
}

NS_END

NS_BEGIN(arangodb)
NS_BEGIN(iresearch)

// ----------------------------------------------------------------------------
// --SECTION--                                             Field implementation
// ----------------------------------------------------------------------------

/*static*/ void Field::setCidValue(Field& field, TRI_voc_cid_t& cid) {
  field._name = CID_FIELD;
  setIdValue(cid, *field._analyzer);
  field._features = &irs::flags::empty_instance();
}

/*static*/ void Field::setCidValue(
    Field& field,
    TRI_voc_cid_t& cid,
    Field::init_stream_t
) {
  field._analyzer = StringStreamPool.emplace();
  setCidValue(field, cid);
}

/*static*/ void Field::setRidValue(Field& field, TRI_voc_rid_t& rid) {
  field._name = RID_FIELD;
  setIdValue(rid, *field._analyzer);
  field._features = &irs::flags::empty_instance();
}

/*static*/ void Field::setRidValue(
    Field& field,
    TRI_voc_rid_t& rid,
    Field::init_stream_t
) {
  field._analyzer = StringStreamPool.emplace();
  setRidValue(field, rid);
}

Field::Field(Field&& rhs)
  : _features(rhs._features),
    _analyzer(std::move(rhs._analyzer)),
    _name(std::move(rhs._name)),
    _storeValues(std::move(rhs._storeValues)) {
  rhs._features = nullptr;
}

Field& Field::operator=(Field&& rhs) {
  if (this != &rhs) {
    _features = rhs._features;
    _analyzer = std::move(rhs._analyzer);
    _name = std::move(rhs._name);
    _storeValues = std::move(rhs._storeValues);
    rhs._features = nullptr;
  }

  return *this;
}

// ----------------------------------------------------------------------------
// --SECTION--                                     FieldIterator implementation
// ----------------------------------------------------------------------------

/*static*/ FieldIterator const FieldIterator::END;

FieldIterator::FieldIterator(): _name(BufferPool.emplace()) {
  // initialize iterator's value
}

FieldIterator::FieldIterator(
    VPackSlice const& doc,
    IResearchLinkMeta const& linkMeta
): FieldIterator() {
  reset(doc, linkMeta);
}

void FieldIterator::reset(
    VPackSlice const& doc,
    IResearchLinkMeta const& linkMeta
) {
  // set surrogate analyzers
  _begin = nullptr;
  _end = 1 + _begin;
  // clear stack
  _stack.clear();
  // clear field name
  _name->clear();

  if (!isArrayOrObject(doc)) {
    // can't handle plain objects
    return;
  }

  auto const* context = &linkMeta;

  // push the provided 'doc' to stack and initialize current value
  if (!pushAndSetValue(doc, context)) {
    next();
  }
}

bool FieldIterator::pushAndSetValue(VPackSlice slice, IResearchLinkMeta const*& context) {
  auto& name = nameBuffer();

  while (isArrayOrObject(slice)) {
    if (!name.empty() && !slice.isArray()) {
      name += NESTING_LEVEL_DELIMITER;
    }

    auto const filter = getFilter(slice, *context);

    _stack.emplace_back(slice, name.size(), *context, filter);

    auto& it = top().it;

    if (!it.valid()) {
      // empty object or array, skip it
      return false;
    }

    auto& value = it.value();

    if (!filter(name, context, value)) {
      // filtered out
      TRI_ASSERT(context);
      return false;
    }

    slice = value.value;
  }

  TRI_ASSERT(context);

  // set value
  _begin = nullptr;
  _end = 1 + _begin;                // set surrogate analyzers

  return setRegularAttribute(*context);
}

bool FieldIterator::setRegularAttribute(IResearchLinkMeta const& context) {
  auto const value = topValue().value;

  _value._storeValues = context._storeValues;

  switch (value.type()) {
    case VPackValueType::None:
    case VPackValueType::Illegal:
      return false;
    case VPackValueType::Null:
      setNullValue(value, nameBuffer(), _value);
      return true;
    case VPackValueType::Bool:
      setBoolValue(value, nameBuffer(), _value);
      return true;
    case VPackValueType::Array:
    case VPackValueType::Object:
      return true;
    case VPackValueType::Double:
      setNumericValue(value, nameBuffer(), _value);
      return true;
    case VPackValueType::UTCDate:
    case VPackValueType::External:
    case VPackValueType::MinKey:
    case VPackValueType::MaxKey:
      return false;
    case VPackValueType::Int:
    case VPackValueType::UInt:
    case VPackValueType::SmallInt:
      setNumericValue(value, nameBuffer(), _value);
      return true;
    case VPackValueType::String:
      resetAnalyzers(context); // reset string analyzers
      return setStringValue(value, nameBuffer(), _value, *_begin);
    case VPackValueType::Binary:
    case VPackValueType::BCD:
    case VPackValueType::Custom:
    default:
      return false;
  }
}

void FieldIterator::next() {
  TRI_ASSERT(valid());

  for (auto const* prev = _begin; ++_begin != _end; ) {
    auto& name = nameBuffer();

    // remove previous suffix
    arangodb::iresearch::kludge::demangleStringField(name, **prev);

    // can have multiple analyzers for string values only
    if (setStringValue(topValue().value, name, _value, *_begin)) {
      return;
    }
  }

  IResearchLinkMeta const* context;

  auto& name = nameBuffer();

  auto nextTop = [this, &name](){
    auto& level = top();
    auto& it = level.it;
    auto const* context = level.meta;
    auto const filter = level.filter;

    name.resize(level.nameLength);
    while (it.next() && !filter(name, context, it.value())) {
      // filtered out
      name.resize(level.nameLength);
    }

    return context;
  };

  do {
    // advance top iterator
    context = nextTop();

    // pop all exhausted iterators
    for (; !top().it.valid(); context = nextTop()) {
      _stack.pop_back();

      if (!valid()) {
        // reached the end
        return;
      }

      // reset name to previous size
      name.resize(top().nameLength);
    }

  } while (!pushAndSetValue(topValue().value, context));
}

// ----------------------------------------------------------------------------
// --SECTION--                                DocumentPrimaryKey implementation
// ----------------------------------------------------------------------------

/* static */ irs::string_ref const& DocumentPrimaryKey::PK() {
  return PK_COLUMN;
}

/* static */ irs::string_ref const& DocumentPrimaryKey::CID() {
  return CID_FIELD;
}

/* static */ irs::string_ref const& DocumentPrimaryKey::RID() {
  return RID_FIELD;
}

/* static */ bool DocumentPrimaryKey::decode(
    uint64_t& buf, const irs::bytes_ref& value
) {
  if (sizeof(uint64_t) != value.size()) {
    return false;
  }

  buf = *reinterpret_cast<uint64_t const*>(value.c_str());

  // convert from little endian
  if (irs::numeric_utils::is_big_endian()) {
    buf = Swap8Bytes(buf);
  }

  return true;
}

/* static */ irs::bytes_ref DocumentPrimaryKey::encode(uint64_t& value) {
  // ensure little endian
  if (irs::numeric_utils::is_big_endian()) {
    value = Swap8Bytes(value);
  }

  return irs::bytes_ref(
    reinterpret_cast<irs::byte_type const*>(&value),
    sizeof(value)
  );
}

DocumentPrimaryKey::DocumentPrimaryKey(
    TRI_voc_cid_t cid,
    TRI_voc_rid_t rid
) noexcept
  : _keys{ cid, rid } {
  static_assert(sizeof(_keys) == sizeof(cid) + sizeof(rid), "Invalid size");
}

bool DocumentPrimaryKey::read(irs::bytes_ref const& in) noexcept {
  if (sizeof(_keys) != in.size()) {
    return false;
  }

  std::memcpy(_keys, in.c_str(), sizeof(_keys));

  return true;
}

bool DocumentPrimaryKey::write(irs::data_output& out) const {
  out.write_bytes(
    reinterpret_cast<const irs::byte_type*>(_keys),
    sizeof(_keys)
  );

  return true;
}

bool appendKnownCollections(
    std::unordered_set<TRI_voc_cid_t>& set,
    const irs::index_reader& reader
) {
  auto visitor = [&set](TRI_voc_cid_t cid)->bool {
    set.emplace(cid);

    return true;
  };

  return visitReaderCollections(reader, visitor);
}

bool visitReaderCollections(
    irs::index_reader const& reader,
    std::function<bool(TRI_voc_cid_t cid)> const& visitor
) {
  for(auto& segment: reader) {
    auto* term_reader = segment.field(CID_FIELD);

    if (!term_reader) {
      LOG_TOPIC(ERR, arangodb::iresearch::TOPIC)
        << "failed to get term reader for the 'cid' column while collecting CIDs for IResearch reader";

      return false;
    }

    auto term_itr = term_reader->iterator();

    if (!term_itr) {
      LOG_TOPIC(ERR, arangodb::iresearch::TOPIC)
        << "failed to get term iterator for the 'cid' column while collecting CIDs for IResearch reader ";

      return false;
    }

    while(term_itr->next()) {
      TRI_voc_cid_t cid;

      if (!DocumentPrimaryKey::decode(cid, term_itr->value())) {
        LOG_TOPIC(ERR, arangodb::iresearch::TOPIC)
          << "failed to decode CID while collecting CIDs for IResearch reader";

        return false;
      }

      if (!visitor(cid)) {
        return false;
      }
    }
  }

  return true;
}

NS_END // iresearch
NS_END // arangodb

// -----------------------------------------------------------------------------
// --SECTION--                                                       END-OF-FILE
// -----------------------------------------------------------------------------
