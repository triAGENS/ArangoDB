////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2022 ArangoDB GmbH, Cologne, Germany
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
/// @author Dr. Frank Celler
////////////////////////////////////////////////////////////////////////////////

#include "Events.h"

namespace arangodb {
namespace events {
void UnknownAuthenticationMethod(GeneralRequest const&) {}
void CredentialsMissing(GeneralRequest const&) {}
void LoggedIn(GeneralRequest const&, std::string const& username) {}
void CredentialsBad(GeneralRequest const&, std::string const& username) {}
void CredentialsBad(GeneralRequest const&, rest::AuthenticationMethod) {}
void Authenticated(GeneralRequest const&, rest::AuthenticationMethod) {}
void NotAuthorized(GeneralRequest const&) {}
void CreateCollection(std::string const& db, std::string const& name,
                      ErrorCode result) {}
void CreateCollection(std::string const& db, std::string_view name,
                      ErrorCode result) {}
void DropCollection(std::string const& db, std::string const& name,
                    ErrorCode result) {}
void PropertyUpdateCollection(std::string const& db,
                              std::string const& collectionName,
                              OperationResult const&) {}
void TruncateCollection(std::string const& db, std::string const& name,
                        OperationResult const& result) {}
void CreateDatabase(std::string const&, Result const&, ExecContext const&) {}
void DropDatabase(std::string const&, Result const&, ExecContext const&) {}
// index creation start
void CreateIndexStart(std::string const& db, std::string const& col,
                      VPackSlice slice) {}
// index creation result
void CreateIndexEnd(std::string const& db, std::string const& col,
                    VPackSlice slice, ErrorCode result) {}
void DropIndex(std::string const& db, std::string const& col,
               std::string const& idx, ErrorCode result) {}
void CreateView(std::string const& db, std::string const& name,
                ErrorCode result) {}
void DropView(std::string const& db, std::string const& name,
              ErrorCode result) {}
void CreateDocument(std::string const& db, std::string const& collection,
                    VPackSlice const& document, OperationOptions const& options,
                    ErrorCode) {}
void DeleteDocument(std::string const& db, std::string const& collection,
                    VPackSlice const& document, OperationOptions const& options,
                    ErrorCode) {}
void ReadDocument(std::string const& db, std::string const& collection,
                  VPackSlice const& document, OperationOptions const& options,
                  ErrorCode) {}
void ReplaceDocument(std::string const& db, std::string const& collection,
                     VPackSlice const& document,
                     OperationOptions const& options, ErrorCode) {}
void ModifyDocument(std::string const& db, std::string const& collection,
                    VPackSlice const& document, OperationOptions const& options,
                    ErrorCode code) {}
void IllegalDocumentOperation(GeneralRequest const& request,
                              rest::ResponseCode result) {}
void AqlQuery(aql::Query const& /*query*/) {}
void QueryDocument(std::string const& db, VPackSlice const&, int code) {}
void CreateHotbackup(std::string const& id, ErrorCode result) {}
void RestoreHotbackup(std::string const& id, ErrorCode result) {}
void DeleteHotbackup(std::string const& id, ErrorCode result) {}

}  // namespace events
}  // namespace arangodb
