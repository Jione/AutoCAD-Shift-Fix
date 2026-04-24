; Set script to run at maximum speed
SetBatchLines, -1
; Use the fastest send method available
SendMode, Input

; Group AutoCAD and AutoCAD LT together
GroupAdd, AutoCADGroup, ahk_exe acad.exe
GroupAdd, AutoCADGroup, ahk_exe acadlt.exe

; Apply hotkey only when AutoCAD is the active window
#IfWinActive ahk_group AutoCADGroup

; Intercept ONLY the Left Shift key press
~LShift::
    ; Wait 260ms for physical key release
    KeyWait, LShift, T0.26
    
    ; If timeout occurs (key is held down for 260ms)
    if (ErrorLevel)
    {
        ; Send down signal
        SendInput {LShift down}
    }
return

; End of AutoCAD specific block
#IfWinActive
