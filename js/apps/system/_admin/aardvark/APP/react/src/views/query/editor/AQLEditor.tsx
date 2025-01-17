import { Global } from "@emotion/react";
import { JsonEditor } from "jsoneditor-react";
import React, { useEffect } from "react";
import { useQueryContext } from "../QueryContextProvider";

export const AQLEditor = ({
  value,
  onChange,
  isPreview,
  autoFocus,
  resetEditor
}: {
  value?: string;
  onChange?: (value: string) => void;
  isPreview?: boolean;
  resetEditor?: boolean;
  autoFocus?: boolean;
}) => {
  const { aqlJsonEditorRef } = useQueryContext();

  useEffect(() => {
    const editor = (aqlJsonEditorRef.current as any)?.jsonEditor;
    editor.options.onChangeText = (value: string) => {
      onChange?.(value);
    };

    /**
     * directly call aceEditor.setValue to avoid cursor jump.
     * This can't be called on every value change because that
     * will destroy the undo stack
     */
    editor.aceEditor.setValue(value, 1);
    /**
     * disabled because we are using the value as 'defaultValue',
     * and onChange function update can be ignored
     * */
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [resetEditor]);

  /** set to readOnly when in preview mode */
  useEffect(() => {
    const editor = (aqlJsonEditorRef.current as any)?.jsonEditor;
    if (isPreview) {
      editor.aceEditor.setReadOnly(true);
      editor.aceEditor.setValue(value, 1);
    }
    // disabled because we don't need the ref in deps
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [isPreview, value]);

  useEffect(() => {
    const editor = (aqlJsonEditorRef.current as any)?.jsonEditor;
    if (autoFocus) {
      editor.aceEditor.focus();
    }
    // disabled because we don't need the ref in deps
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [autoFocus]);
  useSetupAQLEditor(aqlJsonEditorRef);
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
        ref={aqlJsonEditorRef}
        mode={"code"}
        onChange={onChange}
        value={undefined}
        htmlElementProps={{
          style: {
            height: "100%",
            width: "100%"
          }
        }}
        mainMenuBar={false}
      />
    </>
  );
};

const useSetupAQLEditor = (aqlJsonEditorRef: React.MutableRefObject<null>) => {
  useEffect(() => {
    const editor = (aqlJsonEditorRef.current as any)?.jsonEditor;
    const aceEditor = editor.aceEditor;
    aceEditor.$blockScrolling = Infinity;
    aceEditor.getSession().setMode("ace/mode/aql");
    aceEditor.setTheme("ace/theme/textmate");
    aceEditor.getSession().setUseWrapMode(true);
    aceEditor.setFontSize("10pt");
    aceEditor.setShowPrintMargin(false);
  }, [aqlJsonEditorRef]);
  useSetupKeyboardShortcuts(aqlJsonEditorRef);
};

const useSetupKeyboardShortcuts = (
  aqlJsonEditorRef: React.MutableRefObject<null>
) => {
  const {
    onExecute,
    queryBindParams,
    onOpenSaveAsModal,
    bindVariablesJsonEditorRef,
    queryOptions,
    disabledRules
  } = useQueryContext();
  useEffect(() => {
    const editor = (aqlJsonEditorRef.current as any)?.jsonEditor;
    const aceEditor = editor.aceEditor;
    const bindVarsAceEditor = (bindVariablesJsonEditorRef.current as any)
      ?.jsonEditor?.aceEditor;
    aceEditor.commands.addCommand({
      name: "togglecomment",
      bindKey: {
        win: "Ctrl-Shift-C",
        linux: "Ctrl-Shift-C",
        mac: "Command-Shift-C"
      },
      exec: function (editor: any) {
        editor.toggleCommentLines();
      },
      multiSelectAction: "forEach"
    });

    aceEditor.commands.addCommand({
      name: "increaseFontSize",
      bindKey: {
        win: "Shift-Alt-Up",
        linux: "Shift-Alt-Up",
        mac: "Shift-Alt-Up"
      },
      exec: function () {
        let newSize = `${
          parseInt(aceEditor.getFontSize().match(/\d+/)[0], 10) + 1
        }`;
        newSize = `${newSize}pt`;
        aceEditor.setFontSize(newSize);
        bindVarsAceEditor?.setFontSize(newSize);
      },
      multiSelectAction: "forEach"
    });

    aceEditor.commands.addCommand({
      name: "decreaseFontSize",
      bindKey: {
        win: "Shift-Alt-Down",
        linux: "Shift-Alt-Down",
        mac: "Shift-Alt-Down"
      },
      exec: function () {
        var newSize = `${
          parseInt(aceEditor.getFontSize().match(/\d+/)[0], 10) - 1
        }`;
        newSize = `${newSize}pt`;
        aceEditor.setFontSize(newSize);
        bindVarsAceEditor?.setFontSize(newSize);
      },
      multiSelectAction: "forEach"
    });

    aceEditor.commands.addCommand({
      name: "executeSelectedQuery",
      bindKey: {
        win: "Ctrl-Alt-Return",
        mac: "Command-Alt-Return",
        linux: "Ctrl-Alt-Return"
      },
      exec: function (editor: any) {
        const selectedText = editor.getSelectedText();
        onExecute({
          queryValue: selectedText,
          queryBindParams,
          queryOptions,
          disabledRules
        });
      }
    });

    aceEditor.commands.addCommand({
      name: "saveQuery",
      bindKey: {
        win: "Ctrl-Shift-S",
        mac: "Command-Shift-S",
        linux: "Ctrl-Shift-S"
      },
      exec: function () {
        onOpenSaveAsModal();
      }
    });

    aceEditor.commands.addCommand({
      name: "togglecomment",
      bindKey: {
        win: "Ctrl-Shift-C",
        linux: "Ctrl-Shift-C",
        mac: "Command-Shift-C"
      },
      exec: function (editor: any) {
        editor.toggleCommentLines();
      },
      multiSelectAction: "forEach"
    });
  }, [
    aqlJsonEditorRef,
    queryBindParams,
    onExecute,
    onOpenSaveAsModal,
    bindVariablesJsonEditorRef,
    queryOptions,
    disabledRules
  ]);
};
