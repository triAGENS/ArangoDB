import { ClassNames } from "@emotion/react";
import { JsonEditor, JsonEditorProps } from "jsoneditor-react";
import React, { useEffect, useRef } from "react";

/**
 * used to remove the schema on unmount, to avoid issues in next usage
 */
const useResetSchema = ({ schema, ajv }: { schema: any; ajv?: any }) => {
  useEffect(() => {
    return () => {
      ajv?.removeSchema(schema.$id);
    };
  }, [schema, ajv]);
};

type ControlledJSONEditorProps<T> = Omit<JsonEditorProps<T>, "value"> & {
  value?: T;
  isDisabled?: boolean;
  isReadOnly?: boolean;
  defaultValue?: T;
};

/**
 * This declaration extends React type definitions to properly handle generic types with forwardRef.
 * It's necessary because the default type definitions don't fully support combining generic type parameters
 * with forwarded refs. This allows us to maintain type safety while forwarding refs to the JSONEditor component.
 */
declare module "react" {
  function forwardRef<T, P = {}>(
    render: (props: P, ref: React.Ref<T>) => React.ReactElement | null
  ): (props: P & React.RefAttributes<T>) => React.ReactElement | null;
}

export const ControlledJSONEditor = React.forwardRef(
  <T,>(
    {
      value,
      onChange,
      schema,
      isDisabled,
      isReadOnly,
      htmlElementProps,
      mainMenuBar,
      defaultValue,
      ...rest
    }: ControlledJSONEditorProps<T>,
    ref: React.ForwardedRef<JsonEditor>
  ) => {
    const jsonEditorRef = useRef<JsonEditor | undefined>();
    useResetSchema({ schema, ajv: rest.ajv });
    useSetupReadOnly({ jsonEditorRef, isDisabled, isReadOnly });
    useSetupValueUpdate({ jsonEditorRef, value });
    useSetupDefaultValueUpdate({ jsonEditorRef, defaultValue });
    const setRef = (node: JsonEditor | null) => {
      if (node) {
        jsonEditorRef.current = node;
      }
      if (typeof ref === "function") {
        ref(node);
      } else if (ref) {
        ref.current = node;
      }
    };
    return (
      <ClassNames>
        {({ css }) => (
          <JsonEditor
            schema={schema}
            ref={setRef}
            value={value || {}}
            onChange={onChange}
            htmlElementProps={{
              ...htmlElementProps,
              className: isDisabled
                ? css`
                    opacity: 0.6;
                  `
                : ""
            }}
            {...rest}
            mainMenuBar={mainMenuBar}
          />
        )}
      </ClassNames>
    );
  }
);

const useSetupReadOnly = ({
  jsonEditorRef,
  isDisabled,
  isReadOnly
}: {
  jsonEditorRef?: React.MutableRefObject<JsonEditor | undefined>;
  isDisabled: boolean | undefined;
  isReadOnly: boolean | undefined;
}) => {
  useEffect(() => {
    const editor = jsonEditorRef?.current?.jsonEditor;
    const ace = editor?.aceEditor;
    if (!editor || !ace) return;
    ace.setReadOnly(isDisabled || isReadOnly);
  }, [isDisabled, isReadOnly, jsonEditorRef]);
};

const useSetupDefaultValueUpdate = ({
  jsonEditorRef,
  defaultValue
}: {
  jsonEditorRef?: React.MutableRefObject<JsonEditor | undefined>;
  defaultValue: any;
}) => {
  useEffect(() => {
    const editor = jsonEditorRef?.current?.jsonEditor;
    if (editor && defaultValue) {
      editor.update(defaultValue);
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [jsonEditorRef]);
};

const useSetupValueUpdate = ({
  jsonEditorRef,
  value
}: {
  jsonEditorRef: React.MutableRefObject<JsonEditor | undefined>;
  value: any;
}) => {
  useEffect(() => {
    const editor = jsonEditorRef?.current?.jsonEditor;
    if (
      editor &&
      value &&
      JSON.stringify(value) !== JSON.stringify(editor.get())
    ) {
      editor.update(value);
    }
  }, [jsonEditorRef, value]);
};
