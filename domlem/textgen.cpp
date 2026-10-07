/* textgen.cpp - generates body text for the load mails. Interface: textgen.h */

#include <string.h>
#include <random>

#include "textgen.h"


static const char *g_LoremWords[] =
{
    "lorem", "ipsum", "dolor", "sit", "amet", "consectetur", "adipiscing", "elit", "sed", "do",
    "eiusmod", "tempor", "incididunt", "ut", "labore", "et", "dolore", "magna", "aliqua", "enim",
    "ad", "minim", "veniam", "quis", "nostrud", "exercitation", "ullamco", "laboris", "nisi", "aliquip",
    "ex", "ea", "commodo", "consequat", "duis", "aute", "irure", "in", "reprehenderit", "voluptate",
    "velit", "esse", "cillum", "eu", "fugiat", "nulla", "pariatur", "excepteur", "sint", "occaecat",
    "cupidatat", "non", "proident", "sunt", "culpa", "qui", "officia", "deserunt", "mollit", "anim",
    "id", "est", "laborum", "vestibulum", "tincidunt", "pellentesque", "habitant", "morbi", "tristique", "senectus"
};

static const char *g_FunnySentences[] =
{
    "The router has delivered this message, which is more than the printer ever did.",
    "Please do not reply to all. The server is already sweating.",
    "This mail was sent by a lemming. It does not know where it is going, but it is very committed.",
    "Have you tried turning the mail file off and on again?",
    "Our backup strategy is to hope. Our restore strategy is to hope harder.",
    "The meeting could have been an email. This email could have been a meeting. Nobody is happy.",
    "According to the logs, everything is fine. The logs have not been asked about the users.",
    "A database walks into a server and says: I would like to open, please.",
    "This message will self-destruct as soon as somebody reads it, which we both know will not happen.",
    "The cloud is just somebody else's Domino server with better marketing.",
    "Please find attached nothing. The attachment is the friends we made along the way.",
    "Every mail file is unique. Just like the way every mail file is full.",
    "The coffee machine is down. All other incidents have been downgraded.",
    "If you are reading this, the router did its job and the lemming did its job. Please clap.",
    "Our new policy is to have no policy. A policy document will follow.",
    "The quarterly report is late because the spreadsheet found out it was a database.",
    "Nobody knows what the server task does. Everybody is afraid to stop it.",
    "The password is on a sticky note, which is in the cloud, which is a drawer.",
    "Urgent: please ignore the previous urgent message. This one is also urgent.",
    "The load test is going well. By well we mean the graphs are going up.",
    "There are only two hard problems in computer science: cache invalidation, naming things and mail quotas.",
    "A good mail subject is like a good joke: nobody reads it before the punchline.",
    "The server does not crash. It takes unscheduled rests.",
    "We value your feedback, as long as it is positive and short.",
    "Reply later. The lemmings are still marching and the router is still counting.",
    "This is not a drill. This is a load test, which is much worse.",
    "The agent ran successfully. Nobody knows what it did, but it ran successfully.",
    "Remember: a full inbox is just an archive with a bad attitude.",
    "In case of fire, do not forget to commit before you leave the building.",
    "The ID vault is safe. The people who know the password are not."
};

#define COUNT_OF(a)  (sizeof (a) / sizeof ((a)[0]))


/* Case insensitive compare of ASCII names, without a platform specific call */

static bool SameName (const char *pszA, const char *pszB)
{
    while (*pszA && *pszB)
    {
        char chA = (*pszA >= 'A' && *pszA <= 'Z') ? (char) (*pszA - 'A' + 'a') : *pszA;
        char chB = (*pszB >= 'A' && *pszB <= 'Z') ? (char) (*pszB - 'A' + 'a') : *pszB;

        if (chA != chB)
            return false;

        pszA++;
        pszB++;
    }

    return *pszA == *pszB;
}


bool TextStyleFromName (const char *pszName, TextStyle &Style)
{
    bool bFound = true;

    if (NULL == pszName)
        bFound = false;
    else if (SameName (pszName, "lorem"))
        Style = TEXT_LOREM;
    else if (SameName (pszName, "funny"))
        Style = TEXT_FUNNY;
    else
        bFound = false;

    return bFound;
}


std::string RandomBytes (size_t nBytes, unsigned long ulSeed)
{
    std::mt19937 Random ((std::mt19937::result_type) ulSeed);
    std::string  Data;

    Data.reserve (nBytes);

    while (Data.size() < nBytes)
    {
        std::mt19937::result_type Value = Random();

        for (size_t nByte = 0; (nByte < sizeof (Value)) && (Data.size() < nBytes); nByte++)
            Data += (char) ((Value >> (8 * nByte)) & 0xFF);
    }

    return Data;
}


TextGenerator::TextGenerator (TextStyle Style) : m_Style (Style)
{
}


void TextGenerator::SetStyle (TextStyle Style)
{
    m_Style = Style;
}


std::string TextGenerator::Generate (size_t nBytes, unsigned long ulSeed) const
{
    std::string Text;

    if (nBytes)
        Text = (TEXT_FUNNY == m_Style) ? GenerateFunny (nBytes, ulSeed) : GenerateLorem (nBytes, ulSeed);

    return Text;
}


/* Sentences of 6 to 14 words, 2 to 5 sentences per paragraph, an empty line between paragraphs */

std::string TextGenerator::GenerateLorem (size_t nBytes, unsigned long ulSeed) const
{
    std::mt19937 Random ((std::mt19937::result_type) ulSeed);
    std::string  Text;

    while (Text.size() < nBytes)
    {
        size_t nSentences = 2 + (Random() % 4);

        for (size_t nSentence = 0; nSentence < nSentences; nSentence++)
        {
            size_t nWords = 6 + (Random() % 9);

            for (size_t nWord = 0; nWord < nWords; nWord++)
            {
                std::string Word = g_LoremWords[Random() % COUNT_OF (g_LoremWords)];

                if (0 == nWord)
                    Word[0] = (char) (Word[0] - 'a' + 'A');
                else
                    Text += ' ';

                Text += Word;
            }

            Text += (nSentence + 1 < nSentences) ? ". " : ".";
        }

        Text += "\n\n";
    }

    Text.resize (nBytes);
    return Text;
}


/* Paragraphs of 2 to 4 one-liners, an empty line between paragraphs */

std::string TextGenerator::GenerateFunny (size_t nBytes, unsigned long ulSeed) const
{
    std::mt19937 Random ((std::mt19937::result_type) ulSeed);
    std::string  Text;

    while (Text.size() < nBytes)
    {
        size_t nSentences = 2 + (Random() % 3);

        for (size_t nSentence = 0; nSentence < nSentences; nSentence++)
        {
            if (nSentence)
                Text += ' ';

            Text += g_FunnySentences[Random() % COUNT_OF (g_FunnySentences)];
        }

        Text += "\n\n";
    }

    Text.resize (nBytes);
    return Text;
}
