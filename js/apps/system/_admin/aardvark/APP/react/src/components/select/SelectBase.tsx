import React, { useState } from "react";

import Select, {
  components,
  GroupBase,
  OptionProps,
  Props,
  SelectInstance,
  SingleValueProps
} from "react-select";
export type OptionType = {
  value: string;
  label: string;
};

const Option = <IsMulti extends boolean>(
  props: OptionProps<OptionType, IsMulti>
) => {
  const finalChildren =
    typeof props.children === "boolean" ? `${props.children}` : props.children;
  return (
    <div title={props.data.value}>
      <components.Option {...props} children={finalChildren} />
    </div>
  );
};

const SingleValue = <IsMulti extends boolean>(
  props: SingleValueProps<OptionType, IsMulti>
) => {
  const finalChildren =
    typeof props.children === "boolean" ? `${props.children}` : props.children;
  return <components.SingleValue {...props} children={finalChildren} />;
};

export const getSelectBase = <IsMulti extends boolean = false>(
  SelectComponent: Select
) =>
  React.forwardRef(
    (
      props: Props<OptionType, IsMulti> & { normalize?: boolean },
      ref: React.Ref<SelectInstance<OptionType, IsMulti, GroupBase<OptionType>>>
    ) => {
      const { normalize = true, ...rest } = props;
      const [inputValue, setInputValue] = useState("");
      const onInputChange = (inputValue: string) => {
        setInputValue(inputValue.normalize());
      };
      return (
        <SelectComponent
          ref={ref}
          inputValue={
            normalize && !props.onInputChange ? inputValue : props.inputValue
          }
          onInputChange={
            normalize && !props.onInputChange ? onInputChange : undefined
          }
          {...rest}
          menuPortalTarget={document.body}
          components={{
            ...props.components,
            Option,
            SingleValue
          }}
          theme={theme => {
            return {
              ...theme,
              colors: {
                ...theme.colors,
                primary: "var(--green-600)",
                primary75: "var(--gray-400)",
                primary50: "var(--gray-300)",
                primary25: "var(--gray-200)"
              }
            };
          }}
          styles={{
            ...props.styles,
            option: baseStyles => ({
              ...baseStyles,
              overflow: "hidden",
              textOverflow: "ellipsis"
            }),
            control: (baseStyles, inputProps) => ({
              ...baseStyles,
              borderColor: inputProps.isFocused ? "var(--blue-600)" : undefined,
              boxShadow: inputProps.isFocused
                ? "0 0 0 1px var(--blue-600)"
                : undefined,
              ":hover": {
                borderColor: inputProps.isFocused
                  ? "var(--blue-600)"
                  : "var(--gray-300)"
              },
              ...props.styles?.control?.(baseStyles, inputProps)
            }),
            menuPortal: base => ({ ...base, zIndex: 9999 }),
            input: (baseStyles, inputProps) => ({
              ...baseStyles,
              "> input": {
                background: "transparent !important",
                boxShadow: "none !important"
              },
              ...props.styles?.input?.(baseStyles, inputProps)
            })
          }}
        />
      );
    }
  );
