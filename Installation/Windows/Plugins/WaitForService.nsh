!include "LogicLib.nsh"
!define TRI_SVC_NAME 'ArangoDB'

;--------------------------------

Function WaitForServiceUp
  DetailPrint "starting ArangoDB Service..."
  Push 0
  Pop $R0
  try_again:
    SimpleSC::ServiceIsRunning '${TRI_SVC_NAME}'
    Pop $0 ; returns an errorcode (<>0) otherwise success (0)
    Pop $1 ; returns 1 (service is running) - returns 0 (service is not running)
    ${If} $1 == 1
      ;MessageBox MB_OK "Service running : $1 "
      ; ok, running now.
      push 0
      Return
    ${EndIf}
    Sleep 1000
    ${If} $R0 == 40
      SetErrorLevel 3
      push 1
      IfSilent 0 openStartPopup
      Quit
     openStartPopup:
      MessageBox MB_OK "Waited 40 seconds for ArangoDB to come up; Please look at the Windows Eventlog for eventual errors!"
      Return
    ${EndIf}
    IntOp $R0 $R0 + 1
    Goto try_again
FunctionEnd

;--------------------------------
!macro WaitForServiceDown un
Function ${un}WaitForServiceDown
  DetailPrint "stopping ArangoDB Service..."
  Push 0
  Pop $R0
  try_again:
    SimpleSC::ServiceIsRunning '${TRI_SVC_NAME}'
    Pop $0 ; returns an errorcode (<>0) otherwise success (0)
    Pop $1 ; returns 1 (service is running) - returns 0 (service is not running)
    ${If} $1 == 0
      ;MessageBox MB_OK "Service running : $1 "
      ; ok, stopped by now.
      push 0
      Return
    ${EndIf}
    Sleep 1000
    ${If} $R0 == 40
      SetErrorLevel 3
      Push 1
      IfSilent 0 openStopPopup
      Quit
     openStopPopup:
      MessageBox MB_OK "Waited 40 seconds for the ArangoDB Service to shutdown; you may need to remove files by hand"
      Return
    ${EndIf}
    IntOp $R0 $R0 + 1
    Goto try_again
FunctionEnd
!macroend

!insertmacro WaitForServiceDown ""
!insertmacro WaitForServiceDown "un."
