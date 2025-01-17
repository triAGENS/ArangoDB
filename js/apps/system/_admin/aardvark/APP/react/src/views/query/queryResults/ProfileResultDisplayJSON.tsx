import { Global } from "@emotion/react";
import { JsonEditor } from "jsoneditor-react";
import React, { useEffect, useRef } from "react";

export const ProfileResultDisplayJSON = ({
  defaultValue
}: {
  defaultValue?: string;
}) => {
  const jsonEditorRef = useRef(null);
  useEffect(() => {
    const editor = (jsonEditorRef.current as any)?.jsonEditor;
    editor.setText(defaultValue);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [jsonEditorRef]);
  useSetupProfileResultDisplay(jsonEditorRef);
  return (
    <>
      <Global
        styles={{
          ".jsoneditor div.jsoneditor-outer.has-status-bar": {
            padding: "0px",
            marginTop: "0px"
          }
        }}
      />
      <JsonEditor
        ref={jsonEditorRef}
        mode={"code" as any}
        theme="ace/theme/textmate"
        value={undefined}
        htmlElementProps={{
          style: {
            height: "calc(100% - 40px)"
          }
        }}
        mainMenuBar={false}
      />
    </>
  );
};
function useSetupProfileResultDisplay(
  jsonEditorRef: React.MutableRefObject<null>
) {
  useEffect(() => {
    const editor = (jsonEditorRef.current as any)?.jsonEditor;
    editor.options.mainMenuBar = false;

    const aceEditor = editor.aceEditor;
    aceEditor.$blockScrolling = Infinity;
    aceEditor.getSession().setMode("ace/mode/aql");
    aceEditor.getSession().setUseWrapMode(false);
    aceEditor.setReadOnly(true);
    aceEditor.setFontSize("12px");
    aceEditor.setShowPrintMargin(false);
  }, [jsonEditorRef]);
}
