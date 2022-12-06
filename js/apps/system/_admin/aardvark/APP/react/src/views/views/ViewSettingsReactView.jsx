/* global frontendConfig */

import { cloneDeep, times } from 'lodash';
import React, { useEffect, useReducer, useRef, useState } from 'react';
import ToolTip from '../../components/arango/tootip';
import Textbox from '../../components/pure-css/form/Textbox';
import {
  getNumericFieldSetter, getNumericFieldValue, getReducer, isAdminUser as userIsAdmin,
  usePermissions
} from '../../utils/helpers';
import { DeleteButton, SaveButton } from './Actions';
import ConsolidationPolicyForm from './forms/ConsolidationPolicyForm';
import { postProcessor, useNavbar, useView } from './helpers';
import 'semantic-ui-css/semantic.min.css';
import { Container, Header, List } from "semantic-ui-react";
import AccordionExclusive from './Components/AccordionExclusive';

const ViewSettingsReactView = ({ name }) => {

  const generalContent = `<table>
  <tbody>
  <tr className="tableRow" id="row_change-view-name">
    <th className="collectionTh">
      Name*:
    </th>
    <th className="collectionTh">
      <Textbox type={'text'} value={formState.name} onChange={updateName}
              required={true} disabled={nameEditDisabled}/>
    </th>
    <th className="collectionTh">
      <ToolTip
        title={\`The View name (string${nameEditDisabled ? ', immutable' : ''}).\`}
        setArrow={true}
      >
        <span className="arangoicon icon_arangodb_info"></span>
      </ToolTip>
    </th>
  </tr>

  <tr className="tableRow" id="row_change-view-writebufferIdle">
    <th className="collectionTh">
    Writebuffer Idle:
    </th>
    <th className="collectionTh">
      <Textbox type={'number'} disabled={true}
              value={getNumericFieldValue(formState.writebufferIdle)}/>
    </th>
    <th className="collectionTh">
      <ToolTip
        title={\`Maximum number of writers (segments) cached in the pool (default: 64, use 0 to disable, immutable).\`}
        setArrow={true}
      >
        <span className="arangoicon icon_arangodb_info"></span>
      </ToolTip>
    </th>
  </tr>

  <tr className="tableRow" id="row_change-view-writebufferActive">
    <th className="collectionTh">
    Writebuffer Active:
    </th>
    <th className="collectionTh">
      <Textbox type={'number'} disabled={true}
              value={getNumericFieldValue(formState.writebufferActive)}
              onChange={getNumericFieldSetter('writebufferActive', dispatch)}/>
    </th>
    <th className="collectionTh">
      <ToolTip
        title={\`Maximum number of concurrent active writers (segments) that perform a transaction. Other writers (segments) wait till current active writers (segments) finish (default: 0, use 0 to disable, immutable).\`}
        setArrow={true}
      >
        <span className="arangoicon icon_arangodb_info"></span>
      </ToolTip>
    </th>
  </tr>

  <tr className="tableRow" id="row_change-view-writebufferSizeMax">
    <th className="collectionTh">
    Writebuffer Size Max:
    </th>
    <th className="collectionTh">
      <Textbox type={'number'} disabled={true}
              value={getNumericFieldValue(formState.writebufferSizeMax)}
              onChange={getNumericFieldSetter('writebufferSizeMax', dispatch)}/>
    </th>
    <th className="collectionTh">
      <ToolTip
        title={\`Maximum memory byte size per writer (segment) before a writer (segment) flush is triggered. 0 value turns off this limit for any writer (buffer) and data will be flushed periodically based on the value defined for the flush thread (ArangoDB server startup option). 0 value should be used carefully due to high potential memory consumption (default: 33554432, use 0 to disable, immutable).\`}
        setArrow={true}
      >
        <span className="arangoicon icon_arangodb_info"></span>
      </ToolTip>
    </th>
  </tr>

  <tr className="tableRow" id="row_change-view-cleanupIntervalStep">
    <th className="collectionTh">
      Cleanup Interval Step:
    </th>
    <th className="collectionTh">
      <Textbox type={'number'} disabled={!isAdminUser} min={0} step={1}
              value={getNumericFieldValue(formState.cleanupIntervalStep)}
              onChange={getNumericFieldSetter('cleanupIntervalStep', dispatch)}/>
    </th>
    <th className="collectionTh">
      <ToolTip
        title={\`ArangoSearch waits at least this many commits between removing unused files in its data directory.\`}
        setArrow={true}
      >
        <span className="arangoicon icon_arangodb_info"></span>
      </ToolTip>
    </th>
  </tr>

  <tr className="tableRow" id="row_change-view-commitIntervalMsec">
    <th className="collectionTh">
      Commit Interval (msec):
    </th>
    <th className="collectionTh" style={{ width: '100%' }}>
      <Textbox type={'number'} disabled={!isAdminUser} min={0} step={1}
              value={getNumericFieldValue(formState.commitIntervalMsec)}
              onChange={getNumericFieldSetter('commitIntervalMsec', dispatch)}/>
    </th>
    <th className="collectionTh">
      <ToolTip
        title="Wait at least this many milliseconds between committing View data store changes and making documents visible to queries."
        setArrow={true}
      >
        <span className="arangoicon icon_arangodb_info"></span>
      </ToolTip>
    </th>
  </tr>

  <tr className="tableRow" id="row_change-view-consolidationIntervalMsec">
    <th className="collectionTh">
      Consolidation Interval (msec):
    </th>
    <th className="collectionTh">
      <Textbox type={'number'} disabled={!isAdminUser} min={0} step={1}
              value={getNumericFieldValue(formState.consolidationIntervalMsec)}
              onChange={getNumericFieldSetter('consolidationIntervalMsec', dispatch)}/>
    </th>
    <th className="collectionTh">
      <ToolTip
        title="Wait at least this many milliseconds between index segments consolidations."
        setArrow={true}
      >
        <span className="arangoicon icon_arangodb_info"></span>
      </ToolTip>
    </th>
  </tr>
  </tbody>
</table>`;

  const panels = times(3, (i) => ({
    key: `panel-${i}`,
    title: `General`,
    content: generalContent
  }));
  console.log("name in ViewSettingsReactView: ", name);
  const initialState = useRef({
    formState: { name },
    formCache: { name }
  });
  const view = useView(name);
  console.log("view in ViewSettingsReactView: ", view);
  const [changed, setChanged] = useState(!!window.sessionStorage.getItem(`${name}-changed`));
  const [state, dispatch] = useReducer(
    getReducer(initialState.current, postProcessor, setChanged, name),
    initialState.current);
    console.log("state in ViewSettingsReactView: ", state);
    console.log("dispatch in ViewSettingsReactView: ", dispatch);
  const permissions = usePermissions();
  const [isAdminUser, setIsAdminUser] = useState(false);

  useEffect(() => {
    initialState.current.formCache = cloneDeep(view);

    dispatch({
      type: 'initFormState',
      formState: view
    });
  }, [view, name]);

  useNavbar(name, isAdminUser, changed, 'Settings');

  const updateName = (event) => {
    dispatch({
      type: 'setField',
      field: {
        path: 'name',
        value: event.target.value
      }
    });
  };

  const tempIsAdminUser = userIsAdmin(permissions);
  if (tempIsAdminUser !== isAdminUser) { // Prevents an infinite render loop.
    setIsAdminUser(tempIsAdminUser);
  }

  const formState = state.formState;
  const nameEditDisabled = frontendConfig.isCluster || !isAdminUser;

  return <>
    <div style={{
      color: '#717d90',
      fontWeight: 600,
      fontSize: '12.5pt',
      padding: 10
    }}>
      {name}
    </div>
    <AccordionExclusive panels={panels} />
    <div id={'modal-dialog'} className={'createModalDialog'} tabIndex={-1} role={'dialog'}
                aria-labelledby={'myModalLabel'} aria-hidden={'true'} style={{
      width: 1024,
      marginLeft: 'auto',
      marginRight: 'auto'
    }}>
      <div className="modal-body">
        <div className={'tab-content'}>
          <div className="tab-pane tab-pane-modal active" id="General">
            <div style={{
              color: '#717d90',
              fontWeight: 600,
              fontSize: '12.5pt',
              padding: 10,
              borderBottom: '1px solid rgba(0, 0, 0, .3)'
            }}>
              General
            </div>
            <table>
              <tbody>
              <tr className="tableRow" id="row_change-view-name">
                <th className="collectionTh">
                  Name*:
                </th>
                <th className="collectionTh">
                  <Textbox type={'text'} value={formState.name} onChange={updateName}
                          required={true} disabled={nameEditDisabled}/>
                </th>
                <th className="collectionTh">
                  <ToolTip
                    title={`The View name (string${nameEditDisabled ? ', immutable' : ''}).`}
                    setArrow={true}
                  >
                    <span className="arangoicon icon_arangodb_info"></span>
                  </ToolTip>
                </th>
              </tr>

              <tr className="tableRow" id="row_change-view-writebufferIdle">
                <th className="collectionTh">
                Writebuffer Idle:
                </th>
                <th className="collectionTh">
                  <Textbox type={'number'} disabled={true}
                          value={getNumericFieldValue(formState.writebufferIdle)}/>
                </th>
                <th className="collectionTh">
                  <ToolTip
                    title={`Maximum number of writers (segments) cached in the pool (default: 64, use 0 to disable, immutable).`}
                    setArrow={true}
                  >
                    <span className="arangoicon icon_arangodb_info"></span>
                  </ToolTip>
                </th>
              </tr>

              <tr className="tableRow" id="row_change-view-writebufferActive">
                <th className="collectionTh">
                Writebuffer Active:
                </th>
                <th className="collectionTh">
                  <Textbox type={'number'} disabled={true}
                          value={getNumericFieldValue(formState.writebufferActive)}
                          onChange={getNumericFieldSetter('writebufferActive', dispatch)}/>
                </th>
                <th className="collectionTh">
                  <ToolTip
                    title={`Maximum number of concurrent active writers (segments) that perform a transaction. Other writers (segments) wait till current active writers (segments) finish (default: 0, use 0 to disable, immutable).`}
                    setArrow={true}
                  >
                    <span className="arangoicon icon_arangodb_info"></span>
                  </ToolTip>
                </th>
              </tr>

              <tr className="tableRow" id="row_change-view-writebufferSizeMax">
                <th className="collectionTh">
                Writebuffer Size Max:
                </th>
                <th className="collectionTh">
                  <Textbox type={'number'} disabled={true}
                          value={getNumericFieldValue(formState.writebufferSizeMax)}
                          onChange={getNumericFieldSetter('writebufferSizeMax', dispatch)}/>
                </th>
                <th className="collectionTh">
                  <ToolTip
                    title={`Maximum memory byte size per writer (segment) before a writer (segment) flush is triggered. 0 value turns off this limit for any writer (buffer) and data will be flushed periodically based on the value defined for the flush thread (ArangoDB server startup option). 0 value should be used carefully due to high potential memory consumption (default: 33554432, use 0 to disable, immutable).`}
                    setArrow={true}
                  >
                    <span className="arangoicon icon_arangodb_info"></span>
                  </ToolTip>
                </th>
              </tr>

              <tr className="tableRow" id="row_change-view-cleanupIntervalStep">
                <th className="collectionTh">
                  Cleanup Interval Step:
                </th>
                <th className="collectionTh">
                  <Textbox type={'number'} disabled={!isAdminUser} min={0} step={1}
                          value={getNumericFieldValue(formState.cleanupIntervalStep)}
                          onChange={getNumericFieldSetter('cleanupIntervalStep', dispatch)}/>
                </th>
                <th className="collectionTh">
                  <ToolTip
                    title={`ArangoSearch waits at least this many commits between removing unused files in its data directory.`}
                    setArrow={true}
                  >
                    <span className="arangoicon icon_arangodb_info"></span>
                  </ToolTip>
                </th>
              </tr>

              <tr className="tableRow" id="row_change-view-commitIntervalMsec">
                <th className="collectionTh">
                  Commit Interval (msec):
                </th>
                <th className="collectionTh" style={{ width: '100%' }}>
                  <Textbox type={'number'} disabled={!isAdminUser} min={0} step={1}
                          value={getNumericFieldValue(formState.commitIntervalMsec)}
                          onChange={getNumericFieldSetter('commitIntervalMsec', dispatch)}/>
                </th>
                <th className="collectionTh">
                  <ToolTip
                    title="Wait at least this many milliseconds between committing View data store changes and making documents visible to queries."
                    setArrow={true}
                  >
                    <span className="arangoicon icon_arangodb_info"></span>
                  </ToolTip>
                </th>
              </tr>

              <tr className="tableRow" id="row_change-view-consolidationIntervalMsec">
                <th className="collectionTh">
                  Consolidation Interval (msec):
                </th>
                <th className="collectionTh">
                  <Textbox type={'number'} disabled={!isAdminUser} min={0} step={1}
                          value={getNumericFieldValue(formState.consolidationIntervalMsec)}
                          onChange={getNumericFieldSetter('consolidationIntervalMsec', dispatch)}/>
                </th>
                <th className="collectionTh">
                  <ToolTip
                    title="Wait at least this many milliseconds between index segments consolidations."
                    setArrow={true}
                  >
                    <span className="arangoicon icon_arangodb_info"></span>
                  </ToolTip>
                </th>
              </tr>
              </tbody>
            </table>
          </div>

          <div className="tab-pane tab-pane-modal active" id="Consolidation">
            <div style={{
              color: '#717d90',
              fontWeight: 600,
              fontSize: '12.5pt',
              padding: 10,
              borderBottom: '1px solid rgba(0, 0, 0, .3)',
              marginTop: 20
            }}>
              Consolidation Policy
            </div>
            <ConsolidationPolicyForm formState={formState} dispatch={dispatch}
                                    disabled={!isAdminUser}/>
          </div>

          {
            isAdminUser
              ? <div className="tab-pane tab-pane-modal active" id="Actions" style={{
                paddingLeft: 10
              }}>
                {
                  changed
                    ? <SaveButton view={formState} oldName={name} setChanged={setChanged}/>
                    : null
                }
                <DeleteButton view={formState}
                              modalCid={`modal-content-delete-${formState.globallyUniqueId}`}/>
              </div>
              : null
          }
        </div>
      </div>
    </div>
  </>;
};
window.ViewSettingsReactView = ViewSettingsReactView;
