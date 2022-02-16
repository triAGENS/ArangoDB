#include "ClusterProviderStep.h"

using namespace arangodb::graph;

namespace arangodb::graph {
auto operator<<(std::ostream& out, ClusterProviderStep const& step)
    -> std::ostream& {
  out << step._vertex.getID();
  return out;
}
}  // namespace arangodb::graph

ClusterProviderStep::ClusterProviderStep(const VertexType& v)
    : _vertex(v), _edge(), _fetched(false) {}

ClusterProviderStep::ClusterProviderStep(const VertexType& v,
                                         const EdgeType& edge, size_t prev)
    : BaseStep(prev), _vertex(v), _edge(edge), _fetched(false) {}

ClusterProviderStep::ClusterProviderStep(VertexType v, EdgeType edge,
                                         size_t prev, bool fetched)
    : BaseStep(prev),
      _vertex(std::move(v)),
      _edge(std::move(edge)),
      _fetched(fetched) {}

ClusterProviderStep::ClusterProviderStep(VertexType v, EdgeType edge,
                                         size_t prev, bool fetched,
                                         size_t depth)
    : BaseStep(prev, depth),
      _vertex(std::move(v)),
      _edge(std::move(edge)),
      _fetched(fetched) {}

ClusterProviderStep::ClusterProviderStep(VertexType v, EdgeType edge,
                                         size_t prev, bool fetched,
                                         size_t depth, double weight)
    : BaseStep(prev, depth, weight),
      _vertex(std::move(v)),
      _edge(std::move(edge)),
      _fetched(fetched) {}

ClusterProviderStep::ClusterProviderStep(VertexType v, size_t depth,
                                         double weight)
    : BaseStep(std::numeric_limits<size_t>::max(), depth, weight),
      _vertex(std::move(v)),
      _edge(),
      _fetched(false) {}

ClusterProviderStep::~ClusterProviderStep() = default;

VertexType const& ClusterProviderStep::Vertex::getID() const { return _vertex; }

ClusterProviderStep::EdgeType const& ClusterProviderStep::Edge::getID() const {
  return _edge;
}
bool ClusterProviderStep::Edge::isValid() const { return !_edge.empty(); };

bool ClusterProviderStep::isResponsible(transaction::Methods* trx) {
  return true;
}
