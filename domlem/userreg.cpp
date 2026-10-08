/* userreg.cpp - register one Domino user, with mail file
 *
 * One helper, RegEnsureUser(): look the user up in the Domino Directory and, when it does not exist, register it
 * with REGNewPerson() - ID, directory entry and mail file in one step. Used by domlem in "-switch" mode.
 * Everything else a registration tool can do is deliberately left out. Interface and settings: userreg.h.
 */

#include <stdio.h>
#include <string.h>

#include <global.h>
#include <addin.h>
#include <kfm.h>
#include <lookup.h>
#include <misc.h>
#include <miscerr.h>
#include <names.h>
#include <osmem.h>
#include <ostime.h>
#include <reg.h>
#include <regerr.h>

#include "lib.h"
#include "userreg.h"


/* Opens the certifier (Domino CA or certifier ID file) and sets how long the new certificates are valid */
static STATUS OpenCertifier (const DomRegSetup *pSetup, const char *pszServer, HCERTIFIER *phCertCtx, char *pszRetCertifierName)
{
    STATUS       error           = NOERROR;
    BOOL         bHierarchical   = FALSE;
    WORD         wFileVersion    = 0;
    TIMEDATE     tExpiration     = {0};
    KFM_PASSWORD HashedPassword  = {0};
    char         szLogFile[MAXPATH+1]       = {0};
    char         szCAName[MAXUSERNAME+1]    = {0};
    char         szCertFile[MAXPATH+1]      = {0};
    char         szCertPassword[256]          = {0};
    char         szServer[MAXUSERNAME+1]    = {0};

    CopyStr (szCAName, sizeof (szCAName), pSetup->certifier.caName.c_str());
    CopyStr (szCertFile, sizeof (szCertFile), pSetup->certifier.file.c_str());
    CopyStr (szCertPassword, sizeof (szCertPassword), pSetup->certifier.password.c_str());
    CopyStr (szServer, sizeof (szServer), pszServer);

    /* A certifier ID file with its password is used directly, else the Domino CA */
    if (!IsNullStr (szCertFile) && !IsNullStr (szCertPassword))
    {
        SECKFMCreatePassword (szCertPassword, &HashedPassword);

        error = SECKFMGetCertifierCtx (szCertFile, &HashedPassword, szLogFile, &tExpiration, pszRetCertifierName, phCertCtx, &bHierarchical, &wFileVersion);

        if (error)
        {
            AddInLogMessageText ("%s: Cannot open certifier ID [%s]", error, g_szLogPrefix, szCertFile);
            goto Done;
        }
    }
    else if (!IsNullStr (szCAName))
    {
        if (!IsNullStr (szCertFile))
            AddInLogMessageText ("%s: Certifier ID [%s] given without password: using the Domino CA [%s]", NOERROR, g_szLogPrefix, szCertFile, szCAName);

        error = SECKFMGetCertifierCtxExt (NULL, NULL, szServer, szCAName, szLogFile, &tExpiration, pszRetCertifierName, phCertCtx, &bHierarchical, &wFileVersion);

        if (error)
        {
            AddInLogMessageText ("%s: Cannot open Domino CA [%s]", error, g_szLogPrefix, szCAName);
            goto Done;
        }
    }
    else
    {
        AddInLogMessageText ("%s: No certifier: give -certid with DOMLEM_CERTPW, or -ca for the Domino CA", NOERROR, g_szLogPrefix);
        error = ERR_MISC_INVALID_ARGS;
        goto Done;
    }

    OSCurrentTIMEDATE (&tExpiration);
    TimeDateAdjust (&tExpiration, 0, 0, 0, 0, pSetup->expirationMonths ? pSetup->expirationMonths : g_wDefaultMonths, 0);

    error = SECKFMSetCertifierExpiration (*phCertCtx, &tExpiration);

    if (error)
        AddInLogMessageText ("%s: Cannot set the certificate expiration", error, g_szLogPrefix);

Done:

    /* The password hash does not stay on the stack */
    memset (&HashedPassword, 0, sizeof (HashedPassword));
    memset (szCertPassword, 0, sizeof (szCertPassword));

    return error;
}


STATUS RegEnsureUser (const DomRegSetup *pSetup,
                      const char         *pszFirstName,
                      const char         *pszLastName,
                      const char         *pszPassword,
                      const char         *pszShortName,
                      const char         *pszInternetAddress,
                      char               *pszRetUserName,
                      WORD                wMaxUserName,
                      BOOL               *pbRetCreated)
{
    STATUS     error    = NOERROR;
    HCERTIFIER hCertCtx = NULLHANDLE;

    REG_PERSON_INFO   PersonInfo          = {0};
    REG_MAIL_INFO_EXT RegMailInfoExt      = {0};
    REG_MISC_INFO     RegMiscInfo         = {0};
    REG_ID_INFO       RegIDInfo           = {0};

    char szServer[MAXUSERNAME+1]          = {0};
    char szFirstName[MAXUSERNAME+1]       = {0};
    char szLastName[MAXUSERNAME+1]        = {0};
    char szPassword[256]                  = {0};
    char szShortName[MAXUSERNAME+1]       = {0};
    char szInternetAddress[MAXUSERNAME+1] = {0};
    char szMailServer[MAXUSERNAME+1]      = {0};
    char szMailFile[MAXPATH+1]            = {0};
    char szMailTemplate[MAXPATH+1]        = {0};
    char szPolicy[MAXUSERNAME+1]          = {0};
    char szCertifierName[MAXUSERNAME+1]   = {0};
    char szFullName[MAXUSERNAME+1]        = {0};
    char szFoundName[MAXUSERNAME+1]       = {0};
    char szErrorPathName[MAXPATH+1]       = {0};

    const char *pszMailDir = NULL;
    int         nLen       = 0;

    if (pbRetCreated)
        *pbRetCreated = FALSE;

    if ((NULL == pSetup) || (NULL == pszRetUserName) || (0 == wMaxUserName))
    {
        error = ERR_MISC_INVALID_ARGS;
        goto Done;
    }

    *pszRetUserName = '\0';

    if ((IsNullStr (pszFirstName) && IsNullStr (pszLastName)) || IsNullStr (pszShortName) || IsNullStr (pszPassword))
    {
        error = ERR_MISC_INVALID_ARGS;
        goto Done;
    }

    /* The short name becomes part of the mail file path: only plain names */
    if (!IsSafeName (pszShortName, 64))
    {
        AddInLogMessageText ("%s: The short name [%s] cannot be used as a mail file name", NOERROR, g_szLogPrefix, pszShortName);
        error = ERR_MISC_INVALID_ARGS;
        goto Done;
    }

    CopyStr (szServer, sizeof (szServer), pSetup->server.c_str());
    CopyStr (szFirstName, sizeof (szFirstName), pszFirstName);
    CopyStr (szLastName, sizeof (szLastName), pszLastName);
    CopyStr (szPassword, sizeof (szPassword), pszPassword);
    CopyStr (szShortName, sizeof (szShortName), pszShortName);
    CopyStr (szInternetAddress, sizeof (szInternetAddress), pszInternetAddress);
    CopyStr (szMailTemplate, sizeof (szMailTemplate), pSetup->mail.templateFile.c_str());
    CopyStr (szPolicy, sizeof (szPolicy), pSetup->policy.c_str());

    /* No server given: the server this task runs on */
    if (IsNullStr (szServer))
    {
        error = SECKFMGetUserName (szServer);

        if (error)
            goto Done;
    }

    /* Does the user exist already? Short name and mail address first: they need no certifier */
    if (LookupUser (szServer, szShortName, szFoundName, MAXUSERNAME) || LookupUser (szServer, szInternetAddress, szFoundName, MAXUSERNAME))
    {
        AddInLogMessageText ("%s: User [%s] exists already", NOERROR, g_szLogPrefix, szShortName);
        goto Exists;
    }

    error = OpenCertifier (pSetup, szServer, &hCertCtx, szCertifierName);

    if (error)
        goto Done;

    /* The full name: CN=<first> <last>/<certifier> */
    if (!IsNullStr (szFirstName) && !IsNullStr (szLastName))
        nLen = snprintf (szFullName, sizeof (szFullName), "CN=%s %s/%s", szFirstName, szLastName, szCertifierName);
    else
        nLen = snprintf (szFullName, sizeof (szFullName), "CN=%s/%s", IsNullStr (szFirstName) ? szLastName : szFirstName, szCertifierName);

    if ((nLen < 0) || ((size_t) nLen >= sizeof (szFullName)))
    {
        error = ERR_MISC_INVALID_ARGS;
        goto Done;
    }

    if (LookupUser (szServer, szFullName, szFoundName, MAXUSERNAME))
    {
        AddInLogMessageText ("%s: User [%s] exists already", NOERROR, g_szLogPrefix, szFullName);
        goto Exists;
    }

    /* The mail file is created together with the user, on the same server */
    pszMailDir = pSetup->mail.directory.empty() ? (const char *) g_szDefaultMailDir : pSetup->mail.directory.c_str();
    nLen = snprintf (szMailFile, sizeof (szMailFile), "%s/%s.nsf", pszMailDir, szShortName);

    if ((nLen < 0) || ((size_t) nLen >= sizeof (szMailFile)))
    {
        error = ERR_MISC_INVALID_ARGS;
        goto Done;
    }

    CopyStr (szMailServer, sizeof (szMailServer), szServer);

    PersonInfo.Size     = sizeof (PersonInfo);
    RegMailInfoExt.Size = sizeof (RegMailInfoExt);
    RegMiscInfo.Size    = sizeof (RegMiscInfo);
    RegIDInfo.Size      = sizeof (RegIDInfo);

    PersonInfo.MailInfo = &RegMailInfoExt;
    PersonInfo.MiscInfo = &RegMiscInfo;
    PersonInfo.IDInfo   = &RegIDInfo;
    RegIDInfo.Type      = KFM_IDFILE_TYPE_STD;

    PersonInfo.FirstName       = NullIfEmpty (szFirstName);
    PersonInfo.LastName        = NullIfEmpty (szLastName);
    PersonInfo.Password        = szPassword;
    PersonInfo.ShortName       = szShortName;
    PersonInfo.InternetAddress = NullIfEmpty (szInternetAddress);
    PersonInfo.ExplicitPolicy  = NullIfEmpty (szPolicy);

    RegMailInfoExt.MailSystem       = g_wMailSystem;
    RegMailInfoExt.MailOwnerAccess  = REG_MAIL_OWNER_ACL_EDITOR;
    RegMailInfoExt.pMailServerName  = szMailServer;
    RegMailInfoExt.pMailFileName    = szMailFile;
    RegMailInfoExt.pMailTemplateName = NullIfEmpty (szMailTemplate);

    /* ID, directory entry, internet password and the mail file, all while the user is created */
    PersonInfo.Flags    = fREGCreateIDFileNow | fREGCreateAddrBookEntry | fREGSetInternetPassword | fREGCreateMailFileNow;
    PersonInfo.FlagsExt = fREGExtEnforceUniqueShortName | fREGExtCreateINetKeyPair;

    error = REGNewPerson (hCertCtx, NullIfEmpty (szServer), &PersonInfo, NULL, szErrorPathName, NULL);

    if (ERR_REG_ADDRBOOK_ENTRY_EXISTS == ERR (error))
    {
        /* Created by somebody else in the meantime: that is fine, but its ID may not be in the vault yet either */
        AddInLogMessageText ("%s: User [%s] exists already", NOERROR, g_szLogPrefix, szFullName);
        error = NOERROR;

        if (pbRetCreated)
            *pbRetCreated = TRUE;
    }
    else if (error)
    {
        AddInLogMessageText ("%s: Cannot register user [%s]", error, g_szLogPrefix, szFullName);
        goto Done;
    }
    else
    {
        AddInLogMessageText ("%s: User registered: [%s], mail file [%s] on [%s]", NOERROR, g_szLogPrefix, szFullName, szMailFile, szMailServer);

        if (pbRetCreated)
            *pbRetCreated = TRUE;
    }

    CopyStr (pszRetUserName, wMaxUserName, szFullName);
    goto Done;

Exists:

    /* The name of the document that was found, else the name we would have registered */
    if (!IsNullStr (szFoundName))
        CopyStr (pszRetUserName, wMaxUserName, szFoundName);
    else
        CopyStr (pszRetUserName, wMaxUserName, szFullName);

Done:

    if (hCertCtx)
    {
        SECKFMFreeCertifierCtx (hCertCtx);
        hCertCtx = NULLHANDLE;
    }

    /* The password does not stay on the stack */
    memset (szPassword, 0, sizeof (szPassword));

    return error;
}
