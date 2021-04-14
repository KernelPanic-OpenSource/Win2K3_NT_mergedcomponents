;
;/*************************************************************************
;*
;* notifyev.h        !!!! DO NOT EDIT THIS FILE !!!!
;*
;* Winlogon notification DLL error codes
;*
;* NOTE: generated from: \nt\private\dllmerge\wlnotify\notifyev.mc
;*
;*
;* Copyright Microsoft Corporation, 1998
;*
;*************************************************************************/
;
;
;

;
;/*************************************************************************
;*
; TSNOTIFY Events:
;*
;*************************************************************************/
;
;

;#define CATEGORY_NOTIFY_EVENTS 1
MessageId=0x1
Language=English
Terminal Server Notify Events
.

MessageId=1100 SymbolicName=EVENT_NOTIFY_INIT_FAILED
Language=English
TS RDPDR Device Redirection failed to initialize for a single user session %1.  Serial, Parallel, Printer, and File System Redirection will be disabled.  Look in the System event log for other related errors.
.

MessageId=1101 SymbolicName=EVENT_NOTIFY_RDPDR_FAILED
Language=English
The RDPDR device manager component could not be opened.  Check the status of the Terminal Service Device Redirector in the Device Manager.
.

MessageId=1102 SymbolicName=EVENT_NOTIFY_DRIVER_NOT_FOUND
Language=English
Driver %1 required for printer %2 is not installed on the server. Contact the administrator to install the driver before you log in again.
.

MessageId=1103 SymbolicName=EVENT_NOTIFY_PRINTER_REDIRECTION_FAILED
Language=English
An internal communication error occurred.  Redirected printing will no longer function for a single user session.  Check the status of the Terminal Services Device Redirector in the System folder of Device Manager.
.

MessageId=1104 SymbolicName=EVENT_NOTIFY_OPENPRINTER_FAILED
Language=English
The printer %1 could not be opened.
.

MessageId=1105 SymbolicName=EVENT_NOTIFY_SET_PRINTER_ACL_FAILED
Language=English
Printer security information for the printer %1 could not be set.
.

MessageId=1106 SymbolicName=EVENT_NOTIFY_INSTALL_PRINTER_FAILED
Language=English
The printer could not be installed.
.

MessageId=1107 SymbolicName=EVENT_NOTIFY_DELETE_PRINTER_FAILED
Language=English
The printer %1 could not be deleted.
.

MessageId=1108 SymbolicName=EVENT_NOTIFY_RESTORE_PRINTER_CONFIG_FAILED
Language=English
Configuration information for the %1 printer could not be restored. 
.

MessageId=1109 SymbolicName=EVENT_NOTIFY_SETDEFAULTPRINTER_FAILED
Language=English
The %1 printer could not be set as the default printer.
.

MessageId=1110 SymbolicName=EVENT_NOTIFY_ERRORPARSINGINF
Language=English
An error occurred during processing of %1. The file might be corrupted. Run Setup again to reinstall the file. 
.

MessageId=1111 SymbolicName=EVENT_NOTIFY_UNKNOWN_PRINTER_DRIVER
Language=English
Driver %1 required for printer %2 is unknown. Contact the administrator to install the driver before you log in again.
.

MessageId=1112 SymbolicName=EVENT_NOTIFY_FAILEDTOREGFOR_SETTING_NOTIFY
Language=English
Failed to register for user printing preferences change notification.  Open the Services snap-in and confirm that the Printer Spooler service is running 
.

MessageId=1113 SymbolicName=EVENT_NOTIFY_INSUFFICIENTRESOURCES
Language=English
There is not enough memory to complete the operation. Save your work, close other programs, and then try again.
.

MessageId=1114 SymbolicName=EVENT_NOTIFY_SPOOLERERROR
Language=English
Error communicating with the Spooler system service.  Open the Services snap-in and confirm that the Print Spooler service is running.
.

MessageId=1115 SymbolicName=EVENT_NOTIFY_INTERNALERROR
Language=English
Unexpected internal error in TS RDP device redirection.  Contact technical support.
.

