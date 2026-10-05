#include "BulkCommands.hpp"

#include <string>
#include <vector>
#include <cstdint>


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
        s.push_back (kHex[bytes[i] & 0x0F]);
    }
    return s;
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
            }
        },
        "additionalProperties": false,
        "required": [ "payload_b64" ]
    })";
}

GS::Optional<GS::UniString> BulkPingCommand::GetResponseSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "size":       { "type": "integer" },
            "preview_hex":{ "type": "string" }
        },
        "additionalProperties": false,
        "required": [ "size", "preview_hex" ]
    })";
}

GS::ObjectState BulkPingCommand::Execute (const GS::ObjectState& parameters,
                                          GS::ProcessControl& /*processControl*/) const
{
    GS::UniString payloadB64;
    if (!parameters.Get ("payload_b64", payloadB64)) {
        return CreateErrorResponse (APIERR_BADPARS, "payload_b64 is missing");
    }

    const std::string in = payloadB64.ToCStr ().Get ();
    const std::vector<uint8_t> bytes = Base64Decode (in);

    GS::ObjectState response;
    response.Add ("size", static_cast<Int64> (bytes.size ()));
    response.Add ("preview_hex", GS::UniString (ToHexPreview (bytes, 16).c_str ()));
    return response;
}
