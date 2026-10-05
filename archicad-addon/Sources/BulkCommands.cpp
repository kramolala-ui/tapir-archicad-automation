#include "BulkCommands.hpp"

#include <string>
#include <vector>
#include <cstdint>

#include <zstd.h>


// ---------------------------------------------------------------------
//  Base64 decode (своя реализация, без внешних зависимостей)
// ---------------------------------------------------------------------

namespace {

const char kBase64Chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::vector<uint8_t> Base64Decode (const std::string& in)
{
    std::vector<int> table (256, -1);
    for (int i = 0; i < 64; ++i) {
        table[static_cast<unsigned char> (kBase64Chars[i])] = i;
    }

    std::vector<uint8_t> out;
    out.reserve (in.size () * 3 / 4 + 4);

    int buf = 0;
    int bits = -8;
    for (unsigned char c : in) {
        if (c == '=') {
            break;
        }
        const int v = table[c];
        if (v < 0) {
            // whitespace и прочее — пропускаем
            continue;
        }
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 0) {
            out.push_back (static_cast<uint8_t> ((buf >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return out;
}

std::string ToHexPreview (const std::vector<uint8_t>& bytes, size_t maxLen)
{
    static const char kHex[] = "0123456789abcdef";
    const size_t n = bytes.size () < maxLen ? bytes.size () : maxLen;
    std::string s;
    s.reserve (n * 2);
    for (size_t i = 0; i < n; ++i) {
        s.push_back (kHex[(bytes[i] >> 4) & 0x0F]);
        s.push_back (kHex[(bytes[i] & 0x0F)]);
    }
    return s;
}

// ZSTD_decompress требует заранее известный размер выхода.
// Python-клиент (zstandard.ZstdCompressor().compress(data))
// пишет content size в заголовок фрейма, поэтому
// ZSTD_getFrameContentSize возвращает точное значение.
//
// Если получен ZSTD_CONTENTSIZE_UNKNOWN — вернём ошибку
// «streaming frames not supported» (в будущем можно добавить
// стриминговое разжатие, но пока не нужно).
bool ZstdDecompress (const std::vector<uint8_t>& src,
                     std::vector<uint8_t>& out,
                     std::string& errorOut)
{
    if (src.empty ()) {
        errorOut = "zstd: empty input";
        return false;
    }

    const unsigned long long contentSize =
        ZSTD_getFrameContentSize (src.data (), src.size ());

    if (contentSize == ZSTD_CONTENTSIZE_ERROR) {
        errorOut = "zstd: not a valid zstd frame";
        return false;
    }
    if (contentSize == ZSTD_CONTENTSIZE_UNKNOWN) {
        errorOut = "zstd: streaming frames not supported (content size unknown)";
        return false;
    }

    out.resize (static_cast<size_t> (contentSize));
    const size_t n = ZSTD_decompress (
        out.data (), out.size (),
        src.data (), src.size ());

    if (ZSTD_isError (n)) {
        errorOut = std::string ("zstd: ") + ZSTD_getErrorName (n);
        return false;
    }
    if (n != out.size ()) {
        out.resize (n);
    }
    return true;
}

}  // namespace


// ---------------------------------------------------------------------
//  BulkPingCommand
// ---------------------------------------------------------------------

BulkPingCommand::BulkPingCommand () :
    CommandBase (CommonSchema::Used)
{
}

GS::String BulkPingCommand::GetName () const
{
    return "BulkPing";
}

GS::Optional<GS::UniString> BulkPingCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "payload_b64": {
                "type": "string",
                "description": "Base64-encoded binary payload. Any bytes accepted."
            },
            "compression": {
                "type": "string",
                "enum": [ "none", "zstd" ],
                "description": "Compression applied to the raw payload before base64. Default: 'none'."
            }
        },
        "additionalProperties": false,
        "required": [ "payload_b64" ]
    })";
}

GS::Optional<GS::UniString> BulkPingCommand::GetRawResponseSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "size":          { "type": "integer" },
            "preview_hex":   { "type": "string"  },
            "compression":   { "type": "string"  },
            "zstd_version":  { "type": "integer" }
        },
        "additionalProperties": false,
        "required": [ "size", "preview_hex", "compression" ]
    })";
}

GS::ObjectState BulkPingCommand::Execute (const GS::ObjectState& parameters,
                                          GS::ProcessControl& /*processControl*/) const
{
    GS::UniString payloadB64;
    if (!parameters.Get ("payload_b64", payloadB64)) {
        return CreateErrorResponse (APIERR_BADPARS, "payload_b64 is missing");
    }

    GS::UniString compressionUs;
    parameters.Get ("compression", compressionUs);
    const std::string compression = compressionUs.ToCStr ().Get ();

    const std::string in = payloadB64.ToCStr ().Get ();
    std::vector<uint8_t> bytes = Base64Decode (in);

    if (compression == "zstd") {
        std::vector<uint8_t> decompressed;
        std::string err;
        if (!ZstdDecompress (bytes, decompressed, err)) {
            return CreateErrorResponse (APIERR_BADPARS,
                GS::UniString (err.c_str ()));
        }
        bytes.swap (decompressed);
    } else if (!compression.empty () && compression != "none") {
        return CreateErrorResponse (APIERR_BADPARS,
            GS::UniString::Printf (
                "Unknown compression: '%T'", compressionUs.ToPrintf ()));
    }

    GS::ObjectState response;
    response.Add ("size", static_cast<Int64> (bytes.size ()));
    response.Add ("preview_hex", GS::UniString (ToHexPreview (bytes, 16).c_str ()));
    response.Add ("compression", GS::UniString (compression.c_str ()));
    response.Add ("zstd_version", static_cast<Int32> (ZSTD_versionNumber ()));
    return response;
}
