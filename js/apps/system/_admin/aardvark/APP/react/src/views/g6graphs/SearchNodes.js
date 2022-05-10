import React, { useState } from 'react';
import { Tooltip } from 'antd';
import { Node } from './components/node/node.component';
import { InfoCircleFilled } from '@ant-design/icons';
import PlainLabel from "./components/pure-css/form/PlainLabel";

const SearchNodes = ({ nodes, graphData, onNodeInfo, data, onDeleteNode, onNodeSelect }) => {
  const [previousSearchedNode, setPreviousSearchedNode] = useState(null);

  return (
    <div style={{'marginTop': '24px'}}>
      <PlainLabel htmlFor={'searchnodes'}>Search node</PlainLabel>
      <div className="node-list">
        <input
          list="searchnodelist"
          id="searchnodes"
          name="searchnodes"
          onChange={
            (e) => {
              if(e.target.value.includes("/")) {
                onNodeSelect(previousSearchedNode, e.target.value);
                setPreviousSearchedNode(e.target.value);
              }
            }
          }
          style={{
            'width': '500px',
            'height': 'auto',
            'margin-right': '8px',
            'color': '#555555',
            'border': '2px solid rgba(140, 138, 137, 0.25)',
            'border-radius': '4px',
            'background-color': '#fff !important',
            'box-shadow': 'none',
            'outline': 'none',
            'outline-color': 'transparent',
            'outline-style': 'none'
          }} />
        <datalist id="searchnodelist">
          {nodes.map(node => (
            <Node
              key={node.id}
              node={node} />
          ))}
        </datalist>
        <Tooltip placement="bottom" title={"Select the node you are looking for."}>
          <InfoCircleFilled style={{ fontSize: '12px', color: '#555555' }} />
        </Tooltip>
      </div>
    </div>
  );
};

export default SearchNodes;
