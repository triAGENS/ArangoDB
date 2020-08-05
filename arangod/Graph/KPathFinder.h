////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2020-2020 ArangoDB GmbH, Cologne, Germany
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
/// @author Michael Hackstein
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGODB_GRAPH_K_PATHS_FINDER_H
#define ARANGODB_GRAPH_K_PATHS_FINDER_H 1

#include "Graph/EdgeDocumentToken.h"

#include <velocypack/StringRef.h>

#include <deque>
#include <set>
#include <vector>

namespace arangodb {

namespace velocypack {
class Builder;
}  // namespace velocypack

namespace graph {

class EdgeCursor;
class TraverserCache;

struct ShortestPathOptions;

class KPathFinder {
 private:
  enum Direction {FORWARD, BACKWARD};
 
  using VertexRef = arangodb::velocypack::StringRef;

  struct VertexIdentifier {
    VertexRef id;
    size_t predecessor;
    EdgeDocumentToken edge;

    // Make the set work on the VertexRef attribute only
    bool operator<(VertexIdentifier const& other) const;
//    bool operator<(VertexRef const& other) const;
  };

  class PathResult {
   public:
    auto clear() -> void;
    auto appendVertex(VertexRef v) -> void;
    auto prependVertex(VertexRef v) -> void;
    auto toVelocyPack(arangodb::velocypack::Builder& builder) -> void;

   private:
    std::deque<VertexRef> _vertices;
    std::deque<EdgeDocumentToken> _edges;
  };

  using Shell = std::set<VertexIdentifier>;
  using Interior = std::vector<VertexIdentifier>;
  using ResultList = std::deque<std::pair<VertexIdentifier, VertexIdentifier>>;

  // We have two balls, one arround source, one around target, and try to find intersections of the balls
  class Ball {
   public:
    Ball(Direction dir, ShortestPathOptions& options);
    ~Ball();
    auto reset(VertexRef center) -> void;
    auto startNextDepth() -> void;
    auto noPathLeft() const -> bool;
    auto getDepth() const -> size_t;
    auto shellSize() const -> size_t;
    auto doneWithDepth() const -> bool;

    auto buildPath(VertexIdentifier const& vertexInShell, PathResult& path) -> void;

    auto matchResultsInShell(VertexIdentifier const& match, ResultList& results) const -> void;
    auto computeNeighbourhoodOfNextVertex(Ball const& other, ResultList& results) -> void;

   private:
    VertexRef _center;
    Shell _shell{};
    Interior _interior{};
    size_t _depth{0};
    size_t _searchIndex{std::numeric_limits<size_t>::max()};
    Direction _direction;
    std::unique_ptr<EdgeCursor> _cursor;
    // We do not take responsibility for the Cache
    TraverserCache* _cache;

  };

 public:
  explicit KPathFinder(ShortestPathOptions& options);
  ~KPathFinder();

  /**
   * @brief Quick test of the finder can proof there is no more data available.
   *        It can respond with true, even though there is no path left.
   * @return true There is a chance that there is more data available
   * @return false There will be no further path.
   */
  bool hasMore() const;

  /**
   * @brief Reset to new source and target vertices.
   * This API uss string references, this class will not take responsibility
   * for the referenced data. It is callers responsibility to retain the
   * underlying data and make sure the StringRefs stay valid until next
   * call of reset.
   *
   * @param source The source vertex to start the paths
   * @param target The target vertex to end the paths
   */
  void reset(VertexRef source, VertexRef target);

  /**
   * @brief Get the next path, if available written into the result build.
   * The given builder will be not be cleared, this function requires a
   * prepared builder to write into.
   *
   * @param result Input and output, this needs to be an open builder,
   * where the path can be placed into.
   * Can be empty, or an openArray, or the value of an object.
   * Guarantee: Every returned path matches the conditions handed in via
   * options. No path is returned twice, it is intended that paths overlap.
   * @return true Found and written a path, result is modified.
   * @return false No path found, result has not been changed.
   */
  bool getNextPath(arangodb::velocypack::Builder& result);

private:
 auto searchDone() const -> bool;
 auto startNextDepth() -> void;

private:
 ShortestPathOptions& _opts;
 Ball _left;
 Ball _right;
 bool _searchLeft{true};
 ResultList _results{};

 PathResult _resultPath;
};

}  // namespace graph
}  // namespace arangodb

#endif