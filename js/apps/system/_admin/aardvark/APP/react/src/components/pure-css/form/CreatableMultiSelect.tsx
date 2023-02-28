import React from "react";

import CreatableSelect from "react-select/creatable";
import { components, OptionProps, Props } from "react-select";


export type OptionType = {
  value: string;
  label: string;
};

const Option = (props: OptionProps<OptionType>) => {
  return (
    <div title={props.data.value}>
      <components.Option {...props} />
    </div>
  );
};
const CreatableMultiSelect = (props: Props<OptionType>) => (
  <CreatableSelect
    isMulti
    styles={{
      option: baseStyles => ({
        ...baseStyles,
        overflow: "hidden",
        textOverflow: "ellipsis"
      })
    }}
    components={{
      Option,
      ...props.components,
    }}
    {...props}
  />
);
export default CreatableMultiSelect;
