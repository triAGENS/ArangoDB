import { FormLabel, Spacer } from "@chakra-ui/react";
import { useField, useFormikContext } from "formik";
import React, { useEffect, useState } from "react";
import useSWR from "swr";
import { SelectControl } from "../../../../../components/form/SelectControl";
import { OptionType } from "../../../../../components/select/SelectBase";
import { getApiRouteForCurrentDB } from "../../../../../utils/arangoClient";
import { IndexFormFieldProps } from "../IndexFormField";
import { InvertedIndexValuesType } from "./useCreateInvertedIndex";

export const InvertedIndexAnalyzerDropdown = ({
  field,
  autoFocus,
  dependentFieldName = "features"
}: {
  field: IndexFormFieldProps;
  autoFocus: boolean;
  dependentFieldName?: string;
}) => {
  const [options, setOptions] = useState<OptionType[]>([]);
  const { data: analyzersResponse } = useSWR("/analyzer", path =>
    getApiRouteForCurrentDB().get(path)
  );
  const analyzersList = analyzersResponse?.body.result as {
    name: string;
    features: string[];
  }[];
  const { setFieldValue } = useFormikContext<InvertedIndexValuesType>();
  const [formikField] = useField(field.name);
  const fieldValue = formikField.value;
  React.useEffect(() => {
    if (field.isDisabled) {
      return;
    }
    if (fieldValue) {
      const features = analyzersList?.find(
        analyzer => analyzer.name === fieldValue
      )?.features;
      setFieldValue(dependentFieldName, features);
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [analyzersList, fieldValue, dependentFieldName]);

  useEffect(() => {
    if (analyzersList) {
      const tempOptions = analyzersList.map(option => {
        return {
          value: option.name,
          label: option.name
        };
      });
      setOptions(tempOptions);
    }
  }, [analyzersList]);
  return (
    <>
      <FormLabel htmlFor={field.name}>{field.label}</FormLabel>
      <SelectControl
        isDisabled={field.isDisabled}
        selectProps={{
          autoFocus,
          options: options,
          isClearable: true
        }}
        isRequired={field.isRequired}
        name={field.name}
      />
      <Spacer />
    </>
  );
};
