////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2016 ArangoDB GmbH, Cologne, Germany
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
/// @author Jan Christoph Uhde
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGODB_REST_VST_RESPONSE_H
#define ARANGODB_REST_VST_RESPONSE_H 1

#include "Basics/StringBuffer.h"
#include "Rest/GeneralResponse.h"
#include "Rest/VstMessage.h"

namespace arangodb {

class VstResponse : public GeneralResponse {
  friend class rest::GeneralCommTask;

 public:
  static bool HIDE_PRODUCT_HEADER;

  VstResponse(ResponseCode code, uint64_t mid);

  // required by base
  uint64_t messageId() const override { return _messageId; }
  virtual arangodb::Endpoint::TransportType transportType() override {
    return arangodb::Endpoint::TransportType::VST;
  };

  void reset(ResponseCode code) override final;
  void addPayload(VPackSlice const&, arangodb::velocypack::Options const* = nullptr,
                  bool resolveExternals = true) override;
  void addPayload(VPackBuffer<uint8_t>&&, arangodb::velocypack::Options const* = nullptr,
                  bool resolveExternals = true) override;
  void addRawPayload(VPackStringRef payload) override;
  
 private:
  //_responseCode   - from Base
  //_headers        - from Base
  uint64_t _messageId;
  /// actual payloads
  std::vector<VPackBuffer<uint8_t>> _vpackPayloads;
};
}  // namespace arangodb

#endif
