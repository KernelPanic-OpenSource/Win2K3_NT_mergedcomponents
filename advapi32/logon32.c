//+---------------------------------------------------------------------------
//
//  Microsoft Windows
//  Copyright (C) Microsoft Corporation, 1992 - 1993.
//
//  File:       logon32.c
//
//  Contents:
//
//  Classes:
//
//  Functions:
//
//  History:    9-30-94   RichardW   Created
//
//----------------------------------------------------------------------------


#include "advapi.h"
#include <crypt.h>
#include <mpr.h>
#include <ntlsa.h>
#include <ntmsv1_0.h>
#include <wchar.h>
#include <stdlib.h>
#include <lmcons.h>

#define SECURITY_WIN32
#include <security.h>

#include <windows.h>

#include <winbase.h>
#include <winbasep.h>
#include <execsrv.h>
#include <winsta.h>

//
// We dynamically load mpr.dll (no big surprise there), in order to call
// WNetLogonNotify, as defined in private\inc\mpr.h.  This prototype matches
// it -- consult the header file for all the parameters.
//
typedef (* LOGONNOTIFYFN)(LPCWSTR, PLUID, LPCWSTR, LPVOID,
                            LPCWSTR, LPVOID, LPWSTR, LPVOID, LPWSTR *);

//
// The QuotaLimits are global, because the defaults
// are always used for accounts, based on server/wksta, and no one ever
// calls lsasetaccountquota
//

HANDLE      Logon32LsaHandle = NULL;
ULONG       Logon32MsvHandle = 0xFFFFFFFF;
ULONG       Logon32NegoHandle = 0xFFFFFFFF;
WCHAR       Logon32DomainName[DNLEN+1] = L"";

QUOTA_LIMITS    Logon32QuotaLimits;
HINSTANCE       Logon32MprHandle = NULL;
LOGONNOTIFYFN   Logon32LogonNotify = NULL;


RTL_CRITICAL_SECTION    Logon32Lock;

#define LockLogon()     RtlEnterCriticalSection( &Logon32Lock )
#define UnlockLogon()   RtlLeaveCriticalSection( &Logon32Lock )


SID_IDENTIFIER_AUTHORITY L32SystemSidAuthority = SECURITY_NT_AUTHORITY;
SID_IDENTIFIER_AUTHORITY L32LocalSidAuthority = SECURITY_LOCAL_SID_AUTHORITY;



#define COMMON_CREATE_SUSPENDED 0x00000001  // Suspended, do not Resume()
#define COMMON_CREATE_PROCESSSD 0x00000002  // Whack the process SD
#define COMMON_CREATE_THREADSD  0x00000004  // Whack the thread SD


BOOL
WINAPI
LogonUserCommonA(
    LPCSTR          lpszUsername,
    LPCSTR          lpszDomain,
    LPCSTR          lpszPassword,
    DWORD          dwLogonType,
    DWORD          dwLogonProvider,
    BOOL           fExVersion,
    HANDLE *       phToken,
    PSID   *       ppLogonSid,
    PVOID  *       ppProfileBuffer,
    DWORD  *       pdwProfileLength,
    PQUOTA_LIMITS  pQuotaLimits
    );


BOOL
WINAPI
LogonUserCommonW(
    PCWSTR          lpszUsername,
    PCWSTR          lpszDomain,
    PCWSTR          lpszPassword,
    DWORD          dwLogonType,
    DWORD          dwLogonProvider,
    BOOL           fExVersion,
    HANDLE *       phToken,
    PSID   *       ppLogonSid,
    PVOID  *       ppProfileBuffer,
    DWORD  *       pdwProfileLength,
    PQUOTA_LIMITS  pQuotaLimits
    );


//+---------------------------------------------------------------------------
//
//  Function:   Logon32Initialize
//
//  Synopsis:   Initializes the critical section
//
//  Arguments:  [hMod]    --
//              [Reason]  --
//              [Context] --
//
//----------------------------------------------------------------------------
BOOL
Logon32Initialize(
    IN PVOID    hMod,
    IN ULONG    Reason,
    IN PCONTEXT Context)
{
    NTSTATUS    Status;

    if (Reason == DLL_PROCESS_ATTACH)
    {
        Status = RtlInitializeCriticalSection( &Logon32Lock );
        return( Status == STATUS_SUCCESS );
    }

    return( TRUE );
}


/***************************************************************************\
* FindLogonSid
*
* Finds logon sid for a new logon from the access token.
*
\***************************************************************************/
PSID
L32FindLogonSid(
    IN  HANDLE  hToken
    )
{
    PTOKEN_GROUPS   pGroups = NULL;
    DWORD           cbGroups;
    PVOID           FastBuffer[ 512 / sizeof(PVOID) ];
    PTOKEN_GROUPS   pSlowBuffer = NULL;
    UINT            i;
    PSID            Sid = NULL;


    pGroups = (PTOKEN_GROUPS)FastBuffer;
    cbGroups = sizeof(FastBuffer);

    if(!GetTokenInformation(
                hToken,
                TokenGroups,
                pGroups,
                cbGroups,
                &cbGroups
                ))
    {
        if( GetLastError() != ERROR_INSUFFICIENT_BUFFER ) {
            return NULL;
        }

        pSlowBuffer = (PTOKEN_GROUPS)LocalAlloc(LMEM_FIXED, cbGroups);

        if( pSlowBuffer == NULL ) {
            return NULL;
        }

        pGroups = pSlowBuffer;


        if(!GetTokenInformation(
                    hToken,
                    TokenGroups,
                    pGroups,
                    cbGroups,
                    &cbGroups
                    )) {
            goto Cleanup;
        }
    }


    //
    // Get the logon Sid by looping through the Sids in the token
    //

    for(i = 0 ; i < pGroups->GroupCount ; i++) {
        if(pGroups->Groups[i].Attributes & SE_GROUP_LOGON_ID) {
            DWORD dwSidLength;

            //
            // insure we are dealing with a valid Sid
            //

            if(!IsValidSid(pGroups->Groups[i].Sid)) {
                goto Cleanup;
            }

            //
            // get required allocation size to copy the Sid
            //

            dwSidLength = GetLengthSid(pGroups->Groups[i].Sid);

            Sid = (PSID)LocalAlloc( LMEM_FIXED, dwSidLength );
            if( Sid == NULL ) {
                goto Cleanup;
            }

            CopySid(dwSidLength, Sid, pGroups->Groups[i].Sid);

            break;
        }
    }

Cleanup:

    if( pSlowBuffer )
    {
        LocalFree( pSlowBuffer );
    }

    return Sid;
}


/*******************************************************************

    NAME:       GetDefaultDomainName

    SYNOPSIS:   Fills in the given array with the name of the default
                domain to use for logon validation.

    ENTRY:      pszDomainName - Pointer to a buffer that will receive
                    the default domain name.

                cchDomainName - The size (in charactesr) of the domain
                    name buffer.

    RETURNS:    TRUE if successful, FALSE if not.

    HISTORY:
        KeithMo     05-Dec-1994 Created.
        RichardW    10-Jan-95   Liberated from sockets and stuck in base

********************************************************************/
BOOL
L32GetDefaultDomainName(
    PUNICODE_STRING     pDomainName
    )
{
    OBJECT_ATTRIBUTES           ObjectAttributes;
    NTSTATUS                    NtStatus;
    INT                         Result;
    DWORD                       err             = 0;
    LSA_HANDLE                  LsaPolicyHandle = NULL;
    PPOLICY_ACCOUNT_DOMAIN_INFO DomainInfo      = NULL;
    PUNICODE_STRING             pDomain;

    if (Logon32DomainName[0] != L'\0')
    {
        RtlInitUnicodeString(pDomainName, Logon32DomainName);
        return(TRUE);
    }
    //
    //  Open a handle to the local machine's LSA policy object.
    //

    InitializeObjectAttributes( &ObjectAttributes,  // object attributes
                                NULL,               // name
                                0L,                 // attributes
                                NULL,               // root directory
                                NULL );             // security descriptor

    NtStatus = LsaOpenPolicy( NULL,                 // system name
                              &ObjectAttributes,    // object attributes
                              POLICY_EXECUTE,       // access mask
                              &LsaPolicyHandle );   // policy handle

    if( !NT_SUCCESS( NtStatus ) )
    {
        BaseSetLastNTError(NtStatus);
        return(FALSE);
    }

    //
    //  Query the domain information from the policy object.
    //
    NtStatus = LsaQueryInformationPolicy( LsaPolicyHandle,
                                          PolicyAccountDomainInformation,
                                          (PVOID *) &DomainInfo );

    if (!NT_SUCCESS(NtStatus))
    {
        BaseSetLastNTError(NtStatus);
        LsaClose(LsaPolicyHandle);
        return(FALSE);
    }


    (void) LsaClose(LsaPolicyHandle);

    //
    // Copy the domain name into our cache, and
    //

    CopyMemory( Logon32DomainName,
                DomainInfo->DomainName.Buffer,
                DomainInfo->DomainName.Length );

    //
    // Null terminate it appropriately
    //

    Logon32DomainName[DomainInfo->DomainName.Length / sizeof(WCHAR)] = L'\0';

    //
    // Clean up
    //
    LsaFreeMemory( (PVOID)DomainInfo );

    //
    // And init the string
    //
    RtlInitUnicodeString(pDomainName, Logon32DomainName);

    return TRUE;

}   // GetDefaultDomainName

//+---------------------------------------------------------------------------
//
//  Function:   L32pInitLsa
//
//  Synopsis:   Initialize connection with LSA
//
//  Arguments:  (none)
//
//  History:    4-21-95   RichardW   Created
//
//  Notes:
//
//----------------------------------------------------------------------------
BOOL
L32pInitLsa(void)
{
    STRING  PackageName;

    ULONG MsvHandle;
    ULONG NegoHandle;

    NTSTATUS Status;

    //
    // Hookup to the LSA and locate our authentication package.
    //

    Status = LsaConnectUntrusted(
                 &Logon32LsaHandle
                 );

    if (!NT_SUCCESS(Status)) {
        Logon32LsaHandle = NULL;
        goto Cleanup;
    }


    //
    // Connect with the MSV1_0 authentication package
    //
    RtlInitString(&PackageName, "MICROSOFT_AUTHENTICATION_PACKAGE_V1_0");
    Status = LsaLookupAuthenticationPackage (
                Logon32LsaHandle,
                &PackageName,
                &MsvHandle
                );

    if (!NT_SUCCESS(Status)) {
        goto Cleanup;
    }

    //
    // Connect with the Negotiate authentication package
    //
    RtlInitString(&PackageName, NEGOSSP_NAME_A);
    Status = LsaLookupAuthenticationPackage (
                Logon32LsaHandle,
                &PackageName,
                &NegoHandle
                );

    if (!NT_SUCCESS(Status)) {
        goto Cleanup;
    }

    //
    // Wait until successful to update the 2 globals.
    //

    Logon32NegoHandle = NegoHandle;
    Logon32MsvHandle = MsvHandle;

Cleanup:


    if( !NT_SUCCESS(Status) ) {

        if( Logon32LsaHandle ) {
            (VOID) LsaDeregisterLogonProcess( Logon32LsaHandle );
            Logon32LsaHandle = NULL;
        }

        BaseSetLastNTError( Status );
        return FALSE;
    }

    return TRUE;
}

//+---------------------------------------------------------------------------
//
//  Function:   L32pNotifyMpr
//
//  Synopsis:   Loads the MPR DLL and notifies the network providers (like
//              csnw) so they know about this logon session and the credentials
//
//  Arguments:  [NewLogon] -- New logon information
//              [LogonId]  -- Logon ID
//
//  History:    4-24-95   RichardW   Created
//
//  Notes:
//
//----------------------------------------------------------------------------
BOOL
L32pNotifyMpr(
    PMSV1_0_INTERACTIVE_LOGON   NewLogon,
    PLUID                       LogonId
    )
{
    MSV1_0_INTERACTIVE_LOGON    OldLogon;
    LPWSTR                      LogonScripts;
    DWORD                       status;
    LUID                        LocalServiceLuid   = LOCALSERVICE_LUID;
    LUID                        NetworkServiceLuid = NETWORKSERVICE_LUID;

    if (RtlEqualLuid(LogonId, &LocalServiceLuid)
         ||
        RtlEqualLuid(LogonId, &NetworkServiceLuid))
    {
        //
        // Don't notify providers for LocalService/NetworkService logons
        //

        return( TRUE );
    }

    if ( Logon32MprHandle == NULL )
    {
        LockLogon();

        if ( Logon32MprHandle == NULL)
        {
            Logon32MprHandle =  LoadLibrary("mpr.dll");
            if (Logon32MprHandle != NULL) {

                Logon32LogonNotify = (LOGONNOTIFYFN) GetProcAddress(
                                        Logon32MprHandle,
                                        "WNetLogonNotify");

            }
        }

        UnlockLogon();
    }

    if ( Logon32LogonNotify != NULL )
    {


        CopyMemory(&OldLogon, NewLogon, sizeof(OldLogon));

        status = Logon32LogonNotify(
                        L"Windows NT Network Provider",
                        LogonId,
                        L"MSV1_0:Interactive",
                        (LPVOID)NewLogon,
                        L"MSV1_0:Interactive",
                        (LPVOID)&OldLogon,
                        L"SvcCtl",          // StationName
                        NULL,               // StationHandle
                        &LogonScripts);     // LogonScripts

        if (status == NO_ERROR) {
            if (LogonScripts != NULL ) {
                (void) LocalFree(LogonScripts);
            }
        }

        return( TRUE );
    }

    return( FALSE );
}


//+---------------------------------------------------------------------------
//
//  Function:   L32pLogonUser
//
//  Synopsis:   Wraps up the call to LsaLogonUser
//
//  Arguments:  [LsaHandle]             --
//              [AuthenticationPackage] --
//              [LogonType]             --
//              [UserName]              --
//              [Domain]                --
//              [Password]              --
//              [LogonId]               --
//              [LogonToken]            --
//              [Quotas]                --
//              [pProfileBuffer]        --
//              [pProfileBufferLength]  --
//              [pSubStatus]            --
//
//  History:    4-24-95   RichardW   Created
//
//  Notes:
//
//----------------------------------------------------------------------------
NTSTATUS
L32pLogonUser(
    IN HANDLE LsaHandle,
    IN ULONG AuthenticationPackage,
    IN SECURITY_LOGON_TYPE LogonType,
    IN PUNICODE_STRING UserName,
    IN PUNICODE_STRING Domain,
    IN PUNICODE_STRING Password,
    OUT PLUID LogonId,
    OUT PHANDLE LogonToken,
    OUT PQUOTA_LIMITS Quotas,
    OUT PVOID *pProfileBuffer,
    OUT PULONG pProfileBufferLength,
    OUT PNTSTATUS pSubStatus
    )
{
    NTSTATUS Status;
    STRING OriginName;
    TOKEN_SOURCE SourceContext;
    PMSV1_0_INTERACTIVE_LOGON MsvAuthInfo;
    PMSV1_0_LM20_LOGON MsvNetAuthInfo;
    PVOID AuthInfoBuf;
    ULONG AuthInfoSize;
    WCHAR ComputerName[ MAX_COMPUTERNAME_LENGTH + 1 ];
    DWORD ComputerNameLength;

    //
    // Initialize source context structure
    //

    strncpy(SourceContext.SourceName, "Advapi  ", sizeof(SourceContext.SourceName)); // LATER from res file

    Status = NtAllocateLocallyUniqueId(&SourceContext.SourceIdentifier);

    if (!NT_SUCCESS(Status)) {
        return(Status);
    }

    //
    // Set logon origin
    //

    RtlInitString(&OriginName, "LogonUser API");

    //
    // For network logons, do the magic.
    //

    if ( ( LogonType == Network ) )
    {
        ComputerNameLength = MAX_COMPUTERNAME_LENGTH + 1;

        if (!GetComputerNameW( ComputerName, &ComputerNameLength ) )
        {
            return(STATUS_INVALID_PARAMETER);
        }

        AuthInfoSize = sizeof( MSV1_0_LM20_LOGON ) +
                         UserName->Length +
                         Domain->Length +
                         sizeof(WCHAR) * (ComputerNameLength + 1) +
                         Password->Length + // NT password
                         (LM20_PWLEN+1) ;   // LM passsword (worst case)          

        MsvNetAuthInfo = AuthInfoBuf = RtlAllocateHeap( RtlProcessHeap(),
                                                        HEAP_ZERO_MEMORY,
                                                        AuthInfoSize );

        if ( !MsvNetAuthInfo )
        {
            return( STATUS_NO_MEMORY );
        }

        //
        // Start packing in the string
        //

        MsvNetAuthInfo->MessageType = MsV1_0NetworkLogon;

        //
        // Copy the user name into the authentication buffer
        //

        MsvNetAuthInfo->UserName.Length =
                    UserName->Length;
        MsvNetAuthInfo->UserName.MaximumLength =
                    MsvNetAuthInfo->UserName.Length;

        MsvNetAuthInfo->UserName.Buffer = (PWSTR)(MsvNetAuthInfo+1);
        RtlCopyMemory(
            MsvNetAuthInfo->UserName.Buffer,
            UserName->Buffer,
            UserName->Length
            );


        //
        // Copy the domain name into the authentication buffer
        //

        MsvNetAuthInfo->LogonDomainName.Length = Domain->Length;
        MsvNetAuthInfo->LogonDomainName.MaximumLength = Domain->Length ;

        MsvNetAuthInfo->LogonDomainName.Buffer = (PWSTR)
                                     ((PBYTE)(MsvNetAuthInfo->UserName.Buffer) +
                                     MsvNetAuthInfo->UserName.MaximumLength);

        RtlCopyMemory(
            MsvNetAuthInfo->LogonDomainName.Buffer,
            Domain->Buffer,
            Domain->Length);

        //
        // Copy the workstation name into the buffer
        //

        MsvNetAuthInfo->Workstation.Length = (USHORT)
                            (sizeof(WCHAR) * ComputerNameLength);

        MsvNetAuthInfo->Workstation.MaximumLength =
                            MsvNetAuthInfo->Workstation.Length + sizeof(WCHAR);

        MsvNetAuthInfo->Workstation.Buffer = (PWSTR)
                            ((PBYTE) (MsvNetAuthInfo->LogonDomainName.Buffer) +
                            MsvNetAuthInfo->LogonDomainName.MaximumLength );

        wcscpy( MsvNetAuthInfo->Workstation.Buffer, ComputerName );

        //
        // Set up space for Password (Unicode)
        //

        MsvNetAuthInfo->CaseSensitiveChallengeResponse.Buffer = (PUCHAR)
                    ((PBYTE) (MsvNetAuthInfo->Workstation.Buffer) +
                    MsvNetAuthInfo->Workstation.MaximumLength );

        MsvNetAuthInfo->CaseSensitiveChallengeResponse.Length =
        MsvNetAuthInfo->CaseSensitiveChallengeResponse.MaximumLength =
                            Password->Length;

        RtlCopyMemory(
            MsvNetAuthInfo->CaseSensitiveChallengeResponse.Buffer,
            Password->Buffer,
            Password->Length);

        
        //
        // handle passing in the case-insensitive version.
        //

        if( (Password->Length/sizeof(WCHAR)) > LM20_PWLEN )
        {
            Status = STATUS_INVALID_PARAMETER;
        } else {

            MsvNetAuthInfo->CaseInsensitiveChallengeResponse.Buffer = (PUCHAR)
                    ((PBYTE) (MsvNetAuthInfo->CaseSensitiveChallengeResponse.Buffer) +
                    MsvNetAuthInfo->CaseSensitiveChallengeResponse.MaximumLength );

            MsvNetAuthInfo->CaseInsensitiveChallengeResponse.Length = LM20_PWLEN;
            MsvNetAuthInfo->CaseInsensitiveChallengeResponse.MaximumLength = LM20_PWLEN+1;

            Status = RtlUpcaseUnicodeStringToOemString(
                        &MsvNetAuthInfo->CaseInsensitiveChallengeResponse,
                        Password,
                        FALSE
                        );
        }

        if ( !NT_SUCCESS(Status) )
        {
            Status = STATUS_SUCCESS;

            //
            // If we're here, the NT (supplied) password is longer than the
            // limit allowed for LM passwords.  NULL out the field, so that
            // MSV knows not to worry about it.
            //

            RtlZeroMemory( &MsvNetAuthInfo->CaseInsensitiveChallengeResponse,
                           sizeof(MsvNetAuthInfo->CaseInsensitiveChallengeResponse) );
        }

        //
        // to be consistent with Negotiate/Kerberos for _WINNT50 cases,
        // allow machine accounts to be logged on.
        //

        MsvNetAuthInfo->ParameterControl =  MSV1_0_CLEARTEXT_PASSWORD_ALLOWED |
                                            MSV1_0_CLEARTEXT_PASSWORD_SUPPLIED |
                                            MSV1_0_ALLOW_SERVER_TRUST_ACCOUNT |
                                            MSV1_0_ALLOW_WORKSTATION_TRUST_ACCOUNT;

    }
    else
    {
        //
        // Build logon structure for non-network logons - service,
        // batch, interactive, unlock, new credentials, networkcleartext
        //

        AuthInfoSize = sizeof(MSV1_0_INTERACTIVE_LOGON) +
                        UserName->Length +
                        Domain->Length +
                        Password->Length;

        MsvAuthInfo = AuthInfoBuf = RtlAllocateHeap(RtlProcessHeap(),
                                                    HEAP_ZERO_MEMORY,
                                                    AuthInfoSize);

        if (MsvAuthInfo == NULL) {
            return(STATUS_NO_MEMORY);
        }

        //
        // This authentication buffer will be used for a logon attempt
        //

        MsvAuthInfo->MessageType = MsV1_0InteractiveLogon;


        //
        // Copy the user name into the authentication buffer
        //

        MsvAuthInfo->UserName.Length = UserName->Length;
        MsvAuthInfo->UserName.MaximumLength =
                    MsvAuthInfo->UserName.Length;

        MsvAuthInfo->UserName.Buffer = (PWSTR)(MsvAuthInfo+1);
        RtlCopyMemory(
            MsvAuthInfo->UserName.Buffer,
            UserName->Buffer,
            UserName->Length
            );


        //
        // Copy the domain name into the authentication buffer
        //

        MsvAuthInfo->LogonDomainName.Length = Domain->Length;
        MsvAuthInfo->LogonDomainName.MaximumLength =
                     MsvAuthInfo->LogonDomainName.Length;

        MsvAuthInfo->LogonDomainName.Buffer = (PWSTR)
                                     ((PBYTE)(MsvAuthInfo->UserName.Buffer) +
                                     MsvAuthInfo->UserName.MaximumLength);

        RtlCopyMemory(
            MsvAuthInfo->LogonDomainName.Buffer,
            Domain->Buffer,
            Domain->Length
            );

        //
        // Copy the password into the authentication buffer
        // Hide it once we have copied it.  Use the same seed value
        // that we used for the original password in pGlobals.
        //


        MsvAuthInfo->Password.Length = Password->Length;
        MsvAuthInfo->Password.MaximumLength =
                     MsvAuthInfo->Password.Length;

        MsvAuthInfo->Password.Buffer = (PWSTR)
                                     ((PBYTE)(MsvAuthInfo->LogonDomainName.Buffer) +
                                     MsvAuthInfo->LogonDomainName.MaximumLength);

        RtlCopyMemory(
            MsvAuthInfo->Password.Buffer,
            Password->Buffer,
            Password->Length
            );

    }

    //
    // Now try to log this sucker on
    //

    Status = LsaLogonUser (
                LsaHandle,
                &OriginName,
                LogonType,
                AuthenticationPackage,
                AuthInfoBuf,
                AuthInfoSize,
                NULL,
                &SourceContext,
                pProfileBuffer,
                pProfileBufferLength,
                LogonId,
                LogonToken,
                Quotas,
                pSubStatus
                );

    //
    // Notify all the network providers, if this is a NON network logon.  Also
    // skip service logons since the LSA will call WNetLogonNotify for those.
    //

    if ( NT_SUCCESS( Status ) &&
         (LogonType != Network) &&
         (LogonType != Service) )
    {
        L32pNotifyMpr(AuthInfoBuf, LogonId);
    }

    //
    // Discard authentication buffer
    //

    RtlZeroMemory( AuthInfoBuf, AuthInfoSize );

    RtlFreeHeap(RtlProcessHeap(), 0, AuthInfoBuf);

    return(Status);
}


//+---------------------------------------------------------------------------
//
//  Function:   LogonUserCommonA
//
//  Synopsis:   ANSI wrapper for LogonUserCommonW.  See description below
//
//  Arguments:  [lpszUsername]     --
//              [lpszDomain]       --
//              [lpszPassword]     --
//              [dwLogonType]      --
//              [dwLogonProvider]  --
//              [fExVersion]       --
//              [phToken]          --
//              [ppLogonSid]       --
//              [ppProfileBuffer]  --
//              [pdwProfileLength] --
//              [pQuotaLimits]     --
//
//  History:    2-15-2000   JSchwart   Created from RichardW's LogonUserA
//
//  Notes:
//
//----------------------------------------------------------------------------
BOOL
WINAPI
LogonUserCommonA(
    LPCSTR          lpszUsername,
    LPCSTR          lpszDomain,
    LPCSTR          lpszPassword,
    DWORD          dwLogonType,
    DWORD          dwLogonProvider,
    BOOL           fExVersion,
    HANDLE *       phToken,
    PSID   *       ppLogonSid,
    PVOID  *       ppProfileBuffer,
    DWORD  *       pdwProfileLength,
    PQUOTA_LIMITS  pQuotaLimits
    )
{
    UNICODE_STRING Username;
    UNICODE_STRING Domain;
    UNICODE_STRING Password;
    ANSI_STRING Temp ;
    NTSTATUS Status;
    BOOL    bRet;


    Username.Buffer = NULL;
    Domain.Buffer = NULL;
    Password.Buffer = NULL;

    RtlInitAnsiString( &Temp, lpszUsername );
    Status = RtlAnsiStringToUnicodeString( &Username, &Temp, TRUE );
    if (!NT_SUCCESS( Status ) )
    {
        BaseSetLastNTError(Status);
        bRet = FALSE;
        goto Cleanup;
    }

    RtlInitAnsiString( &Temp, lpszDomain );
    Status = RtlAnsiStringToUnicodeString(&Domain, &Temp, TRUE );
    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        bRet = FALSE;
        goto Cleanup;
    }

    RtlInitAnsiString( &Temp, lpszPassword );
    Status = RtlAnsiStringToUnicodeString( &Password, &Temp, TRUE );
    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        bRet = FALSE;
        goto Cleanup;
    }

    bRet = LogonUserCommonW( Username.Buffer,
                             Domain.Buffer,
                             Password.Buffer,
                             dwLogonType,
                             dwLogonProvider,
                             fExVersion,
                             phToken,
                             ppLogonSid,
                             ppProfileBuffer,
                             pdwProfileLength,
                             pQuotaLimits );

Cleanup:

    if (Username.Buffer)
    {
        RtlFreeUnicodeString(&Username);
    }

    if (Domain.Buffer)
    {
        RtlFreeUnicodeString(&Domain);
    }

    if (Password.Buffer)
    {
        RtlZeroMemory(Password.Buffer, Password.Length);
        RtlFreeUnicodeString(&Password);
    }

    return(bRet);
}


//+---------------------------------------------------------------------------
//
//  Function:   LogonUserA
//
//  Synopsis:   ANSI wrapper for LogonUserW.  See description below
//
//  Arguments:  [lpszUsername]    --
//              [lpszDomain]      --
//              [lpszPassword]    --
//              [dwLogonType]     --
//              [dwLogonProvider] --
//              [phToken]         --
//
//  History:    4-25-95   RichardW   Created
//
//  Notes:
//
//----------------------------------------------------------------------------
BOOL
WINAPI
LogonUserA(
    LPCSTR       lpszUsername,
    LPCSTR       lpszDomain,
    LPCSTR       lpszPassword,
    DWORD       dwLogonType,
    DWORD       dwLogonProvider,
    HANDLE *    phToken
    )
{
    return LogonUserCommonA(lpszUsername,
                            lpszDomain,
                            lpszPassword,
                            dwLogonType,
                            dwLogonProvider,
                            FALSE,            // LogonUserA
                            phToken,
                            NULL,             // ppLogonSid
                            NULL,             // ppProfileBuffer
                            NULL,             // pdwProfileLength
                            NULL);            // pQuotaLimits
}


//+---------------------------------------------------------------------------
//
//  Function:   LogonUserExA
//
//  Synopsis:   ANSI wrapper for LogonUserExW.  See description below
//
//  Arguments:  [lpszUsername]     --
//              [lpszDomain]       --
//              [lpszPassword]     --
//              [dwLogonType]      --
//              [dwLogonProvider]  --
//              [phToken]          --
//              [ppLogonSid]       --
//              [ppProfileBuffer]  --
//              [pdwProfileLength] --
//              [pQuotaLimits]     --
//
//  History:    2-15-2000   JSchwart   Created from RichardW's LogonUserW
//
//  Notes:
//
//----------------------------------------------------------------------------
BOOL
WINAPI
LogonUserExA(
    LPCSTR          lpszUsername,
    LPCSTR          lpszDomain,
    LPCSTR          lpszPassword,
    DWORD          dwLogonType,
    DWORD          dwLogonProvider,
    HANDLE *       phToken,
    PSID   *       ppLogonSid,
    PVOID  *       ppProfileBuffer,
    DWORD  *       pdwProfileLength,
    PQUOTA_LIMITS  pQuotaLimits
    )
{
    return LogonUserCommonA(lpszUsername,
                            lpszDomain,
                            lpszPassword,
                            dwLogonType,
                            dwLogonProvider,
                            TRUE,             // LogonUserExA
                            phToken,
                            ppLogonSid,
                            ppProfileBuffer,
                            pdwProfileLength,
                            pQuotaLimits);
}


//+---------------------------------------------------------------------------
//
//  Function:   LogonUserCommonW
//
//  Synopsis:   Common code for LogonUserW and LogonUserExW.  Logs a user on
//              via plaintext password, username and domain name via the LSA.
//
//  Arguments:  [lpszUsername]     -- User name
//              [lpszDomain]       -- Domain name
//              [lpszPassword]     -- Password
//              [dwLogonType]      -- Logon type
//              [dwLogonProvider]  -- Provider
//              [fExVersion]       -- LogonUserExW or LogonUserW
//              [phToken]          -- Returned handle to primary token
//              [ppLogonSid]       -- Returned logon sid
//              [ppProfileBuffer]  -- Returned user profile buffer
//              [pdwProfileLength] -- Returned profile length
//
//  History:    2-15-2000   JSchwart   Created from RichardW's LogonUserW
//
//  Notes:      Requires SeTcbPrivilege, and will enable it if not already
//              present.
//
//----------------------------------------------------------------------------
BOOL
WINAPI
LogonUserCommonW(
    PCWSTR          lpszUsername,
    PCWSTR          lpszDomain,
    PCWSTR          lpszPassword,
    DWORD          dwLogonType,
    DWORD          dwLogonProvider,
    BOOL           fExVersion,
    HANDLE *       phToken,
    PSID   *       ppLogonSid,
    PVOID  *       ppProfileBuffer,
    DWORD  *       pdwProfileLength,
    PQUOTA_LIMITS  pQuotaLimits
    )
{
    NTSTATUS    Status;
    ULONG       PackageId;
    UNICODE_STRING  Username;
    UNICODE_STRING  Domain;
    UNICODE_STRING  Password;
    HANDLE      hTempToken;
    HANDLE    * phTempToken;
    LUID        LogonId;
    PVOID       Profile;
    ULONG       ProfileLength;
    NTSTATUS    SubStatus = STATUS_SUCCESS;
    SECURITY_LOGON_TYPE LogonType;


    //
    // Validate the provider
    //
    if (dwLogonProvider == LOGON32_PROVIDER_DEFAULT)
    {
        dwLogonProvider = LOGON32_PROVIDER_WINNT50;

        //
        // if domain was not supplied, and username is not a UPN, use
        // _WINNT40 to be compatible.
        //

        if((lpszUsername != NULL) &&
           (lpszDomain == NULL || lpszDomain[ 0 ] == L'\0'))
        {
            if( wcschr( lpszUsername, L'@' ) == NULL )
            {
                dwLogonProvider = LOGON32_PROVIDER_WINNT40;
            }
        }
    }

    if (dwLogonProvider > LOGON32_PROVIDER_WINNT50)
    {
        BaseSetLastNTError(STATUS_INVALID_PARAMETER);
        return(FALSE);
    }

    switch (dwLogonType)
    {
        case LOGON32_LOGON_INTERACTIVE:
            LogonType = Interactive;
            break;

        case LOGON32_LOGON_BATCH:
            LogonType = Batch;
            break;

        case LOGON32_LOGON_SERVICE:
            LogonType = Service;
            break;

        case LOGON32_LOGON_NETWORK:
            LogonType = Network;
            break;                 

        case LOGON32_LOGON_UNLOCK:
            LogonType = Unlock ;
            break;

        case LOGON32_LOGON_NETWORK_CLEARTEXT:
            LogonType = NetworkCleartext ;
            break;

        case LOGON32_LOGON_NEW_CREDENTIALS:
            LogonType = NewCredentials;
            break;

        default:
            BaseSetLastNTError(STATUS_INVALID_PARAMETER);
            return(FALSE);
            break;
    }

    //
    // If the MSV handle is -1, grab the lock, and try again:
    //

    if (Logon32MsvHandle == 0xFFFFFFFF || Logon32NegoHandle == 0xFFFFFFFF)
    {
        LockLogon();

        //
        // If the MSV handle is still -1, init our connection to lsa.  We
        // have the lock, so no other threads can't be trying this right now.
        //
        if (Logon32MsvHandle == 0xFFFFFFFF || Logon32NegoHandle == 0xFFFFFFFF)
        {
            if (!L32pInitLsa())
            {
                UnlockLogon();

                return( FALSE );
            }
        }

        UnlockLogon();
    }

    //
    // Validate the parameters.  NULL or empty domain or NULL or empty
    // user name is invalid.
    //

    RtlInitUnicodeString(&Username, lpszUsername);
    if (Username.Length == 0)
    {
        BaseSetLastNTError(STATUS_INVALID_PARAMETER);
        return(FALSE);
    }

    //
    // Initialize/check parameters based on which API we're servicing.
    //
    if (!fExVersion)
    {
        //
        // LogonUserW -- phToken is required.  Initialize the token handle,
        // if the pointer is invalid, then catch the exception now.
        //

        *phToken    = NULL;
        phTempToken = phToken;
    }
    else
    {
        //
        // LogonUserExW -- phToken, ppLogonSid, ppProfileBuffer, and
        // pdwProfileLength are optional.  Initialize as appropriate.
        //

        if (ARGUMENT_PRESENT(phToken))
        {
            *phToken    = NULL;
            phTempToken = phToken;
        }
        else
        {
            //
            // Dummy token handle to use in the LsaLogonUser call
            //
            phTempToken = &hTempToken;
        }

        if (ARGUMENT_PRESENT(ppLogonSid))
        {
            *ppLogonSid = NULL;
        }

        if (!!ppProfileBuffer ^ !!pdwProfileLength)
        {
            //
            // Can't have one without the other...
            //
            BaseSetLastNTError(STATUS_INVALID_PARAMETER);
            return(FALSE);
        }

        if (ARGUMENT_PRESENT(ppProfileBuffer))
        {
            *ppProfileBuffer  = NULL;
            *pdwProfileLength = 0;
        }

        if (ARGUMENT_PRESENT(pQuotaLimits))
        {
            RtlZeroMemory(pQuotaLimits, sizeof(QUOTA_LIMITS));
        }
    }

    //
    // Parse that domain.  Note, if the special token . is passed in for
    // domain, we will use the right value from the LSA, meaning AccountDomain.
    // If the domain is null, the lsa will talk to the local domain, the
    // primary domain, and then on from there...
    //
    if (lpszDomain && *lpszDomain)
    {
        if ((lpszDomain[0] == L'.') &&
            (lpszDomain[1] == L'\0') )
        {
            if (!L32GetDefaultDomainName(&Domain))
            {
                return(FALSE);
            }
        }
        else
        {
            RtlInitUnicodeString(&Domain, lpszDomain);
        }
    }
    else
    {
        RtlInitUnicodeString(&Domain, lpszDomain);
    }

    //
    // Finally, init the password
    //
    RtlInitUnicodeString(&Password, lpszPassword);



    //
    // Attempt the logon
    //

    Status = L32pLogonUser(
                    Logon32LsaHandle,
                    (dwLogonProvider == LOGON32_PROVIDER_WINNT50) ?
                        Logon32NegoHandle : Logon32MsvHandle,
                    LogonType,
                    &Username,
                    &Domain,
                    &Password,
                    &LogonId,
                    phTempToken,
                    pQuotaLimits ? pQuotaLimits : &Logon32QuotaLimits,
                    &Profile,
                    &ProfileLength,
                    &SubStatus);

    //
    // Set output parameters based on which API we're servicing
    //


    // TODO: review cleanup code if something fails mid-stream.
    //

    if (!fExVersion)
    {

        if (!NT_SUCCESS(Status))
        {
            if (Status == STATUS_ACCOUNT_RESTRICTION)
            {
                BaseSetLastNTError(SubStatus);
            }
            else
            {
                BaseSetLastNTError(Status);
            }

            return(FALSE);
        }

        if (Profile != NULL)
        {
            LsaFreeReturnBuffer(Profile);
        }
    }
    else
    {
        //
        // We may need the allocated buffers if all went well, so
        // check the return status first.
        //

        if (!NT_SUCCESS(Status))
        {
            if (Status == STATUS_ACCOUNT_RESTRICTION)
            {
                BaseSetLastNTError(SubStatus);
            }
            else
            {
                BaseSetLastNTError(Status);
            }

            return(FALSE);
        }

        //
        // The logon succeeded -- fill in the requested output parameters.
        //

        if (ARGUMENT_PRESENT(ppLogonSid))
        {
            *ppLogonSid = L32FindLogonSid( *phTempToken );

            if (*ppLogonSid == NULL)
            {
                if (Profile != NULL)
                {
                    LsaFreeReturnBuffer(Profile);
                }

                CloseHandle(*phTempToken);
                *phTempToken = NULL;

                BaseSetLastNTError(STATUS_NO_MEMORY);
                return(FALSE);
            }
        }

        if (ARGUMENT_PRESENT(ppProfileBuffer))
        {
            if (Profile != NULL)
            {
                ASSERT(ProfileLength != 0);

                *ppProfileBuffer = Profile;
                *pdwProfileLength = ProfileLength;
            }
        }
        else
        {
            if (Profile != NULL)
            {
                LsaFreeReturnBuffer(Profile);
            }
        }

        if (!ARGUMENT_PRESENT(phToken))
        {
            //
            // Close the dummy token handle
            //
            CloseHandle(*phTempToken);
        }
    }

    return(TRUE);
}


//+---------------------------------------------------------------------------
//
//  Function:   LogonUserW
//
//  Synopsis:   Logs a user on via plaintext password, username and domain
//              name via the LSA.
//
//  Arguments:  [lpszUsername]    -- User name
//              [lpszDomain]      -- Domain name
//              [lpszPassword]    -- Password
//              [dwLogonType]     -- Logon type
//              [dwLogonProvider] -- Provider
//              [phToken]         -- Returned handle to primary token
//
//  History:    4-25-95   RichardW   Created
//
//  Notes:      Requires SeTcbPrivilege, and will enable it if not already
//              present.
//
//----------------------------------------------------------------------------
BOOL
WINAPI
LogonUserW(
    PCWSTR       lpszUsername,
    PCWSTR       lpszDomain,
    PCWSTR       lpszPassword,
    DWORD       dwLogonType,
    DWORD       dwLogonProvider,
    HANDLE *    phToken
    )
{
    return LogonUserCommonW(lpszUsername,
                            lpszDomain,
                            lpszPassword,
                            dwLogonType,
                            dwLogonProvider,
                            FALSE,            // LogonUserW
                            phToken,
                            NULL,             // ppLogonSid
                            NULL,             // ppProfileBuffer
                            NULL,             // pdwProfileLength
                            NULL);            // pQuotaLimits
}


//+---------------------------------------------------------------------------
//
//  Function:   LogonUserExW
//
//  Synopsis:   Logs a user on via plaintext password, username and domain
//              name via the LSA.
//
//  Arguments:  [lpszUsername]     -- User name
//              [lpszDomain]       -- Domain name
//              [lpszPassword]     -- Password
//              [dwLogonType]      -- Logon type
//              [dwLogonProvider]  -- Provider
//              [phToken]          -- Returned handle to primary token
//              [ppLogonSid]       -- Returned logon sid
//              [ppProfileBuffer]  -- Returned user profile buffer
//              [pdwProfileLength] -- Returned profile length
//              [pQuotaLimits]     -- Returned quota limits
//
//  History:    2-15-2000   JSchwart   Created from RichardW's LogonUserW
//
//  Notes:      Requires SeTcbPrivilege, and will enable it if not already
//              present.
//
//----------------------------------------------------------------------------
BOOL
WINAPI
LogonUserExW(
    PCWSTR          lpszUsername,
    PCWSTR          lpszDomain,
    PCWSTR          lpszPassword,
    DWORD          dwLogonType,
    DWORD          dwLogonProvider,
    HANDLE *       phToken,
    PSID   *       ppLogonSid,
    PVOID  *       ppProfileBuffer,
    DWORD  *       pdwProfileLength,
    PQUOTA_LIMITS  pQuotaLimits
    )
{
    return LogonUserCommonW(lpszUsername,
                            lpszDomain,
                            lpszPassword,
                            dwLogonType,
                            dwLogonProvider,
                            TRUE,             // LogonUserExW
                            phToken,
                            ppLogonSid,
                            ppProfileBuffer,
                            pdwProfileLength,
                            pQuotaLimits);
}


//+---------------------------------------------------------------------------
//
//  Function:   ImpersonateLoggedOnUser
//
//  Synopsis:   Duplicates the token passed in if it is primary, and assigns
//              it to the thread that called.
//
//  Arguments:  [hToken] --
//
//  History:    1-10-95   RichardW   Created
//
//  Notes:
//
//----------------------------------------------------------------------------
BOOL
WINAPI
ImpersonateLoggedOnUser(
    HANDLE  hToken
    )
{
    TOKEN_TYPE                  Type;
    ULONG                       cbType;
    HANDLE                      hImpToken;
    NTSTATUS                    Status;
    SECURITY_QUALITY_OF_SERVICE SecurityQualityOfService;
    OBJECT_ATTRIBUTES           ObjectAttributes;
    BOOL                        fCloseImp;

    Status = NtQueryInformationToken(
                hToken,
                TokenType,
                &Type,
                sizeof(TOKEN_TYPE),
                &cbType);

    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        return(FALSE);
    }

    if (Type == TokenPrimary)
    {
        InitializeObjectAttributes(
                            &ObjectAttributes,
                            NULL,
                            0L,
                            NULL,
                            NULL);

        SecurityQualityOfService.Length = sizeof(SECURITY_QUALITY_OF_SERVICE);
        SecurityQualityOfService.ImpersonationLevel = SecurityImpersonation;
        SecurityQualityOfService.ContextTrackingMode = SECURITY_DYNAMIC_TRACKING;
        SecurityQualityOfService.EffectiveOnly = FALSE;

        ObjectAttributes.SecurityQualityOfService = &SecurityQualityOfService;


        Status = NtDuplicateToken( hToken,
                                   TOKEN_IMPERSONATE | TOKEN_QUERY,
                                   &ObjectAttributes,
                                   FALSE,
                                   TokenImpersonation,
                                   &hImpToken
                                 );

        if (!NT_SUCCESS(Status))
        {
            BaseSetLastNTError(Status);
            return(FALSE);
        }

        fCloseImp = TRUE;

    }

    else

    {
        hImpToken = hToken;
        fCloseImp = FALSE;
    }

    Status = NtSetInformationThread(
                NtCurrentThread(),
                ThreadImpersonationToken,
                (PVOID) &hImpToken,
                sizeof(hImpToken)
                );

    if (fCloseImp)
    {
        (void) NtClose(hImpToken);
    }

    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        return(FALSE);
    }

    return(TRUE);

}


//+---------------------------------------------------------------------------
//
//  Function:   L32SetProcessToken
//
//  Synopsis:   Sets the primary token for the new process.
//
//  Arguments:  [psd]      --
//              [hProcess] --
//              [hThread]  --
//              [hToken]   --
//
//  History:    4-25-95   RichardW   Created
//
//  Notes:
//
//----------------------------------------------------------------------------
BOOL
L32SetProcessToken(
    HANDLE                  hProcess,
    HANDLE                  hThread,
    HANDLE                  hTokenToAssign,
    BOOL                    AlreadyImpersonating
    )
{
    NTSTATUS Status, AdjustStatus;
    PROCESS_ACCESS_TOKEN PrimaryTokenInfo;
    BOOLEAN WasEnabled;
    HANDLE NullHandle;


    //
    // Set the process's primary token.  This is actually much more complex
    // to implement in a single API, but we'll live with it.  This MUST be
    // called when we are not impersonating!  The client generally does *not*
    // have the SeAssignPrimary privilege
    //


    //
    // Enable the required privilege
    //

    if ( !AlreadyImpersonating )
    {
        Status = RtlImpersonateSelf( SecurityImpersonation );
    }
    else
    {
        Status = STATUS_SUCCESS ;
    }

    if ( NT_SUCCESS( Status ) )
    {
        //
        // We now allow restricted tokens to passed in, so we don't
        // fail if the privilege isn't held.  Let the kernel deal with
        // the possibilities.
        //

        Status = RtlAdjustPrivilege(SE_ASSIGNPRIMARYTOKEN_PRIVILEGE, TRUE,
                                    TRUE, &WasEnabled);

        if ( !NT_SUCCESS( Status ) )
        {
            WasEnabled = TRUE ;     // Don't try to restore it.
        }

        PrimaryTokenInfo.Token  = hTokenToAssign;
        PrimaryTokenInfo.Thread = hThread;

        Status = NtSetInformationProcess(
                    hProcess,
                    ProcessAccessToken,
                    (PVOID)&PrimaryTokenInfo,
                    (ULONG)sizeof(PROCESS_ACCESS_TOKEN)
                    );
        //
        // Restore the privilege to its previous state
        //

        if (!WasEnabled)
        {
            AdjustStatus = RtlAdjustPrivilege(SE_ASSIGNPRIMARYTOKEN_PRIVILEGE,
                                          WasEnabled, TRUE, &WasEnabled);
            if (NT_SUCCESS(Status)) {
                Status = AdjustStatus;
            }
        }


        //
        // Revert back to process.
        //

        if ( !AlreadyImpersonating )
        {
            NullHandle = NULL;

            AdjustStatus = NtSetInformationThread(
                                NtCurrentThread(),
                                ThreadImpersonationToken,
                                (PVOID) &NullHandle,
                                sizeof( HANDLE ) );

            if ( NT_SUCCESS( Status ) )
            {
                Status = AdjustStatus;
            }
        }



    } else {

        NOTHING;
    }

    if (!NT_SUCCESS(Status)) {
        BaseSetLastNTError(Status);
    }

    return (NT_SUCCESS(Status));

}


//+---------------------------------------------------------------------------
//
//  Function:   L32SetProcessQuotas
//
//  Synopsis:   Updates the quotas for the process
//
//  Arguments:  [hProcess] --
//
//  History:    4-25-95   RichardW   Created
//
//  Notes:
//
//----------------------------------------------------------------------------
BOOL
L32SetProcessQuotas(
    HANDLE  hProcess,
    BOOL    AlreadyImpersonating )
{
    NTSTATUS Status = STATUS_SUCCESS;
    NTSTATUS AdjustStatus = STATUS_SUCCESS;
    QUOTA_LIMITS RequestedLimits;
    BOOLEAN WasEnabled;
    HANDLE NullHandle;

    RequestedLimits = Logon32QuotaLimits;
    RequestedLimits.MinimumWorkingSetSize = 0;
    RequestedLimits.MaximumWorkingSetSize = 0;

    //
    // Set the process's quota.   This MUST be
    // called when we are not impersonating!  The client generally does *not*
    // have the SeIncreaseQuota privilege.
    //

    if ( !AlreadyImpersonating )
    {
        Status = RtlImpersonateSelf( SecurityImpersonation );
    }

    if ( NT_SUCCESS( Status ) )
    {

        if (RequestedLimits.PagedPoolLimit != 0) {

            Status = RtlAdjustPrivilege(SE_INCREASE_QUOTA_PRIVILEGE, TRUE,
                                        TRUE, &WasEnabled);

            if ( NT_SUCCESS( Status ) )
            {

                Status = NtSetInformationProcess(
                            hProcess,
                            ProcessQuotaLimits,
                            (PVOID)&RequestedLimits,
                            (ULONG)sizeof(QUOTA_LIMITS)
                            );

                if (!WasEnabled)
                {
                    AdjustStatus = RtlAdjustPrivilege(SE_INCREASE_QUOTA_PRIVILEGE,
                                                  WasEnabled, FALSE, &WasEnabled);
                    if (NT_SUCCESS(Status)) {
                        Status = AdjustStatus;
                    }
                }
            }

        }

        if ( !AlreadyImpersonating )
        {
            NullHandle = NULL;

            AdjustStatus = NtSetInformationThread(
                                NtCurrentThread(),
                                ThreadImpersonationToken,
                                (PVOID) &NullHandle,
                                sizeof( HANDLE ) );

            if ( NT_SUCCESS( Status ) )
            {
                Status = AdjustStatus;
            }
        }

    }

    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        return(FALSE);
    }

    return(TRUE);
}


BOOL
L32CreateTokenForNewProcess(
    PSECURITY_DESCRIPTOR    psd,
    HANDLE                  hToken,
    PHANDLE                 phTokenToAssign
    )
{
    OBJECT_ATTRIBUTES ObjectAttributes;
    NTSTATUS Status;

    //
    // Check for a NULL token. (No need to do anything)
    // The process will run in the parent process's context and inherit
    // the default ACL from the parent process's token.
    //
    
    if (hToken == NULL)
    {
        *phTokenToAssign = NULL;
        return TRUE;
    }

    //
    // A primary token can only be assigned to one process.
    // Duplicate the logon token so we can assign one to the new
    // process.
    //

    InitializeObjectAttributes(
                 &ObjectAttributes,
                 NULL,
                 0,
                 NULL,
                 psd
                 );

    Status = NtDuplicateToken(
                 hToken,            // Duplicate this token
                 0,                 // Same desired access
                 &ObjectAttributes,
                 FALSE,             // EffectiveOnly
                 TokenPrimary,      // TokenType
                 phTokenToAssign    // Duplicate token handle stored here
                 );


    if (!NT_SUCCESS(Status)) {
        BaseSetLastNTError(Status);
        return FALSE;
    }
    return TRUE;
}


HANDLE
L32RevertOpenProcess(
    DWORD dwDesiredAccess,
    BOOL  bInheritHandle,
    DWORD dwProcessId
    )
/*+
    A revert to self wrapper around OpenProcess
-*/
{
    HANDLE   hThreadToken = NULL;
    HANDLE   hRevertToken = NULL;
    HANDLE   hProcess     = NULL;
    BOOL     bImp         = FALSE;
    NTSTATUS Status       = STATUS_SUCCESS;

    //
    // If we are impersonating we must revert.
    // 

    Status = NtOpenThreadToken( 
                 NtCurrentThread(),
                 TOKEN_QUERY | TOKEN_IMPERSONATE,
                 TRUE,
                 &hThreadToken
                 );

    if (NT_SUCCESS(Status))
    {
        bImp = TRUE;

        //
        // Stop impersonating.
        //

        Status = NtSetInformationThread(
                     NtCurrentThread(),
                     ThreadImpersonationToken,
                     &hRevertToken,
                     sizeof(HANDLE)
                     );

        if (!NT_SUCCESS(Status))
        {
            BaseSetLastNTError(Status);
            goto Cleanup;
        }
        else
        {
            bImp = FALSE;
        }
    }
    else if (Status == STATUS_NO_TOKEN)
    {
        hThreadToken = NULL;
        bImp         = FALSE;
    }
    else
    {
        //
        // Why couldn't we open the thread token?
        //

        BaseSetLastNTError(Status);
        ASSERT(NT_SUCCESS(Status));
        goto Cleanup;
    }

    //
    // OpenProcess without impersonating.
    //

    hProcess = OpenProcess(
                   dwDesiredAccess,
                   bInheritHandle,
                   dwProcessId
                   );

    if (hThreadToken)
    {
        //
        // Continue to impersonate.
        //

        Status = NtSetInformationThread(
                     NtCurrentThread(),
                     ThreadImpersonationToken,
                     &hThreadToken,
                     sizeof(HANDLE)
                     );

        if (!NT_SUCCESS(Status))
        {
            BaseSetLastNTError(Status);
            goto Cleanup;
        }
        else
        {
            bImp = TRUE;
        }
    }

Cleanup:

    if (hThreadToken)
    {
        if (!bImp)
        {
            //
            // Continue to impersonate.
            //

            Status = NtSetInformationThread(
                         NtCurrentThread(),
                         ThreadImpersonationToken,
                         &hThreadToken,
                         sizeof(HANDLE)
                         );

            ASSERT(NT_SUCCESS(Status));
        }
        NtClose(hThreadToken);
    }

    return hProcess;
}


BOOL
L32CommonCreate(
    DWORD CreateFlags,
    HANDLE hToken,
    LPPROCESS_INFORMATION lpProcessInfo,
    LPSECURITY_ATTRIBUTES lpProcessAttributes,
    LPSECURITY_ATTRIBUTES lpThreadAttributes
    )
/*+
    This will do several tasks.
    
    1 create new security descriptors for the process / thread / token.
    2 create a new token for the new process
    3 assign security to that token
    4 put this new token on the new process
    5 assign security to the process and thread
    6 adjust quotas on the new process
    
-*/
{
    NTSTATUS Status = STATUS_SUCCESS;
    BOOL     b      = TRUE;

    PISECURITY_DESCRIPTOR pProcessSd = NULL;
    PISECURITY_DESCRIPTOR pThreadSd  = NULL;
    PISECURITY_DESCRIPTOR pTokenSd   = NULL;
    
    TOKEN_TYPE Type;
    DWORD      dwLength;
    BOOL       bUsingThreadToken = FALSE;
    BOOL       bUsingImpToken    = FALSE;
    HANDLE     hThreadToken      = NULL;  // the initial thread token, if any
    HANDLE     hNull             = NULL;  // token handle for reverting
    HANDLE     hTokenToAssign    = NULL;  // primary token to place on new process
    HANDLE     hImpToken         = NULL;  // impersonation version of hTokenToAssign

    OBJECT_ATTRIBUTES ObjectAttributes = {0};

    //
    // Please forgive me.
    //

    GENERIC_MAPPING ProcessMapping = {
        STANDARD_RIGHTS_READ |PROCESS_VM_READ | PROCESS_QUERY_INFORMATION,
        STANDARD_RIGHTS_WRITE |PROCESS_CREATE_PROCESS | PROCESS_CREATE_THREAD |PROCESS_VM_OPERATION | 
            PROCESS_VM_WRITE | PROCESS_DUP_HANDLE |PROCESS_TERMINATE | PROCESS_SET_QUOTA |
            PROCESS_SET_INFORMATION | PROCESS_SET_PORT,
        STANDARD_RIGHTS_EXECUTE | SYNCHRONIZE,
        PROCESS_ALL_ACCESS
    };

    GENERIC_MAPPING ThreadMapping = {
        STANDARD_RIGHTS_READ |THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
        STANDARD_RIGHTS_WRITE |THREAD_TERMINATE | THREAD_SUSPEND_RESUME | THREAD_ALERT |THREAD_SET_INFORMATION | THREAD_SET_CONTEXT,
        STANDARD_RIGHTS_EXECUTE |SYNCHRONIZE,
        THREAD_ALL_ACCESS
    };

    GENERIC_MAPPING TokenMapping = { 
        TOKEN_READ,
        TOKEN_WRITE,
        TOKEN_EXECUTE,
        TOKEN_ALL_ACCESS
    };

    //
    // Sanity.
    //

    if (lpProcessInfo->hProcess == NULL)
    {
        b = FALSE;
        BaseSetLastNTError(STATUS_INVALID_HANDLE);
        goto Cleanup;
    }

#ifdef ALLOW_IMPERSONATION_TOKENS
    HANDLE hTempToken = NULL;
#endif

    //
    // Determine type of token, since a non primary token will not work
    // on a process.  Now, we could duplicate it into a primary token,
    // and whack it into the process, but that leaves the process possibly
    // without credentials.
    //

    Status = NtQueryInformationToken(
                 hToken, 
                 TokenType,
                 (PUCHAR) &Type, 
                 sizeof(Type), 
                 &dwLength
                 );

    if (!NT_SUCCESS(Status))
    {
        b = FALSE;
        BaseSetLastNTError(Status);
        goto Cleanup;
    }

    if (Type != TokenPrimary)
    {
#ifdef ALLOW_IMPERSONATION_TOKENS
        //
        // Make this a primary token.
        //

        InitializeObjectAttributes(
                            &ObjectAttributes,
                            NULL,
                            0L,
                            NULL,
                            NULL);

        SecurityQualityOfService.Length              = sizeof(SECURITY_QUALITY_OF_SERVICE);
        SecurityQualityOfService.ImpersonationLevel  = SecurityImpersonation;
        SecurityQualityOfService.ContextTrackingMode = SECURITY_DYNAMIC_TRACKING;
        SecurityQualityOfService.EffectiveOnly       = FALSE;

        ObjectAttributes.SecurityQualityOfService = &SecurityQualityOfService;

        Status = NtDuplicateToken( 
                     hToken,
                     TOKEN_IMPERSONATE | TOKEN_QUERY,
                     &ObjectAttributes,
                     FALSE,
                     TokenPrimary,
                     &hTempToken
                     );

        if (!NT_SUCCESS(Status))
        {
            b = FALSE;
            BaseSetLastNTError(Status);
            goto Cleanup;
        }

        hToken = hTempToken;
#else   // !ALLOW_IMPERSONATION_TOKENS
        b = FALSE;
        Status = STATUS_BAD_TOKEN_TYPE;
        BaseSetLastNTError(Status);
        goto Cleanup;
#endif
    }

    //
    // Make our security descriptors grant ownership and permissions to the principal
    // represented by hToken.  We need a SD for the process, thread, and the token
    // that is getting placed on the new process.
    //

    //
    // If we are impersonating we must revert because CreatePrivateObjectSecurityEx will
    // call RtlpGetDefaultsSubjectContext, which will try to open the process token.  
    // The thread token (if it exists) will most likely not have this access.
    // 

    Status = NtOpenThreadToken( 
                 NtCurrentThread(),
                 TOKEN_QUERY | TOKEN_IMPERSONATE,
                 TRUE,
                 &hThreadToken
                 );

    if (NT_SUCCESS(Status))
    {
        //
        // Stop impersonating.
        //

        Status = NtSetInformationThread(
                     NtCurrentThread(),
                     ThreadImpersonationToken,
                     &hNull,
                     sizeof(HANDLE)
                     );

        if (!NT_SUCCESS(Status))
        {
            b = FALSE;
            BaseSetLastNTError(Status);
            goto Cleanup;
        }

        bUsingThreadToken = FALSE;
    }
    else if (Status == STATUS_NO_TOKEN)
    {
        hThreadToken      = NULL;
        bUsingThreadToken = FALSE;
    }
    else
    {
        //
        // Why couldn't we open the thread token?
        //

        ASSERT(NT_SUCCESS(Status));
        b = FALSE;
        BaseSetLastNTError(Status);
        goto Cleanup;
    }

    //
    // We should not be impersonating.
    //

    ASSERT(!bUsingThreadToken && !bUsingImpToken);

    //
    // Ignore the owner check as the owner in the passed SD may not be assignable as an owner
    // given our current security context.
    //
    
    b = CreatePrivateObjectSecurityEx(
            NULL,
            lpProcessAttributes ? lpProcessAttributes->lpSecurityDescriptor : NULL,
            &pProcessSd,
            NULL,
            FALSE,
            SEF_AVOID_OWNER_CHECK,
            hToken,
            &ProcessMapping
            );

    if (!b)
    {
        goto Cleanup;
    }

    b = CreatePrivateObjectSecurityEx(
            NULL,
            lpThreadAttributes ? lpThreadAttributes->lpSecurityDescriptor : NULL,
            &pThreadSd,
            NULL,
            FALSE,
            SEF_AVOID_OWNER_CHECK,
            hToken,
            &ThreadMapping
            );

    if (!b)
    {
        goto Cleanup;
    }

    b = CreatePrivateObjectSecurityEx(
            NULL,
            NULL,
            &pTokenSd,
            NULL,
            FALSE,
            SEF_AVOID_OWNER_CHECK,
            hToken,
            &TokenMapping
            );
    
    if (!b)
    {
        goto Cleanup;
    }

    //
    // We need an impersonation version of hToken so that we can later assign
    // these SDs to the process and threads.  The SDs we created specify hToken
    // as the owner (assuming no passed SDs with owners), so only a thread 
    // impersonating as hToken can assign them to objects successfully 
    // (else we fail with INVALID_OWNER).
    //

    b = DuplicateTokenEx(
            hToken,
            TOKEN_QUERY | TOKEN_IMPERSONATE,
            NULL,
            SecurityImpersonation,
            TokenImpersonation,
            &hImpToken
            );

    if (!b)
    {
        goto Cleanup;
    }

    //
    // Create a new token to put on the process.  Make this a duplicate of
    // the passed hToken.  We are not impersonating here.
    //

    ASSERT(!bUsingThreadToken && !bUsingImpToken);

    b = L32CreateTokenForNewProcess( 
            pTokenSd, 
            hToken, 
            &hTokenToAssign 
            );
    
    if (!b)
    {
        //
        // Try again under impersonation: if the Owner in pTokenSd isn't assignable 
        // when we run as the process, it will work with the new impersonation 
        // version of hToken (since pTokenSd states that the hToken principal is to 
        // be assigned as the Owner of hTokenToAssign).
        //

        Status = NtSetInformationThread(
                     NtCurrentThread(),
                     ThreadImpersonationToken,
                     &hImpToken,
                     sizeof(HANDLE)
                     );

        if (!NT_SUCCESS(Status))
        {
            b = FALSE;
            BaseSetLastNTError(Status);
            goto Cleanup;
        }

        bUsingImpToken = TRUE;

        b = L32CreateTokenForNewProcess( 
                pTokenSd, 
                hToken, 
                &hTokenToAssign 
                );
    
        if (!b)
        {
            goto Cleanup;
        }

        //
        // Revert.
        //

        Status = NtSetInformationThread(
                     NtCurrentThread(),
                     ThreadImpersonationToken,
                     &hNull,
                     sizeof(HANDLE)
                     );

        if (!NT_SUCCESS(Status))
        {
            b = FALSE;
            BaseSetLastNTError(Status);
            goto Cleanup;
        }

        bUsingImpToken = FALSE;
    }
        
    //
    // Now hTokenToAssign exists as a duplicate of hToken and it has proper security 
    // which grants access and ownership to the hToken principal.  Set the primary token 
    // of the new process to be hTokenToAssign. Try this first without impersonating, 
    // since the current process currently owns this new process.
    //

    ASSERT(!bUsingThreadToken && !bUsingImpToken);

    b = L32SetProcessToken(
            lpProcessInfo->hProcess,
            lpProcessInfo->hThread,
            hTokenToAssign,
            FALSE
            );

    if (!b)                       
    {
        if (hThreadToken)
        {
            //
            // Try again as the original thread principal.  We aren't trying as the
            // hToken principal because that would allow anyone to create a process
            // as any user with the correct privileges (assuming that they could get
            // ahold of said user's token).  If the thread token had the assign primary
            // privilege then we will succeed.
            //

            Status = NtSetInformationThread(
                         NtCurrentThread(),
                         ThreadImpersonationToken,
                         &hThreadToken,
                         sizeof(HANDLE)
                         );

            if (!NT_SUCCESS(Status))
            {
                b = FALSE;
                BaseSetLastNTError(Status);
                goto Cleanup;
            }

            bUsingThreadToken = TRUE;

            b = L32SetProcessToken(
                    lpProcessInfo->hProcess,
                    lpProcessInfo->hThread,
                    hTokenToAssign,
                    TRUE
                    );
    
            if (!b)
            {
                goto Cleanup;
            }

            //
            // Revert.
            //

            Status = NtSetInformationThread(
                         NtCurrentThread(),
                         ThreadImpersonationToken,
                         &hNull,
                         sizeof(HANDLE)
                         );

            if (!NT_SUCCESS(Status))
            {
                b = FALSE;
                BaseSetLastNTError(Status);
                goto Cleanup;
            }

            bUsingThreadToken = FALSE;
        }
        else
        {
            //
            // The process doesn't have rights to assign the new process
            // a primary token, and we weren't called with an impersonation
            // context.  We must give up.
            //

            goto Cleanup;
        }
    }

    //
    // Adjust the quota to something reasonable.
    //

    ASSERT(!bUsingThreadToken && !bUsingImpToken);

    b = L32SetProcessQuotas(
            lpProcessInfo->hProcess,
            FALSE 
            );

    if (!b)
    {
        if (hThreadToken)
        {
            //
            // If we failed to adjust quota as the process then try 
            // while impersonating as the original thread token.
            //

            Status = NtSetInformationThread(
                         NtCurrentThread(),
                         ThreadImpersonationToken,
                         &hThreadToken,
                         sizeof(HANDLE)
                         );

            if (!NT_SUCCESS(Status))
            {
                b = FALSE;
                BaseSetLastNTError(Status);
                goto Cleanup;
            }
        
            bUsingThreadToken = TRUE;

            b = L32SetProcessQuotas(
                    lpProcessInfo->hProcess,
                    TRUE
                    );
    
            if (!b)
            {
                goto Cleanup;
            }

            //
            // Revert.
            //

            Status = NtSetInformationThread(
                         NtCurrentThread(),
                         ThreadImpersonationToken,
                         &hNull,
                         sizeof(HANDLE)
                         );

            if (!NT_SUCCESS(Status))
            {
                b = FALSE;
                BaseSetLastNTError(Status);
                goto Cleanup;
            }
        
            bUsingThreadToken = FALSE;
        }
        else
        {
            //
            // We cannot adjust the quota as the process, and we were
            // not called while impersonating.  Fail.
            //

            goto Cleanup;
        }
    }

    //
    // We should not be impersonating here.
    //

    ASSERT(!bUsingThreadToken && !bUsingImpToken);

    //
    // Now put the correct SD on the process / thread.
    //

    b = SetKernelObjectSecurity( 
            lpProcessInfo->hProcess, 
            GROUP_SECURITY_INFORMATION | OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
            pProcessSd
            );
    
    if (!b)
    {
        //
        // If we failed above the cause was most likely because the owner ID in 
        // the SD does not exist as an assignable owner ID in the current process token.  
        // Impersonating as hImpToken will take care of this.
        //

        Status = NtSetInformationThread(
                        NtCurrentThread(),
                        ThreadImpersonationToken,
                        &hImpToken,
                        sizeof(HANDLE)
                        );

        if (!NT_SUCCESS(Status))
        {
            b = FALSE;
            BaseSetLastNTError(Status);
            goto Cleanup;
        }

        bUsingImpToken = TRUE;
        
        //
        // Try again as hImpToken.
        //

        b = SetKernelObjectSecurity( 
                lpProcessInfo->hProcess, 
                GROUP_SECURITY_INFORMATION | OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                pProcessSd
                );
        
        if (!b)
        {
            goto Cleanup;
        }

        //
        // Revert.
        //

        Status = NtSetInformationThread(
                     NtCurrentThread(),
                     ThreadImpersonationToken,
                     &hNull,
                     sizeof(HANDLE)
                     );

        if (!NT_SUCCESS(Status))
        {
            b = FALSE;
            BaseSetLastNTError(Status);
            goto Cleanup;
        }

        bUsingImpToken = FALSE;
    }

    //
    // Now put it on the thread.
    //

    b = SetKernelObjectSecurity( 
            lpProcessInfo->hThread, 
            GROUP_SECURITY_INFORMATION | OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
            pThreadSd
            );
    
    if (!b)
    {
        //
        // If we failed above the cause was most likely because the owner ID in 
        // the SD does not exist as an assignable owner ID in the current process token.  
        // Impersonating as hImpToken will take care of this.
        //

        Status = NtSetInformationThread(
                        NtCurrentThread(),
                        ThreadImpersonationToken,
                        &hImpToken,
                        sizeof(HANDLE)
                        );

        if (!NT_SUCCESS(Status))
        {
            b = FALSE;
            BaseSetLastNTError(Status);
            goto Cleanup;
        }

        bUsingImpToken = TRUE;
        
        //
        // Try again as hImpToken.
        //

        b = SetKernelObjectSecurity( 
                lpProcessInfo->hThread, 
                GROUP_SECURITY_INFORMATION | OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                pThreadSd
                );
        
        if (!b)
        {
            goto Cleanup;
        }

        //
        // Revert.
        //

        Status = NtSetInformationThread(
                     NtCurrentThread(),
                     ThreadImpersonationToken,
                     &hNull,
                     sizeof(HANDLE)
                     );

        if (!NT_SUCCESS(Status))
        {
            b = FALSE;
            BaseSetLastNTError(Status);
            goto Cleanup;
        }

        bUsingImpToken = FALSE;
    }

    //
    // If we're not supposed to leave it suspended then resume the
    // thread and let it run.
    //

    if ((CreateFlags & COMMON_CREATE_SUSPENDED) == 0)
    {
        ResumeThread(lpProcessInfo->hThread);
    }

    //
    // That's it! 
    //

    goto Cleanup;

Cleanup:

#ifdef ALLOW_IMPERSONATION_TOKENS
    if (hTempToken)
    {
        NtClose(hTempToken);
    }
#endif
    
    //
    // Free our new security descriptors.
    //

    if (pTokenSd)
    {
        DestroyPrivateObjectSecurity(&pTokenSd);
    }

    if (pProcessSd)
    {
        DestroyPrivateObjectSecurity(&pProcessSd);
    }
    
    if (pThreadSd)
    {
        DestroyPrivateObjectSecurity(&pThreadSd);
    }
    
    if (hTokenToAssign)
    {
        NtClose(hTokenToAssign);
    }

    //
    // If we are using the newly created impersonation token
    // then revert.
    //

    if (hImpToken)
    {
        if (bUsingImpToken)
        {
            Status = NtSetInformationThread(
                         NtCurrentThread(),
                         ThreadImpersonationToken,
                         &hNull,
                         sizeof(HANDLE)
                         );

            //
            // We should only ASSERT here because we don't want to 
            // overwrite our real error codes.
            //

            ASSERT(NT_SUCCESS(Status));
        }
        NtClose(hImpToken);
    }
    
    //
    // Resume original impersonation if that is how we were called.
    //

    if (hThreadToken)
    {
        if (!bUsingThreadToken)
        {
            Status = NtSetInformationThread(
                         NtCurrentThread(),
                         ThreadImpersonationToken,
                         &hThreadToken,
                         sizeof(HANDLE)
                         );

            //
            // We should only ASSERT here because we don't want to 
            // overwrite our real error codes.
            //

            ASSERT(NT_SUCCESS(Status));
        }
        NtClose(hThreadToken);
    }

    if (!b)
    {
        if (lpProcessInfo->hProcess)
        {
            NtTerminateProcess(
                lpProcessInfo->hProcess,
                ERROR_ACCESS_DENIED
                );
            NtClose(lpProcessInfo->hProcess);
        }
        if (lpProcessInfo->hThread)
        {
            NtClose(lpProcessInfo->hThread);
        }
        RtlZeroMemory( 
            lpProcessInfo, 
            sizeof(PROCESS_INFORMATION) 
            );
    }
    return b;
}


//+---------------------------------------------------------------------------
//
//  Function:   SaferiReplaceProcessThreadTokens
//
//  Synopsis:
//      Provides a privately exported function to replace the access token
//      of a process and its primary thread of a new process before its
//      execution has begun.  The process is left in a suspended state
//      after the token modification has been performed.
//
//  Effects:
//
//  Arguments:  [NewTokenHandle]  -- Primary token to use
//              [ProcessHandle]   -- Process handle
//              [ThreadHandle]    -- Handle of process's primary Thread
//
//  History:    8-25-2000   JLawson   Created
//
//  Notes:
//      This is merely a wrapper function that calls L32CommonCreate.
//
//----------------------------------------------------------------------------
BOOL
WINAPI
SaferiReplaceProcessThreadTokens(
        IN HANDLE       NewTokenHandle,
        IN HANDLE       ProcessHandle,
        IN HANDLE       ThreadHandle
        )
{
    PROCESS_INFORMATION TempProcessInfo;

    RtlZeroMemory( &TempProcessInfo, sizeof( PROCESS_INFORMATION ) );
    TempProcessInfo.hProcess = ProcessHandle;
    TempProcessInfo.hThread = ThreadHandle;
    return (L32CommonCreate(
            COMMON_CREATE_PROCESSSD | COMMON_CREATE_THREADSD | COMMON_CREATE_SUSPENDED,
            NewTokenHandle, 
            &TempProcessInfo, 
            NULL, 
            NULL));
}


//+---------------------------------------------------------------------------
//
//   MarshallString
//
//    Marshall in a UNICODE_NULL terminated WCHAR string
//
//  ENTRY:
//    pSource (input)
//      Pointer to source string
//
//    pBase (input)
//      Base buffer pointer for normalizing the string pointer
//
//    MaxSize (input)
//      Maximum buffer size available
//
//    ppPtr (input/output)
//      Pointer to the current context pointer in the marshall buffer.
//      This is updated as data is marshalled into the buffer
//
//    pCount (input/output)
//      Current count of data in the marshall buffer.
//      This is updated as data is marshalled into the buffer
//
//  EXIT:
//    NULL - Error
//    !=NULL "normalized" pointer to the string in reference to pBase
//
//+---------------------------------------------------------------------------
PWCHAR
MarshallString(
    PCWSTR pSource,
    PCHAR  pBase,
    ULONG  MaxSize,
    PCHAR  *ppPtr,
    PULONG pCount
    )
{
    ULONG Len;
    PCHAR ptr;

    Len = wcslen( pSource );
    Len++; // include the NULL;

    Len *= sizeof(WCHAR); // convert to bytes
    if( (*pCount + Len) > MaxSize ) {
        return( NULL );
    }

    RtlMoveMemory( *ppPtr, pSource, Len );

    //
    // the normalized ptr is the current count
    //
        // Sundown note: ptr is a zero-extension of *pCount.
    ptr = (PCHAR)ULongToPtr(*pCount);

    *ppPtr += Len;
    *pCount += Len;

    return((PWCHAR)ptr);
}

#if DBG

void DumpOutLastErrorString()
{
    LPVOID  lpMsgBuf;

    FormatMessage(
            FORMAT_MESSAGE_ALLOCATE_BUFFER |
            FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
            NULL,
            GetLastError(),
            MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), // Default language
            (LPTSTR) &lpMsgBuf,
            0,
            NULL
        );
        //
        // Process any inserts in lpMsgBuf.
        // ...
        // Display the string.
        //
        KdPrint(("%s\n", (LPCTSTR)lpMsgBuf ));

        //
        // Free the buffer.
        //
        LocalFree( lpMsgBuf );
}
#endif

#ifdef DBG
#define    DBG_DumpOutLastError    DumpOutLastErrorString();
#else
#define    DBG_DumpOutLastError
#endif


//+---------------------------------------------------------------------------
//
// This function was originally defined in  \nt\private\ole32\dcomss\olescm\execclt.cxx
//
// CreateRemoteSessionProcessW()
//
//  Create a process on the given Terminal Server Session. This is in UNICODE
//
// ENTRY:
//  SessionId (input)
//    SessionId of Session to create process on
//
//  Param1 (input/output)
//    Comments
//
// Comments
//  The security attribs are not used by the session, they are set to NULL
//  We may consider to extend this feature in the future, assuming there is a
//  need for it.
//
// EXIT:
//  STATUS_SUCCESS - no error
//+---------------------------------------------------------------------------
BOOL
CreateRemoteSessionProcessW(
    ULONG  SessionId,
    BOOL   System,
    HANDLE hToken,
    PCWSTR lpszImageName,
    PCWSTR lpszCommandLine,
    PSECURITY_ATTRIBUTES psaProcess,    // these are ignored on the session side, set to NULL
    PSECURITY_ATTRIBUTES psaThread,     // these are ignored on the session side, set to NULL
    BOOL   fInheritHandles,
    DWORD  fdwCreate,
    LPVOID lpvEnvionment,
    LPCWSTR lpszCurDir,
    LPSTARTUPINFOW pStartInfo,
    LPPROCESS_INFORMATION pProcInfo
    )
{
    BOOL            Result = TRUE;
    HANDLE          hPipe = NULL;
    WCHAR           szPipeName[EXECSRVPIPENAMELEN];
    PCHAR           ptr;
    ULONG           Count, AmountWrote, AmountRead;
    DWORD           MyProcId;
    PEXECSRV_REQUEST pReq;
    EXECSRV_REPLY   Rep;
    CHAR            Buf[EXECSRV_BUFFER_SIZE];
    ULONG           MaxSize = EXECSRV_BUFFER_SIZE;
    DWORD           rc;
    LPVOID          lpMsgBuf;
    ULONG           envSize=0;  // size of the lpEnvironemt, if any
    PWCHAR          lpEnv;
    ULONG           ReturnLen;

#if DBG
    if( lpszImageName )
        KdPrint(("logon32.c: CreateRemoteSessionProcessW: lpszImageName %ws\n",lpszImageName));

    if( lpszCommandLine )
        KdPrint(("logon32.c: CreateRemoteSessionProcessW: lpszCommandLine %ws\n",lpszCommandLine));
#endif

    //
    // Winlogon handles all now. System flag tells it what to do
    //
    Result = WinStationQueryInformation( NULL, SessionId, WinStationExecSrvSystemPipe, szPipeName, sizeof(szPipeName), &ReturnLen );
    if ( !Result ) {
        KdPrint(("logon32.c: WinStationQueryInformation for the EXECSRV pipe name failed\n"));
        return(FALSE);
    }

    while ( TRUE )
    {
        hPipe = CreateFileW(
                    szPipeName,
                    GENERIC_READ|GENERIC_WRITE,
                    0,    // File share mode
                    NULL, // default security
                    OPEN_EXISTING,
                    0,    // Attrs and flags
                    NULL  // template file handle
                    );

        if( hPipe == INVALID_HANDLE_VALUE )
        {
            if (GetLastError() == ERROR_PIPE_BUSY)
            {
                if (!WaitNamedPipeW( szPipeName, 30000 ))
                { // 30 sec
                    KdPrint(("logon32.c: Waited too long for pipe name %ws\n", szPipeName));
                    return(FALSE);
                }
            }
            else
            {
                DBG_DumpOutLastError;
                KdPrint(("logon32.c: Could not create pipe name %ws\n", szPipeName));
                return(FALSE);
            }
        }
        else
        {
            break;
        }
    }


    //
    // Get the handle to the current process
    //
    MyProcId = GetCurrentProcessId();

    //
    // setup the marshalling
    //
    ptr = Buf;
    Count = 0;

    pReq = (PEXECSRV_REQUEST)ptr;
    ptr   += sizeof(EXECSRV_REQUEST);
    Count += sizeof(EXECSRV_REQUEST);

    //
    // set the basic parameters
    //
    pReq->System = System;
    pReq->hToken = hToken;
    pReq->RequestingProcessId = MyProcId;
    pReq->fInheritHandles = fInheritHandles;
    pReq->fdwCreate = fdwCreate;

    //
    // marshall the ImageName string
    //
    if( lpszImageName ) {
        pReq->lpszImageName = MarshallString( lpszImageName, Buf, MaxSize, &ptr, &Count );
        if (! pReq->lpszImageName)
        {
            Result = FALSE;
            goto Cleanup;
        }
    }
    else {
        pReq->lpszImageName = NULL;
    }

    //
    // marshall in the CommandLine string
    //
    if( lpszCommandLine ) {
        pReq->lpszCommandLine = MarshallString( lpszCommandLine, Buf, MaxSize, &ptr, &Count );
        if ( ! pReq->lpszCommandLine )
        {
            Result = FALSE;
            goto Cleanup;
        }
    }
    else {
        pReq->lpszCommandLine = NULL;
    }

    //
    // marshall in the CurDir string
    //
    if( lpszCurDir ) {
        pReq->lpszCurDir = MarshallString( lpszCurDir, Buf, MaxSize, &ptr, &Count );
        if ( ! pReq->lpszCurDir  )
        {
            Result = FALSE;
            goto Cleanup;
        }
    }
    else {
        pReq->lpszCurDir = NULL;
    }

    //
    // marshall in the StartupInfo structure
    //
    RtlMoveMemory( &pReq->StartInfo, pStartInfo, sizeof(STARTUPINFO) );

    //
    // Now marshall the strings in STARTUPINFO
    //
    if( pStartInfo->lpDesktop ) {
        pReq->StartInfo.lpDesktop = MarshallString( pStartInfo->lpDesktop, Buf, MaxSize, &ptr, &Count );
        if (! pReq->StartInfo.lpDesktop )
        {
            Result = FALSE;
            goto Cleanup;
        }
    }
    else {
        pReq->StartInfo.lpDesktop = NULL;
    }

    if( pStartInfo->lpTitle ) {
        pReq->StartInfo.lpTitle = MarshallString( pStartInfo->lpTitle, Buf, MaxSize, &ptr, &Count );
        if ( !pReq->StartInfo.lpTitle  )
        {
            Result = FALSE;
            goto Cleanup;
        }
    }
    else {
        pReq->StartInfo.lpTitle = NULL;
    }

    //
    // WARNING: This version does not pass the following:
    //
    //  Also saProcess and saThread are ignored right now and use
    //  the users default security on the remote WinStation
    //
    // Set things that are always NULL
    //
    pReq->StartInfo.lpReserved = NULL;  // always NULL


    if ( lpvEnvionment)
    {
        for ( lpEnv = (PWCHAR) lpvEnvionment;
            (*lpEnv ) && (envSize + Count < MaxSize ) ;  lpEnv++)
        {
            while( *lpEnv )
            {
                lpEnv++;
                envSize += 2;   // we are dealing with wide chars
                if ( envSize+Count >= MaxSize )
                {
                    // we have too many
                    // vars in the user's profile.
                    KdPrint(("\tEnv length too big = %d \n", envSize));
                    break;
                }
            }
            // this is the null which marked the end of the last env var.
            envSize +=2;

        }
        envSize += 2;    // this is the final NULL


        if ( Count + envSize < MaxSize )
        {
            RtlMoveMemory( (PCHAR)&Buf[Count] ,lpvEnvionment, envSize );
                        // SUNDOWN: Count is zero-extended and store in lpvEnvironment.
            //          This zero-extension is valid. The consuming code [see tsext\notify\execsrv.c]
            //          considers lpvEnvironment as an offset (<2GB).
            pReq->lpvEnvironment = (PCHAR)ULongToPtr(Count);
            ptr += envSize;         // for the next guy
            Count += envSize;       // the count used so far
        }
        else    // no room left to make a complete copy
        {
            pReq->lpvEnvironment = NULL;
        }

    }
    else
    {
        pReq->lpvEnvironment = NULL;
    }

    //
    // now fill in the total count
    //
    pReq->Size = Count;

#if DBG
    KdPrint(("pReq->Size = %d, envSize = %d \n", pReq->Size , envSize ));
#endif

    //
    // Now send the buffer out to the server
    //
    Result = WriteFile(
                 hPipe,
                 Buf,
                 Count,
                 &AmountWrote,
                 NULL
                 );

    if( !Result ) {
        KdPrint(("logon32.c: Error %d sending request\n",GetLastError() ));
        goto Cleanup;
    }

    //
    // Now read the reply
    //
    Result = ReadFile(
                 hPipe,
                 &Rep,
                 sizeof(Rep),
                 &AmountRead,
                 NULL
                 );

    if( !Result ) {
        KdPrint(("logon32.c: Error %d reading reply\n",GetLastError()));
        goto Cleanup;
    }

    //
    // Check the result
    //
    if( !Rep.Result ) {
        KdPrint(("logon32.c: Error %d in reply\n",Rep.LastError));
        //
        // set the error in the current thread to the returned error
        //
        Result = Rep.Result;
        SetLastError( Rep.LastError );
        goto Cleanup;
    }

    //
    // We copy the PROCESS_INFO structure from the reply
    // to the caller.
    //
    // The remote site has duplicated the handles into our
    // process space for hProcess and hThread so that they will
    // behave like CreateProcessW()
    //

     RtlMoveMemory( pProcInfo, &Rep.ProcInfo, sizeof( PROCESS_INFORMATION ) );

Cleanup:
    CloseHandle(hPipe);

   KdPrint(("logon32.c:: Result 0x%x\n", Result));

    return(Result);
}


//+---------------------------------------------------------------------------
//
//  Function:   CreateProcessAsUserW
//
//  Synopsis:   Creates a process running as the user in hToken.
//
//  Arguments:  [hToken]               -- Handle to a Primary Token to use
//              [lpApplicationName]    -- as CreateProcess() q.v.
//              [lpCommandLine]        --
//              [lpProcessAttributes]  --
//              [lpThreadAttributes]   --
//              [bInheritHandles]      --
//              [dwCreationFlags]      --
//              [lpEnvironment]        --
//              [lpCurrentDirectory]   --
//              [lpStartupInfo]        --
//              [lpProcessInformation] --
//
//  Return Values
//          If the function succeeds, the return value is nonzero.
//          If the function fails, the return value is zero. To get extended error information, call GetLastError.
//
//  History:    4-25-95   RichardW   Created
//              1-14-98     AraBern     add changes for Hydra
//  Notes:
//
//
//----------------------------------------------------------------------------
BOOL
WINAPI
CreateProcessAsUserW(
    HANDLE  hToken,
    LPCWSTR lpApplicationName,
    LPWSTR lpCommandLine,
    LPSECURITY_ATTRIBUTES lpProcessAttributes,
    LPSECURITY_ATTRIBUTES lpThreadAttributes,
    BOOL bInheritHandles,
    DWORD dwCreationFlags,
    LPVOID lpEnvironment,
    LPCWSTR lpCurrentDirectory,
    LPSTARTUPINFOW lpStartupInfo,
    LPPROCESS_INFORMATION lpProcessInformation
    )
{
    DWORD    CreateFlags;
    DWORD    clientSessionID=0;
    DWORD    currentSessionID=0;
    DWORD    resultLength;
    HANDLE   hTmpToken;
    DWORD    curProcId ;
    NTSTATUS Status ;

    CreateFlags = (dwCreationFlags & CREATE_SUSPENDED ? COMMON_CREATE_SUSPENDED : 0);

    //
    // get the sessionID (if zero then it means that we are on the console).
    //
    currentSessionID = NtCurrentPeb()->SessionId;

    if ( !GetTokenInformation ( hToken, TokenSessionId , &clientSessionID,sizeof( DWORD), &resultLength ) )
    {
    //
    // get the access token for the client of this call
    // get token instead of process since the client might have only
    // impersonated the thread, not the process
    //
        DBG_DumpOutLastError;
        ASSERT( FALSE );
        currentSessionID = 0;

        //
        // We should probably return FALSE here, but at this time we don't want to alter the
        // non-Hydra code-execution-flow at all.
        //
    }

    // KdPrint(("logon32.c: CreateProcessAsUserW(): clientSessionID = %d, currentSessionID = %d \n",
    //    clientSessionID, currentSessionID ));

    if (  clientSessionID != currentSessionID )
    {
        //
        // If the client session ID is not the same as the current session ID, then, we are attempting
        // to create a process on a remote session from the current session.
        // This block of code is used to accomplish such process creation, it is Terminal-Server specific
        //

        BOOL        bHaveImpersonated;
        HANDLE      hCurrentThread;
        HANDLE      hPrevToken = NULL;
        DWORD       rc;
        TOKEN_TYPE  tokenType;

        //
        // We must send the request to the remote session
        // of the requestor
        //
        // NOTE: The current WinStationCreateProcessW() does not use
        //       the supplied security descriptor, but creates the
        //       process under the account of the logged on user.
        //

        //
        // Stop impersonating before doing the WinStationCreateProcess.
        // The remote winstation exec thread will launch the app under
        // the users context. We must not be impersonating because this
        // call only lets SYSTEM request the remote execute.
        //

        //
        // Handle Inheritance is not allowed for cross session process creation
        //
        if (bInheritHandles) {

          SetLastError(ERROR_INVALID_PARAMETER);

          return FALSE;
        }

        hCurrentThread = GetCurrentThread();

        //
        // Init bHaveImpersonated to the FALSE state
        //
        bHaveImpersonated = FALSE;

        //
        // Since the caller of this function (runas-> SecLogon service ) has already
        // impersonated the new (target) user, we do the OpenThreadToken with
        // OpenAsSelf = TRUE
        //
        if ( OpenThreadToken( hCurrentThread, TOKEN_QUERY | TOKEN_IMPERSONATE , TRUE, &hPrevToken ) )
        {

            bHaveImpersonated = TRUE;

            if ( !RevertToSelf() )
            {
                return FALSE;
            }
        }


       //
       // else, we are not impersonating, as reflected by the init value of bHaveImpersonated
       //

        rc = CreateRemoteSessionProcessW(
                clientSessionID,
                FALSE,     // not creating a process for System
                hToken,
                lpApplicationName,
                lpCommandLine,
                lpProcessAttributes,
                lpThreadAttributes,
                bInheritHandles,
                dwCreationFlags,
                lpEnvironment,
                lpCurrentDirectory,
                lpStartupInfo,
                lpProcessInformation) ;

        //
        // Undo the effect of RevertToSelf() if we had impersoanted
        //
        if ( bHaveImpersonated )
        {
            Status = NtSetInformationThread(
                        NtCurrentThread(),
                        ThreadImpersonationToken,
                        &hPrevToken,
                        sizeof( hPrevToken ) );

            ASSERT( NT_SUCCESS(Status ) );

            NtClose( hPrevToken );
        }

        if ( rc )
        {
            return TRUE;
        }
        else
        {
            return FALSE;
        }

    }
    else
    //
    // this is the standard non-Hydra related call block
    //
    {
        HANDLE hRestrictedToken = NULL;
        BOOL b = FALSE;

        if (!CreateProcessInternalW(hToken,
                                    lpApplicationName,
                                    lpCommandLine,
                                    lpProcessAttributes,
                                    lpThreadAttributes,
                                    bInheritHandles,
                                    dwCreationFlags | CREATE_SUSPENDED ,                               
                                    lpEnvironment,
                                    lpCurrentDirectory,
                                    lpStartupInfo,
                                    lpProcessInformation,
                                    &hRestrictedToken))
        {
            //
            // The internal routine might return a token even in the failure case
            // since it uses try-finally. Free the token if needed.
            //

            if (hRestrictedToken != NULL)
            {
                NtClose(hRestrictedToken); 
            } 
            return(FALSE);
        }

        CreateFlags |= (lpProcessAttributes ? 0 : COMMON_CREATE_PROCESSSD);
        CreateFlags |= (lpThreadAttributes ? 0 : COMMON_CREATE_THREADSD);
        
        if(lpProcessInformation->dwProcessId != 0) {
           
           HANDLE VdmWaitHandle = NULL;           

           //
           // Check if it is a shared wow being started
           //
           
           if((ULONG_PTR)lpProcessInformation->hProcess & 0x2) {        

               VdmWaitHandle = lpProcessInformation->hProcess;
               lpProcessInformation->hProcess = OpenProcess(PROCESS_ALL_ACCESS,
                                                            FALSE,
                                                            lpProcessInformation->dwProcessId);

               if (lpProcessInformation->hProcess == NULL)
               {
                   //
                   // Couldn't open it.  Try reverting since the new process gets security
                   // from the process token.
                   //

                   lpProcessInformation->hProcess = L32RevertOpenProcess(PROCESS_ALL_ACCESS,
                                                                         FALSE,
                                                                         lpProcessInformation->dwProcessId);

                   ASSERT(lpProcessInformation->hProcess);
               }
           }
                                                           

           
           //                                                                                   
           // If a restricted token was returned, set it on the process.                        
           // Else use the token provided by the caller.                                        
           //                                                                                   
                                                                                                
           if (hRestrictedToken == NULL)                                                        
           {   
               b = (L32CommonCreate(CreateFlags, hToken, lpProcessInformation, lpProcessAttributes, lpThreadAttributes));                
           }                                                                                    
           else                                                                                 
           {                                                                                    
               b = (L32CommonCreate(CreateFlags, hRestrictedToken, lpProcessInformation, lpProcessAttributes, lpThreadAttributes));      
           }                                                                                    

           //
           // if L32CommonCreate didn't succeed, it closes lpProcessInformation->hProcess and
           // zeros out lpProcessInformation, so we shouldn't be closing it again if it didn't 
           // succeed.           
                                                                                                
           if(b && VdmWaitHandle) {                                                                  
              if(lpProcessInformation->hProcess) {                                                                           
                 NtClose(lpProcessInformation->hProcess);                                       
              }                                                                                 
              lpProcessInformation->hProcess = VdmWaitHandle;                                   
           }                                                                                    

        }
        else {
           b = TRUE;
        }
        
        if (hRestrictedToken) {
           NtClose(hRestrictedToken);
        }

        return b;
    }
}



/***************************************************************************\
* OemToCharW
*
* OemToCharW(pSrc, pDst) - Translates the OEM string at pSrc into
* the Unicode string at pDst.  pSrc == pDst is not legal.
*
* History:
*   This function was copied from NT\windows\Core\ntuser\client\oemxlate.c
*
\***************************************************************************/
BOOL WINAPI ConvertOemToCharW(
    LPCSTR pSrc,
    LPWSTR pDst)
{
    int cch;
    if (pSrc == NULL || pDst == NULL) {
        return FALSE;
    } else if (pSrc == (LPCSTR)pDst) {
        /*
         * MultiByteToWideChar() requires pSrc != pDst: fail this call.
         * LATER: Is this really true?
         */
        return FALSE;
    }

    cch = strlen(pSrc) + 1;

    MultiByteToWideChar(
            CP_OEMCP,                          // Unicode -> OEM
            MB_PRECOMPOSED | MB_USEGLYPHCHARS, // visual map to precomposed
            (LPSTR)pSrc, cch,                  // source & length
            pDst,                              // destination
            cch);                              // max poss. precomposed length

    return TRUE;
}


//----------------------------------------------------------------------------
//
// Function:    OemToCharW_WithAllocation()
//
// Synopsis:    This func will allocated memory for the string ppDst which
//              must be then deallocatd thru a call to LocalFree().
//              If the passed in ansi string is NULL, then no memory
//              is allocated, and a NULL is returned
//
// Arguments:
//              LPCSTR  [in]    ansi string for which we want the wide version
//              *LPWSTR [out]   the wide version of ansi string
// Return:
//      BOOL : TRUE if no errors.
//      BOOL : FALSE if unable to allocated memory.
//
//----------------------------------------------------------------------------
BOOL WINAPI OemToCharW_WithAllocation(  LPCSTR pSrc,
    LPWSTR *ppDst)
{
    DWORD size;

    if (pSrc)
    {
        size = strlen( pSrc );

        *ppDst = ( WCHAR *) LocalAlloc(LMEM_FIXED, ( size + 1 ) * sizeof( WCHAR ) );

        if ( ppDst )
        {
            ConvertOemToCharW( pSrc, *ppDst );
            return TRUE;
        }
        else
            return FALSE;
    }
    else
    {
        *ppDst = NULL;
        return TRUE;
    }

}


//  ANSI wrapper for CreateRemoteSessionProcessW()
//
BOOL
CreateRemoteSessionProcessA(
    ULONG  SessionId,
    BOOL   System,
    HANDLE  hToken,
    LPCSTR lpApplicationName,
    LPSTR lpCommandLine,
    LPSECURITY_ATTRIBUTES lpProcessAttributes,
    LPSECURITY_ATTRIBUTES lpThreadAttributes,
    BOOL bInheritHandles,
    DWORD dwCreationFlags,
    LPVOID lpEnvironment,
    LPCSTR lpCurrentDirectory,
    LPSTARTUPINFOA lpStartupInfo,
    LPPROCESS_INFORMATION lpProcessInformation
)
{
    NTSTATUS                st;
    BOOL                    rc,rc2;
    STARTUPINFOW            WCHAR_StartupInfo;
    PWCHAR                  pWCHAR_AppName, pWCHAR_CommandLine, pWCHAR_CurDir, pWCHAR_Title, pWCHAR_Desktop;

    pWCHAR_AppName = pWCHAR_CommandLine =  pWCHAR_CurDir = pWCHAR_Title =  pWCHAR_Desktop = NULL;

    // in case there is a premature return from this function.
    rc2 = FALSE;

    if ( !( rc = OemToCharW_WithAllocation( lpApplicationName , &pWCHAR_AppName ) ))
    {
        goto Cleanup;
    }

    if ( !( rc = OemToCharW_WithAllocation( lpCommandLine , &pWCHAR_CommandLine ) ))
    {
        goto Cleanup;
    }

    if ( !( rc = OemToCharW_WithAllocation( lpCurrentDirectory , &pWCHAR_CurDir ) ))
    {
        goto Cleanup;
    }

    if ( !( rc = OemToCharW_WithAllocation( lpStartupInfo->lpTitle , &pWCHAR_Title ) ))
    {
        goto Cleanup;
    }

    if ( !( rc = OemToCharW_WithAllocation( lpStartupInfo->lpDesktop , &pWCHAR_Desktop ) ))
    {
        goto Cleanup;
    }

    WCHAR_StartupInfo.cb               = lpStartupInfo->cb ;
    WCHAR_StartupInfo.cbReserved2      = lpStartupInfo->cbReserved2;
    WCHAR_StartupInfo.dwFillAttribute  = lpStartupInfo->dwFillAttribute;
    WCHAR_StartupInfo.dwFlags          = lpStartupInfo->dwFlags;
    WCHAR_StartupInfo.dwX              = lpStartupInfo->dwX;
    WCHAR_StartupInfo.dwXCountChars    = lpStartupInfo->dwXCountChars;
    WCHAR_StartupInfo.dwXSize          = lpStartupInfo->dwXSize;
    WCHAR_StartupInfo.dwY              = lpStartupInfo->dwY;
    WCHAR_StartupInfo.dwYCountChars    = lpStartupInfo->dwYCountChars;
    WCHAR_StartupInfo.dwYSize          = lpStartupInfo->dwYSize;
    WCHAR_StartupInfo.hStdError        = lpStartupInfo->hStdError;
    WCHAR_StartupInfo.hStdInput        = lpStartupInfo->hStdInput;
    WCHAR_StartupInfo.hStdOutput       = lpStartupInfo->hStdOutput;
    WCHAR_StartupInfo.lpReserved2      = lpStartupInfo->lpReserved2;
    WCHAR_StartupInfo.wShowWindow      = lpStartupInfo->wShowWindow;
    WCHAR_StartupInfo.lpDesktop        = pWCHAR_Desktop;
    WCHAR_StartupInfo.lpReserved       = NULL;
    WCHAR_StartupInfo.lpTitle          = pWCHAR_Title;

    rc2 =     CreateRemoteSessionProcessW(
        SessionId,
        System,
        hToken,
        pWCHAR_AppName ,
        pWCHAR_CommandLine,
        lpProcessAttributes,
        lpThreadAttributes ,
        bInheritHandles,
        dwCreationFlags,
        lpEnvironment,
        pWCHAR_CurDir,
        &WCHAR_StartupInfo,
        lpProcessInformation
    );

Cleanup:

    if ( !rc )  // rc is set to FALSE if an attempted memory allocation has failed.
    {
        BaseSetLastNTError(STATUS_NO_MEMORY);
    }

    if (pWCHAR_AppName)
    {
        LocalFree( pWCHAR_AppName );
    }

    if (pWCHAR_CommandLine)
    {
        LocalFree( pWCHAR_CommandLine );
    }

    if (pWCHAR_CurDir)
    {
        LocalFree( pWCHAR_CurDir );
    }

    if (pWCHAR_Title)
    {
        LocalFree( pWCHAR_Title );
    }

    if (pWCHAR_Desktop)
    {
        LocalFree( pWCHAR_Desktop );
    }

    return rc2;
}


//+---------------------------------------------------------------------------
//
//  Function:   CreateProcessAsUserA
//
//  Synopsis:   ANSI wrapper for CreateProcessAsUserW
//
//  Arguments:  [hToken]               --
//              [lpApplicationName]    --
//              [lpCommandLine]        --
//              [lpProcessAttributes]  --
//              [lpThreadAttributes]   --
//              [bInheritHandles]      --
//              [dwCreationFlags]      --
//              [lpEnvironment]        --
//              [lpCurrentDirectory]   --
//              [lpStartupInfo]        --
//              [lpProcessInformation] --
//
//  Return Values
//          If the function succeeds, the return value is nonzero.
//          If the function fails, the return value is zero. To get extended error information, call GetLastError.
//
//  History:    4-25-95   RichardW   Created
//              1-14-98  AraBern     add changes for Hydra
//
//  Notes:
//
//----------------------------------------------------------------------------
BOOL
WINAPI
CreateProcessAsUserA(
    HANDLE  hToken,
    LPCSTR lpApplicationName,
    LPSTR lpCommandLine,
    LPSECURITY_ATTRIBUTES lpProcessAttributes,
    LPSECURITY_ATTRIBUTES lpThreadAttributes,
    BOOL bInheritHandles,
    DWORD dwCreationFlags,
    LPVOID lpEnvironment,
    LPCSTR lpCurrentDirectory,
    LPSTARTUPINFOA lpStartupInfo,
    LPPROCESS_INFORMATION lpProcessInformation
    )
{
    DWORD   CreateFlags;
    DWORD   clientSessionID=0;
    DWORD   currentSessionID=0;
    DWORD   resultLength;
    HANDLE  hTmpToken;
    DWORD   curProcId ;
    NTSTATUS Status ;

    CreateFlags = (dwCreationFlags & CREATE_SUSPENDED ? COMMON_CREATE_SUSPENDED : 0);

    //
    // get the session if (zero means console).
    //
    currentSessionID = NtCurrentPeb()->SessionId;

    if ( !GetTokenInformation ( hToken, TokenSessionId , &clientSessionID,sizeof( DWORD), &resultLength ) )
    {
    //
    // get the access token for the client of this call
    // use get token instead of process since the client might have only
    // impersonated the thread, not the process
    //
        DBG_DumpOutLastError;
        ASSERT( FALSE );
        currentSessionID = 0;

        //
        // We should probably return FALSE here, but at this time we don't want to alter the
        // non-Hydra code-execution-flow at all.
        //
    }

    KdPrint(("logon32.c: CreateProcessAsUserA(): clientSessionID = %d, currentSessionID = %d \n",
            clientSessionID, currentSessionID ));

    if ( ( clientSessionID != currentSessionID ))
    {
       //
       // If the client session ID is not the same as the current session ID, then, we are attempting
       // to create a process on a remote session from the current session.
       // This block of code is used to accomplish such process creation, it is Terminal-Server specific
       //

       BOOL        bHaveImpersonated;
       HANDLE      hCurrentThread;
       HANDLE      hPrevToken = NULL;
       DWORD       rc;
       TOKEN_TYPE  tokenType;

       //
       // We must send the request to the remote WinStation
       // of the requestor
       //
       // NOTE: The current WinStationCreateProcessW() does not use
       //       the supplied security descriptor, but creates the
       //       process under the account of the logged on user.
       //

       //
       // Stop impersonating before doing the WinStationCreateProcess.
       // The remote winstation exec thread will launch the app under
       // the users context. We must not be impersonating because this
       // call only lets SYSTEM request the remote execute.
       //
       hCurrentThread = GetCurrentThread();

       //
       // Init bHaveImpersonated to the FALSE state
       //
       bHaveImpersonated = FALSE;


        //
        // Since the caller of this function (runas-> SecLogon service ) has already
        // impersonated the new (target) user, we do the OpenThreadToken with
        // OpenAsSelf = TRUE
        //
        if ( OpenThreadToken( hCurrentThread, TOKEN_QUERY | TOKEN_IMPERSONATE, TRUE, &hPrevToken ) )
        {

            bHaveImpersonated = TRUE;

            if ( !RevertToSelf() )
            {
                return FALSE;
            }
        }

       //
       // else, we are not impersonating, as reflected by the init value of bHaveImpersonated
       //

        rc = CreateRemoteSessionProcessA(
                clientSessionID,
                FALSE,     // not creating a process for System
                hToken,
                lpApplicationName,
                lpCommandLine,
                lpProcessAttributes,
                lpThreadAttributes,
                bInheritHandles,
                dwCreationFlags,
                lpEnvironment,
                lpCurrentDirectory,
                lpStartupInfo,
                lpProcessInformation) ;

        //
        // Undo the effect of RevertToSelf() if we had impersoanted
        //
        if ( bHaveImpersonated )
        {
            Status = NtSetInformationThread(
                NtCurrentThread(),
                ThreadImpersonationToken,
                &hPrevToken,
                sizeof( hPrevToken ) );

            ASSERT( NT_SUCCESS(Status ) );


            NtClose( hPrevToken );
        }

        if ( rc )
        {
            return TRUE;
        }
        else
        {
            return FALSE;
        }

    }
    else
    //
    // this is the standard non-Hydra related call block
    //
    {
        HANDLE hRestrictedToken = NULL;
        BOOL b = FALSE;

        if (!CreateProcessInternalA(hToken,
                                    lpApplicationName,
                                    lpCommandLine,
                                    lpProcessAttributes,
                                    lpThreadAttributes,
                                    bInheritHandles,
                                    dwCreationFlags | CREATE_SUSPENDED,
                                    lpEnvironment,
                                    lpCurrentDirectory,
                                    lpStartupInfo,
                                    lpProcessInformation,
                                    &hRestrictedToken))
        {
            //
            // The internal routine might return a token even in the failure case
            // since it uses try-finally. Free the token if needed.
            //

            if (hRestrictedToken != NULL)
            {
                NtClose(hRestrictedToken); 
            } 
            return(FALSE);
        }

        CreateFlags |= (lpProcessAttributes ? 0 : COMMON_CREATE_PROCESSSD);
        CreateFlags |= (lpThreadAttributes ? 0 : COMMON_CREATE_THREADSD);


        if(lpProcessInformation->dwProcessId != 0) {

           HANDLE VdmWaitHandle = NULL;           

           //
           // Check if it is a shared wow being started
           //
           
           if((ULONG_PTR)lpProcessInformation->hProcess & 0x2) {        
              
               VdmWaitHandle = lpProcessInformation->hProcess;
               lpProcessInformation->hProcess = OpenProcess(PROCESS_ALL_ACCESS,
                                                            FALSE,
                                                            lpProcessInformation->dwProcessId);

               if (lpProcessInformation->hProcess == NULL)
               {
                   //
                   // Couldn't open it.  Try reverting since the new process gets security
                   // from the process token.
                   //

                   lpProcessInformation->hProcess = L32RevertOpenProcess(PROCESS_ALL_ACCESS,
                                                                         FALSE,
                                                                         lpProcessInformation->dwProcessId);

                   ASSERT(lpProcessInformation->hProcess);
               }
           }
                                                           
           //                                                                                   
           // If a restricted token was returned, set it on the process.                        
           // Else use the token provided by the caller.                                        
           //                                                                                   
                                                                                                
           if (hRestrictedToken == NULL)                                                        
           {                                                                                    
               b = (L32CommonCreate(CreateFlags, hToken, lpProcessInformation, lpProcessAttributes, lpThreadAttributes));                
           }                                                                                    
           else                                                                                 
           {                                                                                    
               b = (L32CommonCreate(CreateFlags, hRestrictedToken, lpProcessInformation, lpProcessAttributes, lpThreadAttributes));      
           }                                                                                    

           //
           // if L32CommonCreate didn't succeed, it closes lpProcessInformation->hProcess and
           // zeros out lpProcessInformation, so we shouldn't be closing it again if it didn't 
           // succeed.           
                                                                                                
           if(b && VdmWaitHandle) {                                                                  
              if(lpProcessInformation->hProcess) {                                                                           
                 NtClose(lpProcessInformation->hProcess);                                       
              }                                                                                 
              lpProcessInformation->hProcess = VdmWaitHandle;                                   
           }                                                                                    

        }
        else {
           b = TRUE;
        }
        
        if (hRestrictedToken) {
           NtClose(hRestrictedToken);
        }

        return b;
    }

}

