import React, { useContext, useState } from "react";
import { UrlParametersContext } from "./url-parameters-context";
import { Input, Tooltip } from 'antd';
import { InfoCircleOutlined } from '@ant-design/icons';

const ParameterEdgeColorAttribute = () => {
  const [urlParameters, setUrlParameters] = useContext(UrlParametersContext);
  const [edgeColorAttribute, setEdgeColorAttribute] = useState(urlParameters.edgeColorAttribute);

  const NEWURLPARAMETERS = { ...urlParameters };
  
  return (
    <>
      <form>
        <label>
          Edge color attribute:
          <input
            type="text"
            value={edgeColorAttribute}
            onChange={(e) => {
              setEdgeColorAttribute(e.target.value);
              NEWURLPARAMETERS.edgeColorAttribute = e.target.value;
              setUrlParameters(NEWURLPARAMETERS);
            }}
            disabled={urlParameters.edgeColorByCollection}
          />
        </label>
      </form>
    </>
  );
};

export default ParameterEdgeColorAttribute;
