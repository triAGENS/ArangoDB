add_library(arango_aql STATIC
  Aggregator.cpp
  AqlCall.cpp
  AqlCallList.cpp
  AqlCallSet.cpp
  AqlCallStack.cpp
  AqlExecuteResult.cpp
  AqlFunctionFeature.cpp
  AqlFunctionsInternalCache.cpp
  AqlItemBlock.cpp
  AqlItemBlockInputRange.cpp
  AqlItemBlockManager.cpp
  AqlItemBlockUtils.cpp
  AqlTransaction.cpp
  AqlValue.cpp
  AqlValueGroup.cpp
  AqlValueMaterializer.cpp
  Arithmetic.cpp
  Ast.cpp
  AstHelper.cpp
  AstNode.cpp
  AstResources.cpp
  AsyncPrefetchSlotsManager.cpp
  AttributeAccessor.cpp
  AttributeNamePath.cpp
  BindParameters.cpp
  BlocksWithClients.cpp
  CalculationNodeVarFinder.cpp
  ClusterQuery.cpp
  CollectOptions.cpp
  Collection.cpp
  CollectionAccess.cpp
  Collections.cpp
  Condition.cpp
  ConditionFinder.cpp
  ConstFetcher.cpp
  CostEstimate.cpp
  DependencyProxy.cpp
  DistributeClientBlock.cpp
  DocumentExpressionContext.cpp
  DocumentProducingHelper.cpp
  EdgeConditionBuilder.cpp
  EdgeConditionBuilderContainer.cpp
  EngineInfoContainerCoordinator.cpp
  EngineInfoContainerDBServerServerBased.cpp
  ExecutionBlock.cpp
  ExecutionEngine.cpp
  ExecutionNodeId.cpp
  ExecutionPlan.cpp
  ExecutionState.cpp
  ExecutionStats.cpp
  ExecutorExpressionContext.cpp
  Expression.cpp
  FixedVarExpressionContext.cpp
  Function.cpp
  Functions.cpp
  InAndOutRowExpressionContext.cpp
  IndexHint.cpp
  IndexJoinStrategy.cpp
  InputAqlItemRow.cpp
  InsertModifier.cpp
  LateMaterializedExpressionContext.cpp
  LazyConditions.cpp
  LimitStats.cpp
  ModificationExecutorFlags.h
  ModificationOptions.cpp
  MultiAqlItemBlockInputRange.cpp
  MultiDependencySingleRowFetcher.cpp
  NodeFinder.cpp
  NonConstExpression.cpp
  NonConstExpressionContainer.cpp
  Optimizer.cpp
  OptimizerRules.cpp
  OptimizerRulesFeature.cpp
  OptimizerUtils.cpp
  OutputAqlItemRow.cpp
  Parser.cpp
  Projections.cpp
  PruneExpressionEvaluator.cpp
  Quantifier.cpp
  Query.cpp
  QueryAborter.cpp
  QueryCache.cpp
  QueryContext.cpp
  QueryExecutionState.cpp
  QueryExpressionContext.cpp
  QueryList.cpp
  QueryMethods.cpp
  QueryOptions.cpp
  QueryProfile.cpp
  QueryRegistry.cpp
  QuerySnippet.cpp
  QueryString.cpp
  QueryWarnings.cpp
  Range.cpp
  RegisterId.cpp
  RegisterInfos.cpp
  RegisterPlan.cpp
  RemoveModifier.cpp
  RestAqlHandler.cpp
  Scopes.cpp
  ShadowAqlItemRow.cpp
  ShardLocking.cpp
  SharedAqlItemBlockPtr.cpp
  SharedQueryState.cpp
  ShortStringStorage.cpp
  SimpleModifier.cpp
  SingleRowFetcher.cpp
  SkipResult.cpp
  SortCondition.cpp
  SortElement.cpp
  SortInformation.cpp
  SortRegister.cpp
  SortedRowsStorageBackendMemory.cpp
  SortedRowsStorageBackendStaged.cpp
  Timing.cpp
  TraversalConditionFinder.cpp
  TraverserEngineShardLists.cpp
  UpdateReplaceModifier.cpp
  UpsertModifier.cpp
  VarUsageFinder.cpp
  Variable.cpp
  VariableGenerator.cpp
  grammar.cpp
  tokens.cpp
)

if (USE_V8) 
  target_sources(arango_aql PRIVATE
    V8ErrorHandler.cpp)
endif()

add_subdirectory(ExecutionNode)
add_subdirectory(Executor)
add_subdirectory(Function)
add_subdirectory(Optimizer)

target_link_libraries(arango_aql
  arango_geo
  arango_graph
  arango_indexes
  arango_iresearch
  arango_network
  immer)

target_include_directories(arango_aql PRIVATE
  "${PROJECT_SOURCE_DIR}/arangod"
  "${PROJECT_SOURCE_DIR}/${ENTERPRISE_INCLUDE_DIR}")

################################################################################
## BISON/FLEX
################################################################################

#note that we check - in the generated FLEX / BISON files, therefore they are
#generate inside the source tree

if (USE_MAINTAINER_MODE AND NOT MSVC)
  set_directory_properties(PROPERTIES CLEAN_NO_CUSTOM "On")

  add_custom_command(
    OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/tokens.cpp
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMAND ${FLEX_EXECUTABLE} -L -otokens.cpp tokens.ll
    MAIN_DEPENDENCY ${CMAKE_CURRENT_SOURCE_DIR}/tokens.ll
    VERBATIM
  )

  add_custom_command(
    OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/grammar.cpp
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMAND ${CMAKE_SOURCE_DIR}/utils/bison-c.sh
      ${BISON_EXECUTABLE} grammar.cpp grammar.y
    MAIN_DEPENDENCY ${CMAKE_CURRENT_SOURCE_DIR}/grammar.y
    VERBATIM
  )
endif ()

if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  set_source_files_properties(tokens.cpp PROPERTIES COMPILE_FLAGS -Wno-sign-compare)
endif ()
if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  set_source_files_properties(grammar.cpp PROPERTIES COMPILE_FLAGS -Wno-unused-but-set-variable)
endif ()
