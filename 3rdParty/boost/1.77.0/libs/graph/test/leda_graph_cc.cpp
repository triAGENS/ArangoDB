//=======================================================================
// Copyright 1997, 1998, 1999, 2000 University of Notre Dame.
// Authors: Andrew Lumsdaine, Lie-Quan Lee, Jeremy G. Siek
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//=======================================================================
#include <boost/graph/graph_concepts.hpp>
#include <boost/graph/leda_graph.hpp>
#include <boost/concept/assert.hpp>

int main(int, char*[])
{
    using namespace boost;
    {
        typedef leda::GRAPH< int, int > Graph;
        typedef graph_traits< Graph >::vertex_descriptor Vertex;
        typedef graph_traits< Graph >::edge_descriptor Edge;
        BOOST_CONCEPT_ASSERT((VertexListGraphConcept< Graph >));
        BOOST_CONCEPT_ASSERT((BidirectionalGraphConcept< Graph >));
        BOOST_CONCEPT_ASSERT((AdjacencyGraphConcept< Graph >));
        BOOST_CONCEPT_ASSERT((VertexMutableGraphConcept< Graph >));
        BOOST_CONCEPT_ASSERT((EdgeMutableGraphConcept< Graph >));
        BOOST_CONCEPT_ASSERT((VertexMutablePropertyGraphConcept< Graph >));
        BOOST_CONCEPT_ASSERT((EdgeMutablePropertyGraphConcept< Graph >));
        BOOST_CONCEPT_ASSERT(
            (ReadablePropertyGraphConcept< Graph, Vertex, vertex_index_t >));
        BOOST_CONCEPT_ASSERT(
            (ReadablePropertyGraphConcept< Graph, Edge, edge_index_t >));
        BOOST_CONCEPT_ASSERT(
            (LvaluePropertyGraphConcept< Graph, Vertex, vertex_all_t >));
        BOOST_CONCEPT_ASSERT(
            (LvaluePropertyGraphConcept< Graph, Vertex, edge_all_t >));
    }
    return 0;
}
