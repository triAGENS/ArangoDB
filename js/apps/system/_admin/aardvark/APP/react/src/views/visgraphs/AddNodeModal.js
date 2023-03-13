/* global arangoHelper, $ */
import React, { useState, useRef, useEffect } from 'react';
import styled from "styled-components";
import { JsonEditor as Editor } from 'jsoneditor-react';
import ToolTip from '../../components/arango/tootip';
import {
  Flex,
  Spacer } from '@chakra-ui/react';

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
  margin: 5% auto;
  padding: 20px;
  width: 50%;
`;

const StyledButton = styled.button`
  margin-left: 15px !important;
  color: white !important;
`;

  export const AddNodeModal = ({ shouldShow, onUpdateNode, onRequestClose, node, vertexCollections, nodeData, editorContent, children, onNodeCreation, graphName, graphData }) => {

  const keyInputRef = useRef();
  const jsonEditorRef = useRef();
  const [json, setJson] = useState(nodeData);
  const [collection, setCollection] = useState([]);

  useEffect(() => {
    if(shouldShow && vertexCollections.length){
      setCollection(vertexCollections[0].name)
    }
   
  }, [vertexCollections, shouldShow]);

  const openNotificationWithIcon = nodeName => {
    arangoHelper.arangoNotification(`The node ${nodeName} was successfully created`);
  };

  const addNode = (graphData, updateNodeId) => {

    const key = keyInputRef.current.value;
    let newJson = {...json};
    if (key !== '' && key !== undefined) {
      newJson = {...newJson, _key: key};
    }

    $.ajax({
      type: 'POST',
      url: arangoHelper.databaseUrl('/_api/gharial/') + encodeURIComponent(graphName) + '/vertex/' + encodeURIComponent(collection),
      contentType: 'application/json',
      data: JSON.stringify(newJson),
      processData: true,
      success: function (response) {
        const nodeModel = {
          id: response.vertex._id,
          label: response.vertex._key,
          shape: "dot"
        };
        openNotificationWithIcon(response.vertex._id);
        onNodeCreation(nodeModel);
        onRequestClose();
      },
      error: function (response) {
        arangoHelper.arangoError('Graph', 'Could not add node.');
        console.log("Error adding this node: ", response);
      }
    });
  }

  const handleChange = (event) => {
    setCollection(event.target.value);
  }

  if (!shouldShow) {
    return null;
  }

  return (
    <ModalBackground onClick={onRequestClose}>
      <ModalBody onClick={(e) => e.stopPropagation()}>
        <div>
          {children}<br />
        </div>
          <label for="nodeKey" style={{ 'marginTop': '12px' }}>
            _key
          </label>
          <div style={{ 'width': '100%', 'background': '#ffffff', 'display': 'flex', 'alignItems': 'center' }}>
            <input
              name="nodeKey"
              type="text"
              ref={keyInputRef}
              placeholder="is optional: leave empty for autogenerated key"
              style={{ width: "100%", marginRight: '8px' }}
            />
            <ToolTip
              title={"The node's unique key (optional attribute, leave empty for autogenerated key)"}
              setArrow={true}>
              <span className="arangoicon icon_arangodb_info" style={{ fontSize: '16px', color: '#989CA1', marginBottom: '10px' }}></span>
            </ToolTip>
          </div>

        <label for="vertexCollection" style={{ 'marginTop': '12px' }}>
          Vertex collection
        </label>
        <div style={{ 'width': '100%', 'background': '#ffffff', 'display': 'flex', 'alignItems': 'baseline' }}>
          <select
            placeholder="Please choose the vertex collection"
            name="vertexCollection"
            onChange={handleChange}
            style={{
              width: '100%',
              marginBottom: '24px',
              marginRight: '8px'
            }}
          >
            {
              vertexCollections.map((vertexCollection) => {
                return (
                  <option key={vertexCollection.name} value={vertexCollection.name}>{vertexCollection.name}</option>
                );
              })
            }
          </select>
          <ToolTip
            title={"Please select the target collection for the new node."}
            setArrow={true}
          >
            <span className="arangoicon icon_arangodb_info" style={{ fontSize: '16px', color: '#989CA1' }}></span>
          </ToolTip>
        </div>
        <div>
          {
            nodeData ? (
              <Editor
                ref={jsonEditorRef}
                value={nodeData}
                onChange={(value) => {
                  setJson(value);
                }}
                mode={'code'}
                history={true} />
            ) : 'Data is loading...'
          }
        </div>
        <Flex direction='row' mt='38'>
          <Spacer />
          <StyledButton className="button-close" onClick={onRequestClose}>Cancel</StyledButton>
          <StyledButton className="button-success" onClick={() => { addNode(node) }}>Create</StyledButton>
        </Flex>
      </ModalBody>
    </ModalBackground>
  );
};
