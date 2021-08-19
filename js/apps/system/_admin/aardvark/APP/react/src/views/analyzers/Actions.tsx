import React, { MouseEvent, useState } from 'react';
import Modal, { ModalBody, ModalFooter, ModalHeader } from "../../components/modal/Modal";
import { getApiRouteForCurrentDB } from '../../utils/arangoClient';
import { mutate } from "swr";
import { JsonEditor as Editor } from "jsoneditor-react";
import { noop, pick } from 'lodash';
import { FormState, State } from "./constants";
import { Cell, Grid } from "../../components/pure-css/grid";
import BaseForm from "./forms/BaseForm";
import FeatureForm from "./forms/FeatureForm";
import DelimiterForm from "./forms/DelimiterForm";
import StemForm from "./forms/StemForm";
import NormForm from "./forms/NormForm";
import NGramForm from "./forms/NGramForm";
import TextForm from "./forms/TextForm";
import AqlForm from "./forms/AqlForm";
import GeoJsonForm from "./forms/GeoJsonForm";
import GeoPointForm from "./forms/GeoPointForm";

declare var frontendConfig: { [key: string]: any };
declare var arangoHelper: { [key: string]: any };

interface Analyzer {
  name: string;
}

interface ButtonProps {
  analyzer: Analyzer;
}

const DeleteButton = ({ analyzer }: ButtonProps) => {
  const [show, setShow] = useState(false);
  const [forceDelete, setForceDelete] = useState(false);

  const toggleForce = () => {
    setForceDelete(!forceDelete);
  };

  const handleDelete = async () => {
    try {
      const result = await getApiRouteForCurrentDB().delete(`/analyzer/${analyzer.name}`, { force: forceDelete });

      if (result.body.error) {
        arangoHelper.arangoError('Failure', `Got unexpected server response: ${result.body.errorMessage}`);
      } else {
        arangoHelper.arangoNotification('Success', `Deleted Analyzer: ${analyzer.name}`);
        await mutate('/analyzer');
      }
    } catch (e) {
      arangoHelper.arangoError('Failure', `Got unexpected server response: ${e.message}`);
    }
  };

  return <>
    <button className={'pure-button'} onClick={() => setShow(true)}
            style={{ background: 'transparent' }}>
      <i className={'fa fa-trash-o'}/>
    </button>
    <Modal show={show} setShow={setShow}>
      <ModalHeader title={`Delete Analyzer ${analyzer.name}?`}/>
      <ModalBody>
        <p>
          This Analyzer might be in use. Deleting it will impact the Views using it. Clicking on
          the <b>Delete</b> button will only Delete the Analyzer if it is not in use.
        </p>
        <p>
          Select the <b>Force Delete</b> option below if you want to delete the Analyzer even if it is being
          used.
        </p>
        <label htmlFor={'force-delete'} className="pure-checkbox">
          <input id={'force-delete'} type={'checkbox'} checked={forceDelete} onChange={toggleForce}
                 style={{ width: 'auto' }}/> Force Delete
        </label>
      </ModalBody>
      <ModalFooter>
        <button className="button-close" onClick={() => setShow(false)}>Close</button>
        <button className="button-danger" style={{ float: 'right' }}
                onClick={handleDelete}>Delete
        </button>
      </ModalFooter>
    </Modal>
  </>;
};

const ViewButton = ({ analyzer }: ButtonProps) => {
  const [show, setShow] = useState(false);
  const [showJsonForm, setShowJsonForm] = useState(false);

  const state: State = {
    formState: pick(analyzer, 'name', 'type', 'features', 'properties') as FormState,
    formCache: {},
    show: false,
    showJsonForm: false,
    lockJsonForm: false,
    renderKey: ''
  };

  const handleClick = (event: MouseEvent<HTMLElement>) => {
    event.preventDefault();
    setShow(true);
  };

  const toggleJsonForm = () => {
    setShowJsonForm(!showJsonForm);
  };

  const getForm = (formState: FormState) => {
    switch (formState.type) {
      case 'identity':
        return null;

      case 'delimiter':
        return <DelimiterForm formState={formState} dispatch={noop} disabled={true}/>;

      case 'stem':
        return <StemForm formState={formState} dispatch={noop} disabled={true}/>;

      case 'norm':
        return <NormForm formState={formState} dispatch={noop} disabled={true}/>;

      case 'ngram':
        return <NGramForm formState={formState} dispatch={noop} disabled={true}/>;

      case 'text':
        return <TextForm formState={formState} dispatch={noop} disabled={true}/>;

      case 'aql':
        return <AqlForm formState={formState} dispatch={noop} disabled={true}/>;

      case 'geojson':
        return <GeoJsonForm formState={formState} dispatch={noop} disabled={true}/>;

      case 'geopoint':
        return <GeoPointForm formState={formState} dispatch={noop} disabled={true}/>;

      case 'pipeline':
        return 'Pipeline';
    }
  };

  const formState = state.formState;

  return <>
    <button className={'pure-button'} onClick={handleClick} style={{ background: 'transparent' }}>
      <i className={'fa fa-eye'}/>
    </button>
    <Modal show={show} setShow={setShow}>
      <ModalHeader title={formState.name}>
        <button className={'button-info'} onClick={toggleJsonForm} style={{ float: 'right' }}>
          {showJsonForm ? 'Switch to form view' : 'Switch to code view'}
        </button>
      </ModalHeader>
      <ModalBody>
        <Grid>
          {
            showJsonForm
              ? <Cell size={'1'}>
                <Editor value={formState} mode={'view'}/>
              </Cell>
              : <>
                <Cell size={'1'}>
                  <BaseForm formState={formState} dispatch={noop} disabled={true}/>
                </Cell>
                <Cell size={'1'}>
                  <fieldset>
                    <legend style={{ fontSize: '12pt' }}>Features</legend>
                    <FeatureForm formState={formState} dispatch={noop} disabled={true}/>
                  </fieldset>
                </Cell>

                {
                  formState.type === 'identity' ? null
                    : <Cell size={'1'}>
                      <fieldset>
                        <legend style={{ fontSize: '12pt' }}>Configuration</legend>
                        {getForm(formState)}
                      </fieldset>
                    </Cell>
                }
              </>
          }
        </Grid>
      </ModalBody>
      <ModalFooter>
        <button className="button-close" onClick={() => setShow(false)}>Close</button>
      </ModalFooter>
    </Modal>
  </>;
};

interface ActionProps {
  analyzer: Analyzer;
  permission: string;
}

const Actions = ({ analyzer, permission }: ActionProps) => {
  const isUserDefined = analyzer.name.includes('::');
  const isSameDB = isUserDefined
    ? analyzer.name.split('::')[0] === frontendConfig.db
    : frontendConfig.db === '_system';
  const isAdminUser = permission === 'rw';
  const canDelete = isUserDefined && isSameDB && isAdminUser;

  return <>
    <ViewButton analyzer={analyzer}/>
    {canDelete ? <>&nbsp;<DeleteButton analyzer={analyzer}/></> : null}
  </>;
};

export default Actions;
