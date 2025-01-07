import { InputControl, SingleSelectControl } from "@arangodb/ui";
import { Box, Flex, Grid, Spacer } from "@chakra-ui/react";
import { AnalyzerDescription } from "arangojs/analyzer";
import { useField } from "formik";
import React from "react";
import { useAnalyzersContext } from "../../AnalyzersContext";
import { ANALYZER_TYPE_OPTIONS } from "../../AnalyzersHelpers";
import { AnalyzerTypeForm } from "../AnalyzerTypeForm";

export const WildcardConfig = ({
  basePropertiesPath
}: {
  basePropertiesPath: string;
}) => {
  const { isFormDisabled: isDisabled } = useAnalyzersContext();

  return (
    <>
      <Grid templateColumns={"1fr 1fr"} columnGap="4">
        <InputControl
          isDisabled={isDisabled}
          name={`${basePropertiesPath}.ngramSize`}
          label="Ngram size"
          inputProps={{ type: "number" }}
        />
      </Grid>
      <AnalyzerField name={`${basePropertiesPath}.analyzer`} />
    </>
  );
};

const SUPPORTED_ANALYZER_TYPE_OPTIONS = ANALYZER_TYPE_OPTIONS.filter(
  option => !["minhash", "pipeline", "wildcard"].includes(option.value)
);
const AnalyzerField = ({ name }: { name: string }) => {
  const { isFormDisabled: isDisabled } = useAnalyzersContext();

  const [field] =
    useField<Omit<AnalyzerDescription, "name" | "features">>(name);
  return (
    <Flex direction="column" gap="2" padding="2" backgroundColor="gray.100">
      <Grid templateColumns={"1fr 1fr"} columnGap="4">
        <SingleSelectControl
          isDisabled={isDisabled}
          name={`${name}.type`}
          label="Analyzer type"
          selectProps={{
            options: SUPPORTED_ANALYZER_TYPE_OPTIONS
          }}
        />
        <Spacer />
      </Grid>
      <Box paddingX="4">
        <AnalyzerTypeForm
          basePropertiesPath={`${name}.properties`}
          analyzerType={field.value?.type}
        />
      </Box>
    </Flex>
  );
};
