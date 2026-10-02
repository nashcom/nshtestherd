// wire.cpp - flat field list, text/JSON rendering and form decoding

#include "wire.h"

#include <cstdio>

void Fields::Add(const std::string &key, const std::string &value)
{
    items_.push_back({ key, value, false });
}

void Fields::Add(const std::string &key, const char *value)
{
    items_.push_back({ key, value, false });
}

void Fields::Add(const std::string &key, long long value)
{
    items_.push_back({ key, std::to_string(value), true });
}

std::string EscapeText(const std::string &value)
{
    std::string out;
    out.reserve(value.size());

    for (char c : value)
    {
        switch (c)
        {
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c;
        }
    }

    return out;
}

std::string RenderText(const Fields &fields)
{
    std::string out;

    for (const Fields::Item &item : fields.Items())
    {
        out += item.key;
        out += '=';
        out += EscapeText(item.value);
        out += '\n';
    }

    return out;
}

namespace
{

// Returns the length of the valid UTF-8 sequence at s[i], or 0 if invalid.
size_t Utf8SequenceLength(const std::string &s, size_t i)
{
    unsigned char c = (unsigned char)s[i];
    size_t        len;
    unsigned      min;

    if (c < 0x80)
        return 1;
    else if (c >= 0xC2 && c <= 0xDF)
    {
        len = 2;
        min = 0x80;
    }
    else if (c >= 0xE0 && c <= 0xEF)
    {
        len = 3;
        min = 0x800;
    }
    else if (c >= 0xF0 && c <= 0xF4)
    {
        len = 4;
        min = 0x10000;
    }
    else
        return 0;

    if (i + len > s.size())
        return 0;

    unsigned cp = c & (0xFF >> (len + 1));

    for (size_t k = 1; k < len; k++)
    {
        unsigned char cc = (unsigned char)s[i + k];

        if (0x80 != (cc & 0xC0))
            return 0;

        cp = (cp << 6) | (cc & 0x3F);
    }

    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
        return 0;

    return len;
}

} // namespace

std::string EscapeJson(const std::string &value)
{
    std::string out;
    out.reserve(value.size() + 2);

    for (size_t i = 0; i < value.size();)
    {
        unsigned char c = (unsigned char)value[i];

        if (c >= 0x80)
        {
            size_t len = Utf8SequenceLength(value, i);

            if (0 == len)
            {
                out += "\\ufffd";
                i++;
            }
            else
            {
                out.append(value, i, len);
                i += len;
            }

            continue;
        }

        i++;

        switch (c)
        {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20 || 0x7F == c)
                {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                }
                else
                    out += (char)c;
        }
    }

    return out;
}

std::string RenderJson(const Fields &fields)
{
    std::string out = "{";
    bool        first = true;

    for (const Fields::Item &item : fields.Items())
    {
        if (!first)
            out += ',';

        first = false;
        out += '"';
        out += EscapeJson(item.key);
        out += "\":";

        if (item.isNumber)
            out += item.value;
        else
        {
            out += '"';
            out += EscapeJson(item.value);
            out += '"';
        }
    }

    out += "}\n";
    return out;
}

namespace
{

int HexValue(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';

    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;

    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;

    return -1;
}

bool UrlDecode(const std::string &in, std::string &out)
{
    out.clear();

    for (size_t i = 0; i < in.size(); i++)
    {
        char c = in[i];

        if ('+' == c)
            out += ' ';
        else if ('%' == c)
        {
            if (i + 2 >= in.size())
                return false;

            int hi = HexValue(in[i + 1]);
            int lo = HexValue(in[i + 2]);

            if (hi < 0 || lo < 0)
                return false;

            char decoded = (char)((hi << 4) | lo);

            if (0 == decoded)
                return false;

            out += decoded;
            i += 2;
        }
        else
            out += c;
    }

    return true;
}

} // namespace

bool ParseForm(const std::string &encoded, Form &form, std::string &err)
{
    form.clear();

    // Tolerate one trailing line break (echo / file based curl bodies)
    size_t end = encoded.size();

    if (end > 0 && '\n' == encoded[end - 1])
        end--;

    if (end > 0 && '\r' == encoded[end - 1])
        end--;

    size_t pos = 0;

    while (pos < end)
    {
        size_t amp = encoded.find('&', pos);

        if (std::string::npos == amp || amp > end)
            amp = end;

        std::string pair = encoded.substr(pos, amp - pos);
        pos              = amp + 1;

        size_t eq = pair.find('=');

        if (std::string::npos == eq)
        {
            err = "malformed form field (missing '=')";
            return false;
        }

        std::string key;
        std::string value;

        if (!UrlDecode(pair.substr(0, eq), key) || !UrlDecode(pair.substr(eq + 1), value))
        {
            err = "malformed percent escape";
            return false;
        }

        if (key.empty())
        {
            err = "empty field name";
            return false;
        }

        if (!form.insert({ key, value }).second)
        {
            err = "duplicate field: " + key;
            return false;
        }
    }

    return true;
}

std::string UrlEncode(const std::string &value)
{
    static const char *hex = "0123456789ABCDEF";
    std::string        out;

    for (char c : value)
    {
        unsigned char u = (unsigned char)c;

        if ((u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') || (u >= '0' && u <= '9') || '-' == c || '_' == c || '.' == c || '~' == c)
            out += c;
        else
        {
            out += '%';
            out += hex[u >> 4];
            out += hex[u & 0x0F];
        }
    }

    return out;
}

void ParseTextFields(const std::string &body, std::map<std::string, std::string> &out)
{
    out.clear();

    size_t pos = 0;

    while (pos < body.size())
    {
        size_t end = body.find('\n', pos);

        if (std::string::npos == end)
            end = body.size();

        std::string line = body.substr(pos, end - pos);
        pos              = end + 1;

        size_t eq = line.find('=');

        if (std::string::npos == eq)
            continue;

        std::string raw = line.substr(eq + 1);
        std::string value;

        for (size_t i = 0; i < raw.size(); i++)
        {
            if ('\\' == raw[i] && i + 1 < raw.size())
            {
                char n = raw[i + 1];

                if ('n' == n || 'r' == n || 't' == n || '\\' == n)
                {
                    value += ('n' == n) ? '\n' : ('r' == n) ? '\r' : ('t' == n) ? '\t' : '\\';
                    i++;
                    continue;
                }
            }

            value += raw[i];
        }

        out[line.substr(0, eq)] = value;
    }
}
