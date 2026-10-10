/* mailclient.h - a mail client for load tests (implemented in mailclient.cpp)
 *
 * MailClient crafts a complete mail note (Form, From, SendTo, Recipients, Subject, PostedDate, rich text Body) and
 * writes it straight into the mail.box of a Domino server, optionally with a sent copy in the sender's mail file. The
 * router picks it up from there, so a message exercises the whole mail path. The mail.box is opened on the server (a
 * remote open): a server with several mail boxes (mail1.box ... mailN.box) hands out one of them for every open.
 *
 * Holds no handle between two messages: Send() opens each database for its note, closes the note and then the database
 * (CloseDb). Every message therefore opens mail.box anew, as a client does. The databases are opened with the identity
 * the process has at that moment. Further mail operations (reading, deleting) are added here later.
 *
 *   MailClient Mail;
 *   std::string Err;
 *
 *   Mail.SetBodySize (ValueRange (4096, 8192));       // bytes, a value in the range for every mail
 *   Mail.SetServer ("server", Err);
 *   Mail.AddSendTo ("CN=Recipient/O=Org");
 *   Mail.AddSendTo ("CN=Another/O=Org");
 *   Mail.Send ("CN=Sender/O=Org", "Subject", Err);
 */

#ifndef DOMLEM_MAILCLIENT_H
#define DOMLEM_MAILCLIENT_H

#include <string>
#include <vector>

#include <global.h>
#include <nsfdb.h>

#include "textgen.h"
#include "lib.h"

class MailClient
{
public:

    MailClient();
    ~MailClient();

    /* Settings: take effect with the next Send() */
    void SetBodySize (const ValueRange &Bytes);    /* size of the generated body text, fixed or a range; 0: no body (default 1024) */
    void SetBodyStyle (TextStyle Style);       /* kind of the generated body text (default: lorem ipsum) */
    /* Attachments per mail (each with an icon in the body) and size of one, each fixed or a range, random bytes or text.
     * Every mail gets its own values out of the ranges. */
    void SetAttachments (const ValueRange &Count, const ValueRange &Bytes, BOOL bBinary);
    void SetAutoSubmitted (BOOL bAutoSubmitted);   /* add the Auto-submitted item (RFC 3834), default TRUE */

    /* The server whose mail.box gets the messages (pszServer empty: local). Starts counting messages at 1 again, so the
     * content of message N is the same in every job. Opens nothing. On an error Err has the failing call. */
    STATUS SetServer (const char *pszServer, std::string &Err);

    /* Also save a copy of every message in this database, the sender's mail file (like "save sent copy" in a Notes
     * client). pszPath is a full path (server!!file); NULL or empty: no sent copies. The copy is written before the
     * message goes into the mail.box. Opens nothing. */
    void SetSentCopyPath (const char *pszPath);

    BOOL HasSentCopy() const { return !m_SentCopyPath.empty(); }

    /* Goes into the attachment names, to tell the workers apart: "attachment_<tag>_<message>_<n>.bin". Only letters,
     * digits, dot, dash and underscore; anything else is left out. */
    void SetAttachmentTag (const char *pszTag);

    /* The recipients of the next message: Notes names or internet addresses, as many as needed. The Recipients item
     * is built from all three lists. They stay until ClearRecipients(), so one set can be sent to repeatedly. */
    void AddSendTo (const char *pszName);
    void AddCopyTo (const char *pszName);
    void AddBlindCopyTo (const char *pszName);
    void ClearRecipients();

    /* Crafts one message to the recipients added before: the sent copy (when there is a sent copy path), then the
     * message in the mail.box. pszFrom is a Notes name or internet address. Err has the failing call when it fails. */
    STATUS Send (const char *pszFrom, const char *pszSubject, std::string &Err);

    DWORD GetSent() const { return m_dwSent; }

    /* What the last message that was sent contained (the values picked out of the ranges) */
    DWORD GetLastBodyBytes() const    { return m_dwLastBodyBytes; }
    DWORD GetLastAttachCount() const  { return m_dwLastAttachCount; }
    DWORD GetLastAttachBytes() const  { return m_dwLastAttachBytes; }       /* all attachments together */

private:

    MailClient (const MailClient &);                /* not copyable */
    MailClient &operator= (const MailClient &);

    STATUS WriteMessage (const char *pszPath, const char *pszWhat, const char *pszFrom, const char *pszSubject, std::string &Err);
    STATUS BuildMessage (NOTEHANDLE hNote, const char *pszFrom, const char *pszSubject, std::string &Err);
    STATUS AddBody (NOTEHANDLE hNote, std::string &Err);
    STATUS AddAttachment (NOTEHANDLE hNote, DWORD dwIndex, std::string &Err);

    std::vector<std::string> m_SendTo;
    std::vector<std::string> m_CopyTo;
    std::vector<std::string> m_BlindCopyTo;

    std::string m_MailBoxPath;  /* server!!mail.box, empty until SetServer() */
    std::string m_SentCopyPath; /* the sender's mail file for the sent copies, empty: none */
    std::string m_AttachTag;
    ValueRange m_BodyBytes;
    TextGenerator m_BodyText;
    ValueRange m_AttachCount;
    ValueRange m_AttachBytes;
    DWORD    m_dwLastBodyBytes;
    DWORD    m_dwLastAttachCount;
    DWORD    m_dwLastAttachBytes;
    BOOL     m_bAttachBinary;
    std::vector<std::string> m_AttachNames;     /* the attachments of the message that is being made */
    BOOL     m_bAutoSubmitted;
    DWORD    m_dwSent;
};

#endif /* DOMLEM_MAILCLIENT_H */
