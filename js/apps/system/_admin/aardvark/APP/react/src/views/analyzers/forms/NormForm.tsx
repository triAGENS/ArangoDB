import React from "react";
import { FormProps } from "../constants";
import CaseInput from "./inputs/CaseInput";
import LocaleInput from "./inputs/LocaleInput";
import { Cell, Grid } from "../../../components/pure-css/grid";
import AccentInput from "./inputs/AccentInput";

const NormForm = ({ formState, dispatch, disabled }: FormProps) =>
  <Grid>
    <Cell size={'1-2'}>
      <LocaleInput formState={formState} dispatch={dispatch} disabled={disabled}/>
    </Cell>

    <Cell size={'1-2'}>
      <CaseInput formState={formState} dispatch={dispatch} disabled={disabled}/>
    </Cell>

    <Cell size={'1-3'}>
      <AccentInput formState={formState} dispatch={dispatch} disabled={disabled}/>
    </Cell>
  </Grid>;

export default NormForm;
