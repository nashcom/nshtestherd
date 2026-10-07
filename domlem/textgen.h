/* textgen.h - generates body text for the load mails (implemented in textgen.cpp)
 *
 * Plain C++, no Notes headers. The text of a message is made from a seed: the same seed gives the same text, so a
 * test run can be repeated, and different seeds (the message number) give different messages.
 *
 *   TextGenerator Generator (TEXT_LOREM);
 *   std::string   Body = Generator.Generate (4096, 17);      // 4096 bytes of text for message 17
 */

#ifndef DOMLEM_TEXTGEN_H
#define DOMLEM_TEXTGEN_H

#include <string>

enum TextStyle
{
    TEXT_LOREM,       /* lorem ipsum: sentences made of the classic latin words */
    TEXT_FUNNY        /* made-up office and IT one-liners */
};

/* "lorem" or "funny" (case insensitive). Returns false for any other name and leaves Style alone. */
bool TextStyleFromName (const char *pszName, TextStyle &Style);

/* Random bytes (binary, does not compress), the same for the same seed. For attachments. */
std::string RandomBytes (size_t nBytes, unsigned long ulSeed);

class TextGenerator
{
public:

    explicit TextGenerator (TextStyle Style = TEXT_LOREM);

    void SetStyle (TextStyle Style);

    /* Exactly nBytes bytes of text (lines separated by \n, paragraphs by an empty line), the same for the same seed */
    std::string Generate (size_t nBytes, unsigned long ulSeed) const;

private:

    std::string GenerateLorem (size_t nBytes, unsigned long ulSeed) const;
    std::string GenerateFunny (size_t nBytes, unsigned long ulSeed) const;

    TextStyle m_Style;
};

#endif /* DOMLEM_TEXTGEN_H */
