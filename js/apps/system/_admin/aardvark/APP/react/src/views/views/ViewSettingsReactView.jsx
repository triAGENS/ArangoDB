/* global frontendConfig */

import { cloneDeep, isEqual, uniqueId, times } from 'lodash';
import React, { useEffect, useReducer, useRef, useState } from 'react';
import "./split-pane-styles.css";
import SplitPane from "react-split-pane";
import useSWR from 'swr';
import Textarea from '../../components/pure-css/form/Textarea';
import { Cell, Grid } from '../../components/pure-css/grid';
import { getApiRouteForCurrentDB } from '../../utils/arangoClient';
import { HashRouter, Route, Switch } from 'react-router-dom';
import LinkList from './Components/LinkList';
import { ViewContext } from './constants';
import LinkPropertiesForm from './forms/LinkPropertiesForm';
import CopyFromInput from './forms/inputs/CopyFromInput';
import JsonForm from './forms/JsonForm';
import ToolTip from '../../components/arango/tootip';
import Textbox from '../../components/pure-css/form/Textbox';
import {
  getNumericFieldSetter, getNumericFieldValue, getReducer, isAdminUser as userIsAdmin,
  usePermissions
} from '../../utils/helpers';
import { DeleteButton, SaveButton } from './Actions';
import ConsolidationPolicyForm from './forms/ConsolidationPolicyForm';
import { postProcessor, useNavbar, useView } from './helpers';
import AccordionView from './Components/Accordion/Accordion';

const ViewSettingsReactView = ({ name }) => {

  const PrimarySortContent = () => {
    return (<table>
      <tbody>
      {
        formState.primarySort.map (item =>(
          <tr className="tableRow" id={"row_" + (item.field)}>
            <th className="collectionTh">
              {item.field}:
            </th>
            <th className="collectionTh">
            <Textbox type={'text'} disabled={true} required={false}
              value={item.asc ? "asc" : "desc"} />
            </th>
          </tr>
        ))
      }
      </tbody>
      </table>)
  };

  const StoredValuesContent = () => {
    return (<table>
      <tbody>
      {
        formState.storedValues.map (item =>(
          <tr className="tableRow" id={"row_" + (item.fields)}>
            <th className="collectionTh">
            <Textbox type={'text'} disabled={true} required={false}
              value={item.fields} /> {item.compression}
            </th>
          </tr>
        ))
      }
      </tbody>
      </table>)
  };

  const GeneralContent = () => {
  return( <table>
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
</table>)};

  const panels = times(3, (i) => ({
    key: `panel-${i}`,
    title: `General`,
    content: GeneralContent
  }));
  const initialState = useRef({
    formState: { name },
    formCache: { name },
    renderKey: uniqueId('force_re-render_')
  });
  const view = useView(name);
  const [changed, setChanged] = useState(!!window.sessionStorage.getItem(`${name}-changed`));
  const [state, dispatch] = useReducer(
    getReducer(initialState.current, postProcessor, setChanged, name),
    initialState.current);
  const permissions = usePermissions();
  const [isAdminUser, setIsAdminUser] = useState(false);
  const { data } = useSWR(isAdminUser ? '/view' : null,
    (path) => getApiRouteForCurrentDB().get(path));
  const [views, setViews] = useState([]);

  useEffect(() => {
    initialState.current.formCache = cloneDeep(view);

    dispatch({
      type: 'initFormState',
      formState: view
    });
    dispatch({
      type: 'regenRenderKey'
    });
  }, [view, name]);

  useNavbar(name, isAdminUser, changed, 'Settings');
const [linkName, setLinkName] = useState('')
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

  if (data) {
    if (!isEqual(data.body.result, views)) {
      setViews(data.body.result);
    }
  }

  let jsonFormState = '';
  let jsonRows = 1;
  if (!isAdminUser) {
    jsonFormState = JSON.stringify(formState, null, 4);
    jsonRows = jsonFormState.split('\n').length;
  }

  return <>
      <div id="viewsheader">
        <div style={{
          color: '#717d90',
          fontWeight: 600,
          fontSize: '12.5pt',
          padding: 10
        }}>
          {name}
        </div>
        {
          isAdminUser && views.length
            ? <Cell size={'1'} style={{ paddingLeft: 10 }}>
                <div style={{display: 'flex'}}>
                  <div style={{flex: 1, padding: '10'}}>
                    <CopyFromInput views={views} dispatch={dispatch} formState={formState}/>
                  </div>
                  <div style={{flex: 1, padding: '10'}}>
                    {
                      isAdminUser && changed
                      ?
                        <SaveButton view={formState} oldName={name} menu={'json'} setChanged={setChanged}/>
                      : <SaveButton view={formState} oldName={name} menu={'json'} setChanged={setChanged} disabled/>
                    }
                    <DeleteButton view={formState}
                              modalCid={`modal-content-delete-${formState.globallyUniqueId}`}/>
                  </div>
                </div>
            </Cell>
            : null
        }
      </div>
      <SplitPane
        paneStyle={{ overflow: 'scroll' }}
        defaultSize={parseInt(localStorage.getItem('splitPos'), 10)}
        onChange={(size) => localStorage.setItem('splitPos', size)}
        style={{ borderTop: '2px solid #7a7a7a', paddingTop: '15px', marginTop: '10px', marginLeft: '15px', marginRight: '15px' }}>
        <div style={{ marginRight: '15px' }}>
          <AccordionView
            allowMultipleOpen
            accordionConfig={[
              {
                index: 0,
                content: (
                  <div>
                    <GeneralContent />
                  </div>
                ),
                label: "General",
                testID: "accordionItem1"
              },
              {
                index: 1,
                content: (
                  <div>
                    <ConsolidationPolicyForm formState={formState} dispatch={dispatch}
                                        disabled={!isAdminUser}/>
                  </div>
                ),
                label: "Consolidation Policy",
                testID: "accordionItem2"
              },
              {
                index: 2,
                content: <div><PrimarySortContent /></div>,
                label: "Primary Sort",
                testID: "accordionItem3"
              },
              {
                index: 3,
                content: <div><StoredValuesContent /></div>,
                label: "Stored Values",
                testID: "accordionItem4"
              },
              {
                index: 4,
                content: <div><ViewContext.Provider
                value={{
                  formState,
                  dispatch,
                  isAdminUser,
                  changed,
                  setChanged
                }}
              >
                <HashRouter basename={`view/${name}`} hashType={'noslash'}>
                  <Switch>
                    <Route path={'/:link'}>
                      <LinkPropertiesForm name={name}/>
                    </Route>
                    <Route exact path={'/'}>
                      <LinkList name={'andreasview'}/>
                    </Route>
                  </Switch>
                </HashRouter>
              </ViewContext.Provider></div>,
                label: "Links",
                testID: "accordionItem5",
                defaultActive: true
              }
            ]}
          />
        </div>
        <div style={{ marginLeft: '15px' }}>
          <div id={'modal-dialog'} className={'createModalDialog'} tabIndex={-1} role={'dialog'}
                  aria-labelledby={'myModalLabel'} aria-hidden={'true'} style={{
            marginLeft: 'auto',
            marginRight: 'auto'
          }}>
            <div className="modal-body" style={{ display: 'unset' }} id={'view-json'}>
              <div className={'tab-content'} style={{ display: 'unset' }}>
                <div className="tab-pane tab-pane-modal active" id="JSON">
                  <Grid>
                    {
                      /*
                      isAdminUser && views.length
                        ? <Cell size={'1'} style={{ paddingLeft: 10 }}>
                          <CopyFromInput views={views} dispatch={dispatch} formState={formState}/>
                        </Cell>
                        : null
                      */
                    }

                    <Cell size={'1'}>
                      {
                        isAdminUser
                          ? <JsonForm formState={formState} dispatch={dispatch}
                          renderKey={state.renderKey}/>
                          : <Textarea label={'JSON Dump'} disabled={true} value={jsonFormState}
                                      rows={jsonRows}
                                      style={{ cursor: 'text' }}/>
                      }
                    </Cell>
                  </Grid>
                </div>
              </div>
            </div>
          </div>
        </div>
      </SplitPane>
    
    {
      /*
      <LinkList name={linkName} />
      <LinkPropertiesForm name={linkName}/>
      */
    }
    {
    /*
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
    */
    }
  </>;
};
window.ViewSettingsReactView = ViewSettingsReactView;
