import styled from "styled-components";

const ModalBackground = styled.div`
  position: fixed;
  z-index: 1;
  left: 0;
  top: 0;
  width: 100%;
  height: 100%;
  overflow: auto;
  background-color: rgba(0, 0, 0, 0.5);
`;

const ModalBody = styled.div`
  background-color: white;
  margin: 10% auto;
  padding: 20px;
  width: 50%;
`;

export const DeleteNodeModal = ({ graphData, shouldShow, onDeleteNode, onRequestClose, nodeId, children }) => {
  const deleteNode = (graphData, deleteNodeId) => {
    console.log("DeleteNodeFromGraphData in deleteNode (graphData): ", graphData);
    console.log("DeleteNodeFromGraphData in deleteNode (deleteNodeId): ", deleteNodeId);

    console.log("DeleteNodeFromGraphData() - graphData.nodes: ", graphData.nodes);
    console.log("DeleteNodeFromGraphData() - graphData.edges: ", graphData.edges);
    console.log("DeleteNodeFromGraphData() - graphData.settings: ", graphData.settings);

    // delete node from nodes
    const nodes = graphData.nodes.filter((node) => {
      return node.id !== deleteNodeId;
    });

    // delete edges with deleteNodeId as source
    let edges = graphData.edges.filter((edge) => {
      return edge.source !== deleteNodeId;
    });

    // delete edges with deleteNodeId as target
    edges = edges.filter((edge) => {
      return edge.target !== deleteNodeId;
    });
   
    //const edges = graphData.edges;
    const settings = graphData.settings;
    const mergedGraphData = {
      nodes,
      edges,
      settings
    };
    console.log("########################");
    console.log("DeleteNodeFromGraphData() - nodes: ", nodes);
    console.log("DeleteNodeFromGraphData() - edges: ", edges);
    //console.log("DeleteNodeFromGraphData() - newGraphData: ", newGraphData);
    console.log("------------------------");
    console.log("DeleteNodeFromGraphData() - mergedGraphData: ", mergedGraphData);
    console.log("########################");
    onDeleteNode(mergedGraphData);
  }
  return shouldShow ? (
      <ModalBackground onClick={onRequestClose}>
        <ModalBody onClick={(e) => e.stopPropagation()}>
          <p>{children}<br />
          nodeId: {nodeId}</p>
          <button onClick={onRequestClose}>Cancel</button><button className="button-danger" onClick={() => { deleteNode(graphData, nodeId) }}>Delete</button>
        </ModalBody>
      </ModalBackground>
  ) : null;
};
