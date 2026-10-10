/* mailclient.cpp - a mail client for load tests. Interface: mailclient.h */

#include <stdio.h>
#include <string.h>

#include <global.h>
#include <miscerr.h>
#include <nsfdb.h>
#include <nsfnote.h>
#include <osfile.h>
#include <ostime.h>
#include <fontid.h>
#include <easycd.h>
#include <stdnames.h>

#include "lib.h"
#include "mailclient.h"

#define MAILBOX_NAME        "mail.box"
#define MAIL_BODY_CHUNK     16000         /* text handed to the rich text API in one piece */


MailClient::MailClient() : m_BodyBytes (1024), m_AttachCount (0), m_AttachBytes (0), m_dwLastBodyBytes (0), m_dwLastAttachCount (0), m_dwLastAttachBytes (0), m_bAttachBinary (TRUE), m_bAutoSubmitted (TRUE), m_dwSent (0)
{
}


MailClient::~MailClient()
{
    /* No handle is kept between messages: nothing to close */
}


void MailClient::SetBodySize (const ValueRange &Bytes)
{
    m_BodyBytes = Bytes;
}


static void AddName (std::vector<std::string> &List, const char *pszName)
{
    if (!IsNullStr (pszName))
        List.push_back (pszName);
}


void MailClient::AddSendTo (const char *pszName)
{
    AddName (m_SendTo, pszName);
}


void MailClient::AddCopyTo (const char *pszName)
{
    AddName (m_CopyTo, pszName);
}


void MailClient::AddBlindCopyTo (const char *pszName)
{
    AddName (m_BlindCopyTo, pszName);
}


void MailClient::ClearRecipients()
{
    m_SendTo.clear();
    m_CopyTo.clear();
    m_BlindCopyTo.clear();
}


void MailClient::SetAttachments (const ValueRange &Count, const ValueRange &Bytes, BOOL bBinary)
{
    m_AttachCount   = Count;
    m_AttachBytes   = Bytes;
    m_bAttachBinary = bBinary;
}


void MailClient::SetBodyStyle (TextStyle Style)
{
    m_BodyText.SetStyle (Style);
}


void MailClient::SetAutoSubmitted (BOOL bAutoSubmitted)
{
    m_bAutoSubmitted = bAutoSubmitted;
}


void MailClient::SetAttachmentTag (const char *pszTag)
{
    m_AttachTag.clear();

    for (const char *p = pszTag ? pszTag : ""; *p && (m_AttachTag.size() < 32); p++)
    {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || '.' == *p || '-' == *p || '_' == *p)
            m_AttachTag += *p;
    }
}


STATUS MailClient::SetServer (const char *pszServer, std::string &Err)
{
    STATUS error = NOERROR;
    char   szPath[MAXPATH+1] = {0};

    m_MailBoxPath.clear();

    error = OSPathNetConstruct (NULL, NullIfEmpty ((char *) pszServer), MAILBOX_NAME, szPath);

    if (error)
    {
        Err = std::string ("Cannot construct path for ") + MAILBOX_NAME + ": " + ErrorText (error);
        goto Done;
    }

    m_MailBoxPath = szPath;

    /* Message numbers (and with them the content) start again with every job */
    m_dwSent = 0;

Done:

    return error;
}


void MailClient::SetSentCopyPath (const char *pszPath)
{
    m_SentCopyPath = pszPath ? pszPath : "";
}


/* One message into one database: open it, create the note, write the message, close the note, close the database.
 * pszWhat names the database in messages ("the mail.box", "the sent copy"). */

STATUS MailClient::WriteMessage (const char *pszPath, const char *pszWhat, const char *pszFrom, const char *pszSubject, std::string &Err)
{
    STATUS     error = NOERROR;
    DBHANDLE   hDb   = NULLHANDLE;
    NOTEHANDLE hNote = NULLHANDLE;

    error = NSFDbOpen (pszPath, &hDb);

    if (error)
    {
        Err = std::string ("Cannot open ") + pszPath + " for " + pszWhat + ": " + ErrorText (error);
        hDb = NULLHANDLE;
        goto Done;
    }

    error = NSFNoteCreate (hDb, &hNote);

    if (error)
    {
        Err   = std::string ("NSFNoteCreate (") + pszWhat + ") failed: " + ErrorText (error);
        hNote = NULLHANDLE;
        goto Done;
    }

    error = BuildMessage (hNote, pszFrom, pszSubject, Err);

    if (error)
        goto Done;

    error = NSFNoteUpdate (hNote, 0);

    if (error)
        Err = std::string ("Cannot write ") + pszWhat + " into " + pszPath + ": " + ErrorText (error);

Done:

    /* The note before its database */
    if (hNote)
        NSFNoteClose (hNote);

    CloseDb (&hDb);

    return error;
}


/* Text item with the length the API wants */

static STATUS SetText (NOTEHANDLE hNote, const char *pszItem, const char *pszText)
{
    return NSFItemSetText (hNote, pszItem, pszText, (WORD) strlen (pszText));
}


/* A text list item with all the names; with bAppend the names are added to an existing item. Duplicates are dropped. */

static STATUS SetTextList (NOTEHANDLE hNote, const char *pszItem, const std::vector<std::string> &Names, BOOL bAppend)
{
    STATUS error  = NOERROR;
    size_t nIndex = 0;
    BOOL   bExists = bAppend;

    for (nIndex = 0; (NOERROR == error) && (nIndex < Names.size()); nIndex++)
    {
        if (bExists)
            error = NSFItemAppendTextList (hNote, pszItem, Names[nIndex].c_str(), (WORD) Names[nIndex].length(), FALSE);
        else
            error = NSFItemCreateTextList (hNote, pszItem, Names[nIndex].c_str(), (WORD) Names[nIndex].length());

        bExists = TRUE;
    }

    return error;
}


/* Error text for an item that could not be written */

static std::string ItemError (const char *pszItem, STATUS error)
{
    return std::string ("Cannot write item [") + pszItem + "]: " + ErrorText (error);
}


/* The body: generated lines of text up to the configured size, as rich text */

STATUS MailClient::AddBody (NOTEHANDLE hNote, std::string &Err)
{
    STATUS        error     = NOERROR;
    DHANDLE       hCompound = NULLHANDLE;
    size_t        nOffset   = 0;
    DWORD         dwStyleID = 0;
    COMPOUNDSTYLE Style     = {0};
    std::string   Text;

    /* Nothing to write: no text, and no attachment icons */
    if ((0 == m_dwLastBodyBytes) && m_AttachNames.empty())
        goto Done;

    error = CompoundTextCreate (hNote, (char *) MAIL_BODY_ITEM, &hCompound);

    if (error)
    {
        Err = "CompoundTextCreate failed: " + ErrorText (error);
        hCompound = NULLHANDLE;
        goto Done;
    }

    /* One paragraph style for the whole body */
    CompoundTextInitStyle (&Style);
    Style.LineSpacing = 1;

    error = CompoundTextDefineStyle (hCompound, (char *) "", &Style, &dwStyleID);

    if (error)
    {
        Err = "CompoundTextDefineStyle failed: " + ErrorText (error);
        goto Done;
    }

    /* The text for this message: the seed is the message number, so every message differs */
    Text = m_BodyText.Generate ((size_t) m_dwLastBodyBytes, (unsigned long) (m_dwSent + 1));

    while (nOffset < Text.size())
    {
        size_t nPart = Text.size() - nOffset;

        if (nPart > MAIL_BODY_CHUNK)
            nPart = MAIL_BODY_CHUNK;

        error = CompoundTextAddTextExt (hCompound, dwStyleID, DEFAULT_FONT_ID, (char *) Text.c_str() + nOffset, (DWORD) nPart, (char *) "\n", COMP_PRESERVE_LINES, NULL);

        if (error)
        {
            Err = "CompoundTextAddTextExt failed: " + ErrorText (error);
            goto Done;
        }

        nOffset += nPart;
    }

    /* The attachment icons, after the text */
    for (size_t nName = 0; nName < m_AttachNames.size(); nName++)
    {
        error = AddAttachmentHotspot (hCompound, m_AttachNames[nName].c_str(), (std::string (" ") + m_AttachNames[nName]).c_str(), Err);

        if (error)
            goto Done;
    }

    /* Writes the rich text item into the note. After a successful close the handle is gone. */
    error = CompoundTextClose (hCompound, NULL, NULL, NULL, 0);

    if (error)
    {
        Err = "CompoundTextClose failed: " + ErrorText (error);
        goto Done;
    }

    hCompound = NULLHANDLE;

Done:

    if (hCompound)
        CompoundTextDiscard (hCompound);

    return error;
}


/* One attachment: random data in a temporary file, attached with the official API under its display name, and the
 * file removed again. The file is in the data directory; its name has the process id and the message number. */

STATUS MailClient::AddAttachment (NOTEHANDLE hNote, DWORD dwIndex, std::string &Err)
{
    STATUS      error   = NOERROR;
    FILE       *pFile   = NULL;
    BOOL        bFile   = FALSE;
    size_t      nWritten = 0;
    unsigned long ulSeed = (unsigned long) (m_dwSent + 1) * 1000UL + dwIndex;
    DWORD       dwBytes  = PickInRange (m_AttachBytes, ulSeed);
    std::string Data;
    char        szDir[MAXPATH+1]      = {0};
    char        szPath[MAXPATH+1]     = {0};
    char        szName[128]           = {0};
    int         nLen    = 0;

    OSGetDataDirectory (szDir);
    OSPathAddTrailingPathSeparator (szDir, MAXPATH);

    nLen = snprintf (szPath, sizeof (szPath), "%sdomlem_att_%lu_%lu_%lu.tmp", szDir, DOMLEM_GETPID(), (unsigned long) (m_dwSent + 1), (unsigned long) dwIndex);

    if ((nLen < 0) || ((size_t) nLen >= sizeof (szPath)))
    {
        Err   = "Path of the attachment file too long";
        error = ERR_MISC_INVALID_ARGS;
        goto Done;
    }

    /* The name the receiver sees, with the tag of the worker when there is one */
    snprintf (szName, sizeof (szName), "attachment_%s%s%lu_%lu.%s", m_AttachTag.c_str(), m_AttachTag.empty() ? "" : "_",
              (unsigned long) (m_dwSent + 1), (unsigned long) (dwIndex + 1), m_bAttachBinary ? "bin" : "txt");

    Data = m_bAttachBinary ? RandomBytes ((size_t) dwBytes, ulSeed) : m_BodyText.Generate ((size_t) dwBytes, ulSeed);

    pFile = fopen (szPath, "wb");

    if (NULL == pFile)
    {
        Err   = std::string ("Cannot create the attachment file ") + szPath;
        error = ERR_MISC_INVALID_ARGS;
        goto Done;
    }

    bFile    = TRUE;
    nWritten = fwrite (Data.data(), 1, Data.size(), pFile);

    if (nWritten != Data.size())
    {
        Err   = std::string ("Cannot write the attachment file ") + szPath;
        error = ERR_MISC_INVALID_ARGS;
        goto Done;
    }

    fclose (pFile);
    pFile = NULL;

    error = NSFNoteAttachFile (hNote, ITEM_NAME_ATTACHMENT, (WORD) strlen (ITEM_NAME_ATTACHMENT), szPath, szName, COMPRESS_LZ1);

    if (error)
        Err = std::string ("NSFNoteAttachFile (") + szName + ") failed: " + ErrorText (error);
    else
    {
        m_AttachNames.push_back (szName);
        m_dwLastAttachBytes += dwBytes;
    }

Done:

    if (pFile)
        fclose (pFile);

    if (bFile)
        remove (szPath);

    return error;
}


/* Items, attachments and body of a message, into a new note: the mail.box note or the sent copy. The values come from
 * the message number, so both get the same content. */

STATUS MailClient::BuildMessage (NOTEHANDLE hNote, const char *pszFrom, const char *pszSubject, std::string &Err)
{
    STATUS   error = NOERROR;
    TIMEDATE tNow  = {0};

    /* The items of a message the router can pick up. Recipients is the one it needs; the others make it a normal memo.
     * Every item is checked: the first error ends the message. */
    error = SetText (hNote, MAIL_FORM_ITEM, MAIL_MEMO_FORM);

    if (error)
    {
        Err = ItemError (MAIL_FORM_ITEM, error);
        goto Done;
    }

    error = SetText (hNote, MAIL_FROM_ITEM, pszFrom);

    if (error)
    {
        Err = ItemError (MAIL_FROM_ITEM, error);
        goto Done;
    }

    error = SetTextList (hNote, MAIL_SENDTO_ITEM, m_SendTo, FALSE);

    if (error)
    {
        Err = ItemError (MAIL_SENDTO_ITEM, error);
        goto Done;
    }

    error = SetTextList (hNote, MAIL_COPYTO_ITEM, m_CopyTo, FALSE);

    if (error)
    {
        Err = ItemError (MAIL_COPYTO_ITEM, error);
        goto Done;
    }

    error = SetTextList (hNote, MAIL_BLINDCOPYTO_ITEM, m_BlindCopyTo, FALSE);

    if (error)
    {
        Err = ItemError (MAIL_BLINDCOPYTO_ITEM, error);
        goto Done;
    }

    /* Recipients: everybody, once. This is the list the router delivers to. */
    error = SetTextList (hNote, MAIL_RECIPIENTS_ITEM, m_SendTo, FALSE);

    if (!error)
        error = SetTextList (hNote, MAIL_RECIPIENTS_ITEM, m_CopyTo, !m_SendTo.empty());

    if (!error)
        error = SetTextList (hNote, MAIL_RECIPIENTS_ITEM, m_BlindCopyTo, !m_SendTo.empty() || !m_CopyTo.empty());

    if (error)
    {
        Err = ItemError (MAIL_RECIPIENTS_ITEM, error);
        goto Done;
    }

    error = SetText (hNote, MAIL_SUBJECT_ITEM, IsNullStr (pszSubject) ? "domlem load test" : pszSubject);

    if (error)
    {
        Err = ItemError (MAIL_SUBJECT_ITEM, error);
        goto Done;
    }

    /* Generated messages carry this item (RFC 3834): no auto replies, no out of office answers */
    if (m_bAutoSubmitted)
    {
        error = SetText (hNote, MAIL_ITEM_AUTOSUBMITTED, MAIL_AUTOGENERATED);

        if (error)
        {
            Err = ItemError (MAIL_ITEM_AUTOSUBMITTED, error);
            goto Done;
        }
    }

    OSCurrentTIMEDATE (&tNow);
    error = NSFItemSetTime (hNote, MAIL_POSTEDDATE_ITEM, &tNow);

    if (error)
    {
        Err = ItemError (MAIL_POSTEDDATE_ITEM, error);
        goto Done;
    }

    /* First the attachments: their names are needed for the icons in the body */
    m_AttachNames.clear();

    /* The values of this message out of the ranges: the same message number gives the same values */
    m_dwLastBodyBytes    = PickInRange (m_BodyBytes, (unsigned long) (m_dwSent + 1) * 7UL + 1);
    m_dwLastAttachCount  = PickInRange (m_AttachCount, (unsigned long) (m_dwSent + 1) * 7UL + 2);
    m_dwLastAttachBytes  = 0;

    for (DWORD dwIndex = 0; dwIndex < m_dwLastAttachCount; dwIndex++)
    {
        error = AddAttachment (hNote, dwIndex, Err);

        if (error)
            goto Done;
    }

    error = AddBody (hNote, Err);

Done:

    return error;
}


STATUS MailClient::Send (const char *pszFrom, const char *pszSubject, std::string &Err)
{
    STATUS error = NOERROR;

    if (m_MailBoxPath.empty())
    {
        Err   = "No mail.box (SetServer)";
        error = ERR_MISC_INVALID_ARGS;
        goto Done;
    }

    if (IsNullStr (pszFrom) || (m_SendTo.empty() && m_CopyTo.empty() && m_BlindCopyTo.empty()))
    {
        Err   = "No sender or recipient";
        error = ERR_MISC_INVALID_ARGS;
        goto Done;
    }

    /* The sent copy first: a note of its own in the sender's mail file, built from the same values (not a copy of the
     * mail.box note: once that is in the mail.box the router may take it away at any moment). Each database is opened
     * for its note and closed again before the next one is opened. */
    if (!m_SentCopyPath.empty())
    {
        error = WriteMessage (m_SentCopyPath.c_str(), "the sent copy", pszFrom, pszSubject, Err);

        if (error)
            goto Done;
    }

    error = WriteMessage (m_MailBoxPath.c_str(), "the message", pszFrom, pszSubject, Err);

    if (error)
        goto Done;

    m_dwSent++;

Done:

    return error;
}
