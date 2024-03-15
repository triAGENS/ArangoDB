import { Divider, Flex, Grid, Stack, Switch, Text } from "@chakra-ui/react";
import { useField } from "formik";
import React, { useEffect } from "react";
import { InputControl } from "../../../components/form/InputControl";
import { SelectControl } from "../../../components/form/SelectControl";
import { ExternalLink } from "../../../components/link/ExternalLink";
import { AnalyzerTypes } from "../Analyzer.types";
import { useAnalyzersContext } from "../AnalyzersContext";
import { ANALYZER_TYPE_OPTIONS } from "../AnalyzersHelpers";
import { useReinitializeForm } from "../useReinitializeForm";
import { AnalyzerTypeForm } from "./AnalyzerTypeForm";

export const AddAnalyzerForm = ({
  initialFocusRef
}: {
  initialFocusRef?: React.RefObject<HTMLInputElement>;
}) => {
  const [analyzerTypeField] = useField<AnalyzerTypes>("type");
  const { isFormDisabled: isDisabled } = useAnalyzersContext();
  const analyzerTypeValue = analyzerTypeField.value;
  const version = window.versionHelper.getDocuVersion();
  const { onReinitialize } = useReinitializeForm();
  useEffect(() => {
    onReinitialize();

    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [analyzerTypeField.value]);
  return (
    <Grid templateColumns={"1fr 1fr"} gap="6">
      <Stack>
        <Text>Basic</Text>
        <Divider borderColor="gray.400" />
        <Grid templateColumns={"1fr 1fr"} columnGap="4" alignItems="start">
          <InputControl
            ref={initialFocusRef}
            isDisabled={isDisabled}
            name="name"
            label="Analyzer name"
          />
          <Flex alignItems="flex-end">
            <SelectControl
              isDisabled={isDisabled}
              name="type"
              label="Analyzer type"
              selectProps={{
                options: ANALYZER_TYPE_OPTIONS
              }}
            />
            <ExternalLink
              marginLeft="2"
              marginBottom="2"
              href={`https://docs.arangodb.com/${version}/index-and-search/analyzers/#${analyzerTypeValue}`}
            >
              Docs
            </ExternalLink>
          </Flex>
        </Grid>
      </Stack>
      <Stack>
        <Text>Features</Text>
        <Divider borderColor="gray.400" />
        <Grid templateColumns={"1fr 1fr 1fr"} rowGap="4">
          <FeatureSwitch name="frequency" />
          <FeatureSwitch name="norm" />
          <FeatureSwitch name="position" />
          <FeatureSwitch name="offset" />
        </Grid>
      </Stack>
      <AnalyzerTypeForm analyzerType={analyzerTypeValue} />
    </Grid>
  );
};

const FEATURE_NAME_TO_LABEL_MAP = {
  frequency: "Frequency",
  norm: "Norm",
  position: "Position",
  offset: "Offset"
};

const FeatureSwitch = ({ name }: { name: string }) => {
  const [featuresField, , featuresFieldHelper] = useField("features");
  const { isFormDisabled: isDisabled } = useAnalyzersContext();

  return (
    <Switch
      display="flex"
      alignItems="center"
      isDisabled={isDisabled}
      isChecked={featuresField.value.includes(name)}
      onChange={() => {
        if (featuresField.value.includes(name)) {
          featuresFieldHelper.setValue(
            featuresField.value.filter((feature: string) => feature !== name)
          );
          return;
        }
        featuresFieldHelper.setValue([...featuresField.value, name]);
      }}
    >
      {
        FEATURE_NAME_TO_LABEL_MAP[
          name as keyof typeof FEATURE_NAME_TO_LABEL_MAP
        ]
      }
    </Switch>
  );
};
