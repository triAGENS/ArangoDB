import React, { useContext, useState } from "react";
import { UrlParametersContext } from "./url-parameters-context";
import { Checkbox, Tooltip } from 'antd';
import { InfoCircleFilled } from '@ant-design/icons';

const ParameterNodeSizeByEdges = () => {
  const [urlParameters, setUrlParameters] = useContext(UrlParametersContext);
  const [nodeSizeByEdges, setNodeSizeByEdges] = useState(urlParameters.nodeSizeByEdges);

  const NEWURLPARAMETERS = { ...urlParameters };

  return (
    <div>
      <Checkbox
        checked={nodeSizeByEdges}
        onChange={() => {
          const newNodeSizeByEdges = !nodeSizeByEdges;
          setNodeSizeByEdges(newNodeSizeByEdges);
          NEWURLPARAMETERS.nodeSizeByEdges = newNodeSizeByEdges;
          setUrlParameters(NEWURLPARAMETERS);
        }}
        style={{ 'color': '#736b68' }}
      >
        Size by connections
      </Checkbox>
      <Tooltip placement="bottom" title={"Should nodes be colorized by their collection? If enabled, node color and node color attribute will be ignored."}>
        <InfoCircleFilled style={{ fontSize: '12px', color: '#555555' }} />
      </Tooltip>
    </div>
  );
};

export default ParameterNodeSizeByEdges;
