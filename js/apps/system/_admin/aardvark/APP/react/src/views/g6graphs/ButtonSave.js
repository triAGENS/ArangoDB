/* global arangoHelper, arangoFetch, frontendConfig, $ */

import React, { useContext, useState } from "react";
import { UrlParametersContext } from "./url-parameters-context";
import { Button, Tooltip } from 'antd';
import { SaveOutlined } from '@ant-design/icons';

const ButtonSave = ({ graphName, onGraphDataLoaded, onIsLoadingData }) => {
  const urlParameters = useContext(UrlParametersContext);
  const [isLoadingData, setIsLoadingData] = useState(false)

  const callApi = () => {
    console.log("urlParameters to use for API call: ", urlParameters);
      setIsLoadingData(true);
      onIsLoadingData(true);

      $.ajax({
        type: 'GET',
        url: arangoHelper.databaseUrl(`/_admin/aardvark/g6graph/${graphName}`),
        contentType: 'application/json',
        data: urlParameters[0],
        success: function (data) {
          const element = document.getElementById("graph-card");
          console.log("element: ", element);
          element.scrollIntoView({ behavior: "smooth" });
          setIsLoadingData(false);
          onIsLoadingData(false);
          onGraphDataLoaded(data);
        },
        error: function (e) {
          arangoHelper.arangoError('Graph', 'Could not load graph.');
        }
      });
  };
  
  return (
    <>
      <Tooltip placement="left" title={"Use current settings to receive data"}>    
        <Button
          type="primary"
          style={{ background: "#2ecc71", borderColor: "#2ecc71" }}
          icon={<SaveOutlined />}
          onClick={() => {
            console.log("urlParameters (to make API call): ", urlParameters);
            callApi();
          }}
          disabled={isLoadingData}
        >
          Save
        </Button>
      </Tooltip>
    </>
  );
};

export default ButtonSave;
