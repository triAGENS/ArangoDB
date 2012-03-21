static string JS_common_bootstrap_errors = 
  "////////////////////////////////////////////////////////////////////////////////\n"
  "/// @brief auto-generated file generated from errors.dat\n"
  "////////////////////////////////////////////////////////////////////////////////\n"
  "\n"
  "var internal = require(\"internal\");\n"
  "\n"
  "ModuleCache[\"/internal\"].exports.errors = {\n"
  "  \"ERROR_NO_ERROR\"               : { \"code\" : 0, \"message\" : \"no error\" }, \n"
  "  \"ERROR_FAILED\"                 : { \"code\" : 1, \"message\" : \"failed\" }, \n"
  "  \"ERROR_SYS_ERROR\"              : { \"code\" : 2, \"message\" : \"system error\" }, \n"
  "  \"ERROR_OUT_OF_MEMORY\"          : { \"code\" : 3, \"message\" : \"out of memory\" }, \n"
  "  \"ERROR_INTERNAL\"               : { \"code\" : 4, \"message\" : \"internal error\" }, \n"
  "  \"ERROR_ILLEGAL_NUMBER\"         : { \"code\" : 5, \"message\" : \"illegal number\" }, \n"
  "  \"ERROR_NUMERIC_OVERFLOW\"       : { \"code\" : 6, \"message\" : \"numeric overflow\" }, \n"
  "  \"ERROR_ILLEGAL_OPTION\"         : { \"code\" : 7, \"message\" : \"illegal option\" }, \n"
  "  \"ERROR_DEAD_PID\"               : { \"code\" : 8, \"message\" : \"dead process identifier\" }, \n"
  "  \"ERROR_NOT_IMPLEMENTED\"        : { \"code\" : 9, \"message\" : \"not implemented\" }, \n"
  "  \"ERROR_HTTP_BAD_PARAMETER\"     : { \"code\" : 400, \"message\" : \"bad parameter\" }, \n"
  "  \"ERROR_HTTP_METHOD_NOT_ALLOWED\" : { \"code\" : 405, \"message\" : \"method not supported\" }, \n"
  "  \"ERROR_HTTP_CORRUPTED_JSON\"    : { \"code\" : 600, \"message\" : \"invalid JSON object\" }, \n"
  "  \"ERROR_HTTP_SUPERFLUOUS_SUFFICES\" : { \"code\" : 601, \"message\" : \"superfluous URL suffices\" }, \n"
  "  \"ERROR_AVOCADO_ILLEGAL_STATE\"  : { \"code\" : 1000, \"message\" : \"illegal state\" }, \n"
  "  \"ERROR_AVOCADO_SHAPER_FAILED\"  : { \"code\" : 1001, \"message\" : \"illegal shaper\" }, \n"
  "  \"ERROR_AVOCADO_DATAFILE_SEALED\" : { \"code\" : 1002, \"message\" : \"datafile sealed\" }, \n"
  "  \"ERROR_AVOCADO_UNKNOWN_COLLECTION_TYPE\" : { \"code\" : 1003, \"message\" : \"unknown type\" }, \n"
  "  \"ERROR_AVOCADO_READ_ONLY\"      : { \"code\" : 1004, \"message\" : \"ready only\" }, \n"
  "  \"ERROR_AVOCADO_CORRUPTED_DATAFILE\" : { \"code\" : 1100, \"message\" : \"corrupted datafile\" }, \n"
  "  \"ERROR_AVOCADO_ILLEGAL_PARAMETER_FILE\" : { \"code\" : 1101, \"message\" : \"illegal parameter file\" }, \n"
  "  \"ERROR_AVOCADO_CORRUPTED_COLLECTION\" : { \"code\" : 1102, \"message\" : \"corrupted collection\" }, \n"
  "  \"ERROR_AVOCADO_MMAP_FAILED\"    : { \"code\" : 1103, \"message\" : \"mmap failed\" }, \n"
  "  \"ERROR_AVOCADO_MSYNC_FAILED\"   : { \"code\" : 1104, \"message\" : \"msync failed\" }, \n"
  "  \"ERROR_AVOCADO_NO_JOURNAL\"     : { \"code\" : 1105, \"message\" : \"no journal\" }, \n"
  "  \"ERROR_AVOCADO_DATAFILE_ALREADY_EXISTS\" : { \"code\" : 1106, \"message\" : \"cannot rename because file ready exists\" }, \n"
  "  \"ERROR_AVOCADO_FILESYSTEM_FULL\" : { \"code\" : 1107, \"message\" : \"filesystem full\" }, \n"
  "  \"ERROR_AVOCADO_CONFLICT\"       : { \"code\" : 1200, \"message\" : \"conflict\" }, \n"
  "  \"ERROR_AVOCADO_WRONG_VOCBASE_PATH\" : { \"code\" : 1201, \"message\" : \"wrong path for database\" }, \n"
  "  \"ERROR_AVOCADO_DOCUMENT_NOT_FOUND\" : { \"code\" : 1202, \"message\" : \"document not found\" }, \n"
  "  \"ERROR_AVOCADO_COLLECTION_NOT_FOUND\" : { \"code\" : 1203, \"message\" : \"collection not found\" }, \n"
  "  \"ERROR_AVOCADO_COLLECTION_PARAMETER_MISSING\" : { \"code\" : 1204, \"message\" : \"parameter 'collection' not found\" }, \n"
  "  \"ERROR_AVOCADO_DOCUMENT_HANDLE_BAD\" : { \"code\" : 1205, \"message\" : \"illegal document handle\" }, \n"
  "  \"ERROR_AVOCADO_MAXIMAL_SIZE_TOO_SMALL\" : { \"code\" : 1206, \"message\" : \"maixaml size of journal too small\" }, \n"
  "  \"ERROR_AVOCADO_DATAFILE_FULL\"  : { \"code\" : 1300, \"message\" : \"datafile full\" }, \n"
  "  \"ERROR_QUERY_KILLED\"           : { \"code\" : 1500, \"message\" : \"query killed\" }, \n"
  "  \"ERROR_QUERY_PARSE\"            : { \"code\" : 1501, \"message\" : \"parse error: %s\" }, \n"
  "  \"ERROR_QUERY_EMPTY\"            : { \"code\" : 1502, \"message\" : \"query is empty\" }, \n"
  "  \"ERROR_QUERY_SPECIFICATION_INVALID\" : { \"code\" : 1503, \"message\" : \"query specification invalid\" }, \n"
  "  \"ERROR_QUERY_NUMBER_OUT_OF_RANGE\" : { \"code\" : 1504, \"message\" : \"number '%s' is out of range\" }, \n"
  "  \"ERROR_QUERY_LIMIT_VALUE_OUT_OF_RANGE\" : { \"code\" : 1521, \"message\" : \"limit value '%s' is out of range\" }, \n"
  "  \"ERROR_QUERY_TOO_MANY_JOINS\"   : { \"code\" : 1505, \"message\" : \"too many joins.\" }, \n"
  "  \"ERROR_QUERY_COLLECTION_NAME_INVALID\" : { \"code\" : 1506, \"message\" : \"collection name '%s' is invalid\" }, \n"
  "  \"ERROR_QUERY_COLLECTION_ALIAS_INVALID\" : { \"code\" : 1507, \"message\" : \"collection alias '%s' is invalid\" }, \n"
  "  \"ERROR_QUERY_COLLECTION_ALIAS_REDECLARED\" : { \"code\" : 1508, \"message\" : \"collection alias '%s' is declared multiple times in the same query\" }, \n"
  "  \"ERROR_QUERY_COLLECTION_ALIAS_UNDECLARED\" : { \"code\" : 1509, \"message\" : \"collection alias '%s' is used but was not declared in the from clause\" }, \n"
  "  \"ERROR_QUERY_COLLECTION_NOT_FOUND\" : { \"code\" : 1510, \"message\" : \"unable to open collection '%s'\" }, \n"
  "  \"ERROR_QUERY_GEO_RESTRICTION_INVALID\" : { \"code\" : 1511, \"message\" : \"geo restriction for alias '%s' is invalid\" }, \n"
  "  \"ERROR_QUERY_GEO_INDEX_MISSING\" : { \"code\" : 1512, \"message\" : \"no suitable geo index found for geo restriction on '%s'\" }, \n"
  "  \"ERROR_QUERY_BIND_PARAMETER_MISSING\" : { \"code\" : 1513, \"message\" : \"no value specified for declared bind parameter '%s'\" }, \n"
  "  \"ERROR_QUERY_BIND_PARAMETER_REDECLARED\" : { \"code\" : 1514, \"message\" : \"value for bind parameter '%s' is declared multiple times\" }, \n"
  "  \"ERROR_QUERY_BIND_PARAMETER_UNDECLARED\" : { \"code\" : 1515, \"message\" : \"bind parameter '%s' was not declared in the query\" }, \n"
  "  \"ERROR_QUERY_BIND_PARAMETER_VALUE_INVALID\" : { \"code\" : 1516, \"message\" : \"invalid value for bind parameter '%s'\" }, \n"
  "  \"ERROR_QUERY_BIND_PARAMETER_NUMBER_OUT_OF_RANGE\" : { \"code\" : 1517, \"message\" : \"bind parameter number '%s' out of range\" }, \n"
  "  \"ERROR_CURSOR_NOT_FOUND\"       : { \"code\" : 1600, \"message\" : \"cursor not found\" }, \n"
  "  \"ERROR_SESSION_USERHANDLER_URL_INVALID\" : { \"code\" : 1700, \"message\" : \"expecting <prefix>/user/<username>\" }, \n"
  "  \"ERROR_SESSION_USERHANDLER_CANNOT_CREATE_USER\" : { \"code\" : 1701, \"message\" : \"cannot create user\" }, \n"
  "  \"ERROR_SESSION_USERHANDLER_ROLE_NOT_FOUND\" : { \"code\" : 1702, \"message\" : \"role not found\" }, \n"
  "  \"ERROR_SESSION_USERHANDLER_NO_CREATE_PERMISSION\" : { \"code\" : 1703, \"message\" : \"no permission to create user with that role\" }, \n"
  "  \"ERROR_SESSION_USERHANDLER_USER_NOT_FOUND\" : { \"code\" : 1704, \"message\" : \"user not found\" }, \n"
  "  \"ERROR_SESSION_USERHANDLER_CANNOT_CHANGE_PW\" : { \"code\" : 1705, \"message\" : \"cannot manage password for user\" }, \n"
  "  \"ERROR_SESSION_SESSIONHANDLER_URL_INVALID1\" : { \"code\" : 1706, \"message\" : \"expecting POST <prefix>/session\" }, \n"
  "  \"ERROR_SESSION_SESSIONHANDLER_URL_INVALID2\" : { \"code\" : 1707, \"message\" : \"expecting GET <prefix>/session/<sid>\" }, \n"
  "  \"ERROR_SESSION_SESSIONHANDLER_URL_INVALID3\" : { \"code\" : 1708, \"message\" : \"expecting PUT <prefix>/session/<sid>/<method>\" }, \n"
  "  \"ERROR_SESSION_SESSIONHANDLER_URL_INVALID4\" : { \"code\" : 1709, \"message\" : \"expecting DELETE <prefix>/session/<sid>\" }, \n"
  "  \"ERROR_SESSION_SESSIONHANDLER_SESSION_UNKNOWN\" : { \"code\" : 1710, \"message\" : \"unknown session\" }, \n"
  "  \"ERROR_SESSION_SESSIONHANDLER_SESSION_NOT_BOUND\" : { \"code\" : 1711, \"message\" : \"session has not bound to user\" }, \n"
  "  \"ERROR_SESSION_SESSIONHANDLER_CANNOT_LOGIN\" : { \"code\" : 1712, \"message\" : \"cannot login with session\" }, \n"
  "  \"ERROR_SESSION_USERSHANDLER_INVALID_URL\" : { \"code\" : 1713, \"message\" : \"expecting GET <prefix>/users\" }, \n"
  "  \"ERROR_SESSION_DIRECTORYSERVER_INVALID_URL\" : { \"code\" : 1714, \"message\" : \"expecting /directory/sessionvoc/<token>\" }, \n"
  "  \"ERROR_SESSION_DIRECTORYSERVER_NOT_CONFIGURED\" : { \"code\" : 1715, \"message\" : \"directory server is not configured\" }, \n"
  "  \"SIMPLE_CLIENT_UNKNOWN_ERROR\"  : { \"code\" : 2000, \"message\" : \"unknown client error\" }, \n"
  "  \"SIMPLE_CLIENT_COULD_NOT_CONNECT\" : { \"code\" : 2001, \"message\" : \"could not connect to server\" }, \n"
  "  \"SIMPLE_CLIENT_COULD_NOT_WRITE\" : { \"code\" : 2002, \"message\" : \"could not write to server\" }, \n"
  "  \"SIMPLE_CLIENT_COULD_NOT_READ\" : { \"code\" : 2003, \"message\" : \"could not read from server\" }, \n"
  "};\n"
  "\n"
;
