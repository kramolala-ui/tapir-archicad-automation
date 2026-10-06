#include "BulkCommands.hpp"
#include "MigrationHelper.hpp"
#include "ElementCreationCommands.hpp"

#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <cstdint>
#include <cstring>

#include <zstd.h>
#include <msgpack.hpp>


// ---------------------------------------------------------------------
//  Base64 (своя реализация, без внешних зависимостей)
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
    int buf = 0, bits = -8;
    for (unsigned char c : in) {
        if (c == '=') break;
        const int v = table[c];
        if (v < 0) continue;
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 0) {
            out.push_back (static_cast<uint8_t> ((buf >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return out;
}

std::string Base64Encode (const std::vector<uint8_t>& in)
{
    std::string out;
    out.reserve (((in.size () + 2) / 3) * 4);
    size_t i = 0;
    while (i + 3 <= in.size ()) {
        const uint32_t n = (uint32_t (in[i]) << 16) |
                           (uint32_t (in[i+1]) << 8) |
                            uint32_t (in[i+2]);
        out.push_back (kBase64Chars[(n >> 18) & 0x3F]);
        out.push_back (kBase64Chars[(n >> 12) & 0x3F]);
        out.push_back (kBase64Chars[(n >> 6) & 0x3F]);
        out.push_back (kBase64Chars[n & 0x3F]);
        i += 3;
    }
    if (i + 1 == in.size ()) {
        const uint32_t n = uint32_t (in[i]) << 16;
        out.push_back (kBase64Chars[(n >> 18) & 0x3F]);
        out.push_back (kBase64Chars[(n >> 12) & 0x3F]);
        out.push_back ('=');
        out.push_back ('=');
    } else if (i + 2 == in.size ()) {
        const uint32_t n = (uint32_t (in[i]) << 16) |
                           (uint32_t (in[i+1]) << 8);
        out.push_back (kBase64Chars[(n >> 18) & 0x3F]);
        out.push_back (kBase64Chars[(n >> 12) & 0x3F]);
        out.push_back (kBase64Chars[(n >> 6) & 0x3F]);
        out.push_back ('=');
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
        errorOut = "zstd: streaming frames not supported";
        return false;
    }
    out.resize (static_cast<size_t> (contentSize));
    const size_t n = ZSTD_decompress (
        out.data (), out.size (), src.data (), src.size ());
    if (ZSTD_isError (n)) {
        errorOut = std::string ("zstd: ") + ZSTD_getErrorName (n);
        return false;
    }
    if (n != out.size ()) out.resize (n);
    return true;
}

std::vector<uint8_t> ZstdCompress (const std::vector<uint8_t>& src)
{
    const size_t bound = ZSTD_compressBound (src.size ());
    std::vector<uint8_t> out (bound);
    const size_t n = ZSTD_compress (
        out.data (), out.size (), src.data (), src.size (), 3);
    if (ZSTD_isError (n)) {
        return {};
    }
    out.resize (n);
    return out;
}

// --- envelope helpers (общий транспорт для всех Bulk*-команд) ---------

bool DecodeEnvelope (const GS::UniString& payloadB64,
                     std::string compression,
                     std::vector<uint8_t>& raw,
                     std::string& errorOut)
{
    if (compression.empty ()) compression = "none";
    raw = Base64Decode (payloadB64.ToCStr ().Get ());
    if (compression == "zstd") {
        std::vector<uint8_t> dec;
        if (!ZstdDecompress (raw, dec, errorOut)) return false;
        raw.swap (dec);
        return true;
    }
    if (compression != "none") {
        errorOut = "Unknown compression: '" + compression + "'";
        return false;
    }
    return true;
}

std::string EncodeEnvelope (const void* data, size_t size,
                            std::string& outCompression)
{
    std::vector<uint8_t> src (
        reinterpret_cast<const uint8_t*> (data),
        reinterpret_cast<const uint8_t*> (data) + size);
    std::vector<uint8_t> toSend = src;
    outCompression = "none";
    auto comp = ZstdCompress (src);
    if (!comp.empty ()) {
        toSend.swap (comp);
        outCompression = "zstd";
    }
    return Base64Encode (toSend);
}

// Чтение содержимого memo.textContent для Text/Label (обе версии API).
GS::UniString ReadTextFromMemo (const API_ElementMemo& memo)
{
#ifdef ServerMainVers_2800
    if (memo.textContent != nullptr) return *memo.textContent;
#else
    if (memo.textContent != nullptr) {
        const GS::uchar_t* ustr =
            reinterpret_cast<const GS::uchar_t*> (*memo.textContent);
        if (ustr != nullptr) return GS::UniString (ustr);
    }
#endif
    return GS::UniString ();
}

// Собрать GUID'ы всех Text + Label элементов проекта.
void CollectAllTextElementGuids (GS::Array<API_Guid>& out)
{
    GS::Array<API_Guid> texts;
    if (ACAPI_Element_GetElemList (API_TextID, &texts) == NoError) {
        for (const API_Guid& g : texts) out.Push (g);
    }
    GS::Array<API_Guid> labels;
    if (ACAPI_Element_GetElemList (API_LabelID, &labels) == NoError) {
        for (const API_Guid& g : labels) out.Push (g);
    }
}

// Общий путь записи: SetTextContentAndParagraphs + маска полей + Change.
// isLabel = true, если element.label.u.text; false — для element.text.
GSErrCode ApplyTextToElement (API_Element& element,
                              const GS::UniString& text,
                              bool isLabel)
{
    API_Element mask = {};
    API_ElementMemo clipMemo = {};
    if (isLabel) {
        SetTextContentAndParagraphs (clipMemo, element.label.u.text, text);
        ACAPI_ELEMENT_MASK_SET (mask, API_LabelType, u.text.nLine);
        ACAPI_ELEMENT_MASK_SET (mask, API_LabelType, u.text.useEolPos);
        ACAPI_ELEMENT_MASK_SET (mask, API_LabelType, u.text.nonBreaking);
        ACAPI_ELEMENT_MASK_SET (mask, API_LabelType, u.text.width);
        ACAPI_ELEMENT_MASK_SET (mask, API_LabelType, u.text.height);
#ifndef ServerMainVers_2800
        ACAPI_ELEMENT_MASK_SET (mask, API_LabelType, u.text.charCode);
#endif
    } else {
        SetTextContentAndParagraphs (clipMemo, element.text, text);
        ACAPI_ELEMENT_MASK_SET (mask, API_TextType, nLine);
        ACAPI_ELEMENT_MASK_SET (mask, API_TextType, useEolPos);
        ACAPI_ELEMENT_MASK_SET (mask, API_TextType, nonBreaking);
        ACAPI_ELEMENT_MASK_SET (mask, API_TextType, width);
        ACAPI_ELEMENT_MASK_SET (mask, API_TextType, height);
#ifndef ServerMainVers_2800
        ACAPI_ELEMENT_MASK_SET (mask, API_TextType, charCode);
#endif
    }
    const GSErrCode err = ACAPI_Element_Change (&element, &mask, &clipMemo,
        APIMemoMask_TextContent | APIMemoMask_Paragraph, true);
    ACAPI_DisposeElemMemoHdls (&clipMemo);
    return err;
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
    std::string compression = compressionUs.ToCStr ().Get ();
    if (compression.empty ()) compression = "none";

    const std::string in = payloadB64.ToCStr ().Get ();
    std::vector<uint8_t> bytes = Base64Decode (in);

    if (compression == "zstd") {
        std::vector<uint8_t> decompressed;
        std::string err;
        if (!ZstdDecompress (bytes, decompressed, err)) {
            return CreateErrorResponse (APIERR_BADPARS, GS::UniString (err.c_str ()));
        }
        bytes.swap (decompressed);
    } else if (compression != "none") {
        const std::string msg = "Unknown compression: '" + compression + "'";
        return CreateErrorResponse (APIERR_BADPARS, GS::UniString (msg.c_str ()));
    }

    GS::ObjectState response;
    response.Add ("size", static_cast<Int64> (bytes.size ()));
    response.Add ("preview_hex", GS::UniString (ToHexPreview (bytes, 16).c_str ()));
    response.Add ("compression", GS::UniString (compression.c_str ()));
    response.Add ("zstd_version", static_cast<Int32> (ZSTD_versionNumber ()));
    return response;
}


// ---------------------------------------------------------------------
//  BulkGetPropertyValuesCommand
// ---------------------------------------------------------------------
//
// Вход (msgpack внутри payload_b64):
//   { "elements":   ["guid", ...],   // elementId.guid
//     "properties": ["guid", ...] }  // propertyId.guid
//
// Выход (msgpack внутри payload_b64):
//   { "rows": [
//       { "elementId": "guid",
//         "propertyValues": [
//           {"propertyId": "guid", "value": "..."},
//           ...
//         ]},
//       ... ] }
//
// Внутри читаем через ACAPI_Element_GetPropertyValuesByGuid, но
// порциями по 20 свойств (быстрый путь ACAPI — порог K=24,
// см. AI_PRINCIPLES.md, раздел 9). Цикл по элементам — 500 шт.
// за один Execute.

namespace {

constexpr size_t kPropertyChunkSize = 20;

GS::Optional<GS::UniString> ReadPropertyValueString (const API_Property& prop)
{
    if (prop.status == API_Property_NotAvailable ||
        prop.status == API_Property_NotEvaluated) {
        return {};
    }
    GS::UniString out;
    GSErrCode err = ACAPI_Property_GetPropertyValueString (prop, &out);
    if (err != NoError) return {};
    return out;
}

}  // namespace

BulkGetPropertyValuesCommand::BulkGetPropertyValuesCommand () :
    CommandBase (CommonSchema::Used)
{
}

GS::String BulkGetPropertyValuesCommand::GetName () const
{
    return "BulkGetPropertyValues";
}

GS::Optional<GS::UniString> BulkGetPropertyValuesCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "payload_b64": { "type": "string" },
            "compression": { "type": "string", "enum": [ "none", "zstd" ] }
        },
        "additionalProperties": false,
        "required": [ "payload_b64" ]
    })";
}

GS::Optional<GS::UniString> BulkGetPropertyValuesCommand::GetRawResponseSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "payload_b64":     { "type": "string"  },
            "compression":     { "type": "string"  },
            "elements_count":  { "type": "integer" },
            "properties_count":{ "type": "integer" },
            "values_count":    { "type": "integer" }
        },
        "additionalProperties": false,
        "required": [ "payload_b64", "compression" ]
    })";
}

GS::ObjectState BulkGetPropertyValuesCommand::Execute (
    const GS::ObjectState& parameters,
    GS::ProcessControl& /*processControl*/) const
{
    GS::UniString payloadB64;
    if (!parameters.Get ("payload_b64", payloadB64)) {
        return CreateErrorResponse (APIERR_BADPARS, "payload_b64 is missing");
    }
    GS::UniString compressionUs;
    parameters.Get ("compression", compressionUs);
    std::string compression = compressionUs.ToCStr ().Get ();
    if (compression.empty ()) compression = "none";

    if (compression != "none" && compression != "zstd") {
        const std::string msg = "Unknown compression: '" + compression + "'";
        return CreateErrorResponse (APIERR_BADPARS, GS::UniString (msg.c_str ()));
    }

    // ---- 1. base64 → (zstd) → msgpack ----
    std::vector<uint8_t> raw = Base64Decode (payloadB64.ToCStr ().Get ());
    if (compression == "zstd") {
        std::vector<uint8_t> decompressed;
        std::string err;
        if (!ZstdDecompress (raw, decompressed, err)) {
            return CreateErrorResponse (APIERR_BADPARS, GS::UniString (err.c_str ()));
        }
        raw.swap (decompressed);
    }

    std::vector<std::string> elemGuids;
    std::vector<std::string> propGuids;
    try {
        auto oh = msgpack::unpack (reinterpret_cast<const char*> (raw.data ()), raw.size ());
        auto obj = oh.get ();
        auto m = obj.as<std::map<std::string, msgpack::object>> ();
        auto itEl = m.find ("elements");
        auto itPr = m.find ("properties");
        if (itEl == m.end () || itPr == m.end ()) {
            return CreateErrorResponse (APIERR_BADPARS,
                "payload must contain 'elements' and 'properties'");
        }
        itEl->second.convert (elemGuids);
        itPr->second.convert (propGuids);
    } catch (const std::exception& e) {
        const std::string msg = std::string ("msgpack decode failed: ") + e.what ();
        return CreateErrorResponse (APIERR_BADPARS, GS::UniString (msg.c_str ()));
    }

    const size_t N = elemGuids.size ();
    const size_t K = propGuids.size ();

    // ---- 2. Пройти по элементам × чанкам свойств ----
    // Структура: rows[i] — map elementId → list of (propertyId, value)
    msgpack::sbuffer outBuf;
    msgpack::packer<msgpack::sbuffer> pk (&outBuf);

    pk.pack_map (1);
    pk.pack (std::string ("rows"));
    pk.pack_array (N);

    size_t valuesCount = 0;

    for (size_t i = 0; i < N; ++i) {
        // --- row i ---
        pk.pack_map (2);
        pk.pack (std::string ("elementId"));
        pk.pack (elemGuids[i]);
        pk.pack (std::string ("propertyValues"));
        pk.pack_array (K);

        API_Guid elemGuid = APIGuidFromString (elemGuids[i].c_str ());
        if (elemGuid == APINULLGuid) {
            // Пустой гуид — вернём K пустых записей
            for (size_t k = 0; k < K; ++k) {
                pk.pack_map (2);
                pk.pack (std::string ("propertyId"));
                pk.pack (propGuids[k]);
                pk.pack (std::string ("value"));
                pk.pack (std::string (""));
            }
            continue;
        }

        // Проходим по чанкам свойств (по 20) и собираем значения
        std::vector<std::string> values (K);
        for (size_t start = 0; start < K; start += kPropertyChunkSize) {
            const size_t end = std::min (start + kPropertyChunkSize, K);
            GS::Array<API_Guid> chunkGuids;
            for (size_t k = start; k < end; ++k) {
                API_Guid g = APIGuidFromString (propGuids[k].c_str ());
                if (g != APINULLGuid) chunkGuids.Push (g);
            }
            GS::Array<API_Property> fetched;
            GSErrCode err = ACAPI_Element_GetPropertyValuesByGuid (
                elemGuid, chunkGuids, fetched);
            if (err != NoError) {
                continue;
            }
            // Сопоставляем: fetched[i].definition.guid ↔ chunkGuids[i]
            for (const API_Property& p : fetched) {
                API_Guid pg = p.definition.guid;
                for (size_t k = start; k < end; ++k) {
                    if (APIGuidFromString (propGuids[k].c_str ()) == pg) {
                        auto s = ReadPropertyValueString (p);
                        if (s.HasValue ()) {
                            values[k] = s->ToCStr ().Get ();
                        }
                        break;
                    }
                }
            }
        }

        for (size_t k = 0; k < K; ++k) {
            pk.pack_map (2);
            pk.pack (std::string ("propertyId"));
            pk.pack (propGuids[k]);
            pk.pack (std::string ("value"));
            pk.pack (values[k]);
            if (!values[k].empty ()) ++valuesCount;
        }
    }

    // ---- 3. Сжать ответ и закодировать ----
    std::vector<uint8_t> outBytes (
        reinterpret_cast<const uint8_t*> (outBuf.data ()),
        reinterpret_cast<const uint8_t*> (outBuf.data ()) + outBuf.size ());

    std::string outCompression = "none";
    std::vector<uint8_t> toSend = outBytes;
    {
        auto compressed = ZstdCompress (outBytes);
        if (!compressed.empty ()) {
            toSend.swap (compressed);
            outCompression = "zstd";
        }
    }

    const std::string outB64 = Base64Encode (toSend);

    GS::ObjectState response;
    response.Add ("payload_b64", GS::UniString (outB64.c_str ()));
    response.Add ("compression", GS::UniString (outCompression.c_str ()));
    response.Add ("elements_count", static_cast<Int64> (N));
    response.Add ("properties_count", static_cast<Int64> (K));
    response.Add ("values_count", static_cast<Int64> (valuesCount));
    return response;
}


// ---------------------------------------------------------------------
//  BulkGetTextsCommand
// ---------------------------------------------------------------------
//
// Вход (msgpack в payload_b64):  { "elements": ["guid", ...] }
// Выход (msgpack в payload_b64): { "rows": [
//   {"elementId": "...", "type": "Text"|"Label"|"", "text": "..."} ] }

BulkGetTextsCommand::BulkGetTextsCommand () :
    CommandBase (CommonSchema::Used)
{
}

GS::String BulkGetTextsCommand::GetName () const
{
    return "BulkGetTexts";
}

GS::Optional<GS::UniString> BulkGetTextsCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "payload_b64": { "type": "string" },
            "compression": { "type": "string", "enum": [ "none", "zstd" ] }
        },
        "additionalProperties": false,
        "required": [ "payload_b64" ]
    })";
}

GS::Optional<GS::UniString> BulkGetTextsCommand::GetRawResponseSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "payload_b64":    { "type": "string"  },
            "compression":    { "type": "string"  },
            "elements_count": { "type": "integer" },
            "texts_count":    { "type": "integer" }
        },
        "additionalProperties": false,
        "required": [ "payload_b64", "compression" ]
    })";
}

GS::ObjectState BulkGetTextsCommand::Execute (
    const GS::ObjectState& parameters,
    GS::ProcessControl& /*processControl*/) const
{
    GS::UniString payloadB64;
    if (!parameters.Get ("payload_b64", payloadB64)) {
        return CreateErrorResponse (APIERR_BADPARS, "payload_b64 is missing");
    }
    GS::UniString compressionUs;
    parameters.Get ("compression", compressionUs);
    std::string compression = compressionUs.ToCStr ().Get ();

    std::vector<uint8_t> raw;
    std::string err;
    if (!DecodeEnvelope (payloadB64, compression, raw, err)) {
        return CreateErrorResponse (APIERR_BADPARS, GS::UniString (err.c_str ()));
    }

    std::vector<std::string> elemGuids;
    try {
        auto oh = msgpack::unpack (reinterpret_cast<const char*> (raw.data ()), raw.size ());
        auto obj = oh.get ();
        auto m = obj.as<std::map<std::string, msgpack::object>> ();
        auto itEl = m.find ("elements");
        if (itEl == m.end ()) {
            return CreateErrorResponse (APIERR_BADPARS,
                "payload must contain 'elements'");
        }
        itEl->second.convert (elemGuids);
    } catch (const std::exception& e) {
        const std::string msg = std::string ("msgpack decode failed: ") + e.what ();
        return CreateErrorResponse (APIERR_BADPARS, GS::UniString (msg.c_str ()));
    }

    msgpack::sbuffer outBuf;
    msgpack::packer<msgpack::sbuffer> pk (&outBuf);
    pk.pack_map (1);
    pk.pack (std::string ("rows"));
    pk.pack_array (elemGuids.size ());

    size_t foundCount = 0;
    for (const std::string& guidStr : elemGuids) {
        std::string typeStr, textStr;
        API_Element element = {};
        element.header.guid = APIGuidFromString (guidStr.c_str ());
        if (element.header.guid != APINULLGuid &&
            ACAPI_Element_Get (&element) == NoError) {
            // GetMemo дёргаем ТОЛЬКО для Text/Label — иначе лишний вызов
            // на каждый Wall/Slab в батче. Тип элемента — через GetElemTypeId:
            // у API_Elem_Head нет поля typeID (см. APIdefs_Elements.h).
            const bool isText = (GetElemTypeId (element.header) == API_TextID);
            const bool isLabel = (GetElemTypeId (element.header) == API_LabelID &&
                                  element.label.labelClass == APILblClass_Text);
            if (isText || isLabel) {
                API_ElementMemo memo = {};
                GS::UniString txt;
                if (ACAPI_Element_GetMemo (element.header.guid, &memo,
                        APIMemoMask_TextContent | APIMemoMask_Paragraph) == NoError) {
                    txt = ReadTextFromMemo (memo);
                }
                ACAPI_DisposeElemMemoHdls (&memo);
                typeStr = isText ? "Text" : "Label";
                textStr = txt.ToCStr ().Get ();
                ++foundCount;
            }
        }

        pk.pack_map (3);
        pk.pack (std::string ("elementId")); pk.pack (guidStr);
        pk.pack (std::string ("type"));      pk.pack (typeStr);
        pk.pack (std::string ("text"));      pk.pack (textStr);
    }

    std::string outCompression;
    const std::string outB64 =
        EncodeEnvelope (outBuf.data (), outBuf.size (), outCompression);

    GS::ObjectState response;
    response.Add ("payload_b64", GS::UniString (outB64.c_str ()));
    response.Add ("compression", GS::UniString (outCompression.c_str ()));
    response.Add ("elements_count", static_cast<Int64> (elemGuids.size ()));
    response.Add ("texts_count", static_cast<Int64> (foundCount));
    return response;
}


// ---------------------------------------------------------------------
//  BulkSetTextsCommand
// ---------------------------------------------------------------------
//
// Вход (msgpack в payload_b64): { "rows": [{"elementId": "...", "text": "..."}] }
// Выход (msgpack в payload_b64): { "updated": N, "total": M,
//   "errors": [{"elementId": "...", "message": "..."}] }
//
// Переиспользует SetTextContentAndParagraphs (тот же путь, что
// CreateTexts/ModifyTexts) через ApplyTextToElement.

BulkSetTextsCommand::BulkSetTextsCommand () :
    CommandBase (CommonSchema::Used)
{
}

GS::String BulkSetTextsCommand::GetName () const
{
    return "BulkSetTexts";
}

GS::Optional<GS::UniString> BulkSetTextsCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "payload_b64": { "type": "string" },
            "compression": { "type": "string", "enum": [ "none", "zstd" ] }
        },
        "additionalProperties": false,
        "required": [ "payload_b64" ]
    })";
}

GS::Optional<GS::UniString> BulkSetTextsCommand::GetRawResponseSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "payload_b64":   { "type": "string"  },
            "compression":   { "type": "string"  },
            "updated_count": { "type": "integer" },
            "rows_count":    { "type": "integer" }
        },
        "additionalProperties": false,
        "required": [ "payload_b64", "compression" ]
    })";
}

GS::ObjectState BulkSetTextsCommand::Execute (
    const GS::ObjectState& parameters,
    GS::ProcessControl& /*processControl*/) const
{
    GS::UniString payloadB64;
    if (!parameters.Get ("payload_b64", payloadB64)) {
        return CreateErrorResponse (APIERR_BADPARS, "payload_b64 is missing");
    }
    GS::UniString compressionUs;
    parameters.Get ("compression", compressionUs);
    std::string compression = compressionUs.ToCStr ().Get ();

    std::vector<uint8_t> raw;
    std::string err;
    if (!DecodeEnvelope (payloadB64, compression, raw, err)) {
        return CreateErrorResponse (APIERR_BADPARS, GS::UniString (err.c_str ()));
    }

    struct Row { std::string elementId; std::string text; };
    std::vector<Row> rows;
    try {
        auto oh = msgpack::unpack (reinterpret_cast<const char*> (raw.data ()), raw.size ());
        auto obj = oh.get ();
        auto m = obj.as<std::map<std::string, msgpack::object>> ();
        auto itRows = m.find ("rows");
        if (itRows == m.end ()) {
            return CreateErrorResponse (APIERR_BADPARS, "payload must contain 'rows'");
        }
        std::vector<std::map<std::string, msgpack::object>> rawRows;
        itRows->second.convert (rawRows);
        for (auto& rr : rawRows) {
            auto itId = rr.find ("elementId");
            auto itTx = rr.find ("text");
            if (itId == rr.end () || itTx == rr.end ()) continue;
            Row row;
            itId->second.convert (row.elementId);
            itTx->second.convert (row.text);
            rows.push_back (row);
        }
    } catch (const std::exception& e) {
        const std::string msg = std::string ("msgpack decode failed: ") + e.what ();
        return CreateErrorResponse (APIERR_BADPARS, GS::UniString (msg.c_str ()));
    }

    struct ErrEntry { std::string elementId; std::string message; };
    std::vector<ErrEntry> errors;
    size_t updatedCount = 0;

    // Один undo-барьер на весь батч: без ACAPI_CallUndoableCommand
    // изменения модели не применяются (или применяются, но undo-стек
    // рвётся на каждый элемент). См. AI_PRINCIPLES.md, раздел 5.
    ACAPI_CallUndoableCommand ("BulkSetTexts", [&]() {
        for (const Row& row : rows) {
            API_Guid guid = APIGuidFromString (row.elementId.c_str ());
            if (guid == APINULLGuid) {
                errors.push_back ({row.elementId, "invalid guid"});
                continue;
            }
            API_Element element = {};
            element.header.guid = guid;
            if (ACAPI_Element_Get (&element) != NoError) {
                errors.push_back ({row.elementId, "element not found"});
                continue;
            }
            const GS::UniString text (row.text.c_str ());
            GSErrCode e = NoError;
            if (GetElemTypeId (element.header) == API_TextID) {
                e = ApplyTextToElement (element, text, false);
            } else if (GetElemTypeId (element.header) == API_LabelID &&
                       element.label.labelClass == APILblClass_Text) {
                e = ApplyTextToElement (element, text, true);
            } else {
                errors.push_back ({row.elementId, "unsupported element type"});
                continue;
            }
            if (e == NoError) ++updatedCount;
            else errors.push_back ({row.elementId, "change failed"});
        }
    });

    msgpack::sbuffer outBuf;
    msgpack::packer<msgpack::sbuffer> pk (&outBuf);
    pk.pack_map (3);
    pk.pack (std::string ("updated")); pk.pack (static_cast<uint64_t> (updatedCount));
    pk.pack (std::string ("total"));   pk.pack (static_cast<uint64_t> (rows.size ()));
    pk.pack (std::string ("errors"));
    pk.pack_array (errors.size ());
    for (const ErrEntry& ee : errors) {
        pk.pack_map (2);
        pk.pack (std::string ("elementId")); pk.pack (ee.elementId);
        pk.pack (std::string ("message"));   pk.pack (ee.message);
    }

    std::string outCompression;
    const std::string outB64 =
        EncodeEnvelope (outBuf.data (), outBuf.size (), outCompression);

    GS::ObjectState response;
    response.Add ("payload_b64", GS::UniString (outB64.c_str ()));
    response.Add ("compression", GS::UniString (outCompression.c_str ()));
    response.Add ("updated_count", static_cast<Int64> (updatedCount));
    response.Add ("rows_count", static_cast<Int64> (rows.size ()));
    return response;
}


// ---------------------------------------------------------------------
//  BulkFindReplaceTextCommand
// ---------------------------------------------------------------------
//
// Вход (msgpack в payload_b64):
//   { "find": "...", "replace": "...",
//     "case_sensitive": true, "dry_run": true,
//     "elements": ["guid", ...]   // опц.; без него — все Text+Label проекта
//   }
// Выход (msgpack в payload_b64):
//   { "find": "...", "replace": "...", "dry_run": bool,
//     "scanned_count": N, "matched_count": M, "replaced_count": K,
//     "matches": [{"elementId", "type", "before", "after"}] }

BulkFindReplaceTextCommand::BulkFindReplaceTextCommand () :
    CommandBase (CommonSchema::Used)
{
}

GS::String BulkFindReplaceTextCommand::GetName () const
{
    return "BulkFindReplaceText";
}

GS::Optional<GS::UniString> BulkFindReplaceTextCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "payload_b64": { "type": "string" },
            "compression": { "type": "string", "enum": [ "none", "zstd" ] }
        },
        "additionalProperties": false,
        "required": [ "payload_b64" ]
    })";
}

GS::Optional<GS::UniString> BulkFindReplaceTextCommand::GetRawResponseSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "payload_b64":    { "type": "string"  },
            "compression":    { "type": "string"  },
            "scanned_count":  { "type": "integer" },
            "matched_count":  { "type": "integer" },
            "replaced_count": { "type": "integer" }
        },
        "additionalProperties": false,
        "required": [ "payload_b64", "compression" ]
    })";
}

GS::ObjectState BulkFindReplaceTextCommand::Execute (
    const GS::ObjectState& parameters,
    GS::ProcessControl& /*processControl*/) const
{
    GS::UniString payloadB64;
    if (!parameters.Get ("payload_b64", payloadB64)) {
        return CreateErrorResponse (APIERR_BADPARS, "payload_b64 is missing");
    }
    GS::UniString compressionUs;
    parameters.Get ("compression", compressionUs);
    std::string compression = compressionUs.ToCStr ().Get ();

    std::vector<uint8_t> raw;
    std::string err;
    if (!DecodeEnvelope (payloadB64, compression, raw, err)) {
        return CreateErrorResponse (APIERR_BADPARS, GS::UniString (err.c_str ()));
    }

    std::string findStr, replaceStr;
    bool caseSensitive = true;
    bool dryRun = true;
    std::vector<std::string> elemGuids;
    bool scopeAll = true;

    try {
        auto oh = msgpack::unpack (reinterpret_cast<const char*> (raw.data ()), raw.size ());
        auto obj = oh.get ();
        auto m = obj.as<std::map<std::string, msgpack::object>> ();
        auto itF = m.find ("find");
        if (itF == m.end ()) {
            return CreateErrorResponse (APIERR_BADPARS, "payload must contain 'find'");
        }
        itF->second.convert (findStr);
        auto itR = m.find ("replace");
        if (itR != m.end ()) itR->second.convert (replaceStr);
        auto itC = m.find ("case_sensitive");
        if (itC != m.end ()) itC->second.convert (caseSensitive);
        auto itD = m.find ("dry_run");
        if (itD != m.end ()) itD->second.convert (dryRun);
        auto itE = m.find ("elements");
        if (itE != m.end ()) {
            itE->second.convert (elemGuids);
            scopeAll = false;
        }
    } catch (const std::exception& e) {
        const std::string msg = std::string ("msgpack decode failed: ") + e.what ();
        return CreateErrorResponse (APIERR_BADPARS, GS::UniString (msg.c_str ()));
    }

    if (findStr.empty ()) {
        return CreateErrorResponse (APIERR_BADPARS, "find string is empty");
    }

    GS::Array<API_Guid> targets;
    if (scopeAll) {
        CollectAllTextElementGuids (targets);
    } else {
        for (const std::string& s : elemGuids) {
            API_Guid g = APIGuidFromString (s.c_str ());
            if (g != APINULLGuid) targets.Push (g);
        }
    }

    // ASCII-aware поиск. Для не-ASCII регистр не меняется — такие строки
    // матчатся точно. case_sensitive=true — точное совпадение.
    std::string findLower = findStr;
    for (char& c : findLower) if (c >= 'A' && c <= 'Z') c += 32;
    auto findAt = [&](const std::string& hay, size_t from) -> size_t {
        if (caseSensitive) return hay.find (findStr, from);
        std::string h = hay;
        for (char& c : h) if (c >= 'A' && c <= 'Z') c += 32;
        return h.find (findLower, from);
    };
    auto replaceAll = [&](const std::string& src) -> std::string {
        std::string result;
        size_t pos = 0;
        while (true) {
            const size_t hit = findAt (src, pos);
            if (hit == std::string::npos) {
                result.append (src, pos, std::string::npos);
                break;
            }
            result.append (src, pos, hit - pos);
            result.append (replaceStr);
            pos = hit + findStr.size ();
        }
        return result;
    };

    struct Match { std::string elementId; std::string type; std::string before; std::string after; };
    std::vector<Match> matches;
    size_t scannedCount = 0, matchedCount = 0, replacedCount = 0;

    auto doWork = [&]() {
        for (const API_Guid& guid : targets) {
            API_Element element = {};
            element.header.guid = guid;
            if (ACAPI_Element_Get (&element) != NoError) continue;

            std::string typeStr;
            bool isLabel = false;
            if (GetElemTypeId (element.header) == API_TextID) {
                typeStr = "Text";
            } else if (GetElemTypeId (element.header) == API_LabelID &&
                       element.label.labelClass == APILblClass_Text) {
                typeStr = "Label";
                isLabel = true;
            } else {
                continue;
            }

            API_ElementMemo memo = {};
            if (ACAPI_Element_GetMemo (guid, &memo,
                    APIMemoMask_TextContent | APIMemoMask_Paragraph) != NoError) {
                ACAPI_DisposeElemMemoHdls (&memo);
                continue;
            }
            const GS::UniString currentUs = ReadTextFromMemo (memo);
            ACAPI_DisposeElemMemoHdls (&memo);
            ++scannedCount;

            const std::string current = currentUs.ToCStr ().Get ();
            if (findAt (current, 0) == std::string::npos) continue;

            const std::string next = replaceAll (current);
            ++matchedCount;
            Match mm;
            mm.elementId = APIGuidToString (guid).ToCStr ().Get ();
            mm.type = typeStr;
            mm.before = current;
            mm.after = next;
            matches.push_back (mm);

            if (dryRun) continue;

            const GS::UniString nextUs (next.c_str ());
            if (ApplyTextToElement (element, nextUs, isLabel) == NoError) {
                ++replacedCount;
            }
        }
    };

    // Undo-барьер только для РЕАЛЬНОЙ мутации. При dry_run модель не
    // меняется — пустая undo-запись в стеке пользователя была бы шумом
    // (Ctrl+Z → «ничего не произошло»).
    if (dryRun) {
        doWork ();
    } else {
        ACAPI_CallUndoableCommand ("BulkFindReplaceText", doWork);
    }

    msgpack::sbuffer outBuf;
    msgpack::packer<msgpack::sbuffer> pk (&outBuf);
    pk.pack_map (7);
    pk.pack (std::string ("find"));           pk.pack (findStr);
    pk.pack (std::string ("replace"));        pk.pack (replaceStr);
    pk.pack (std::string ("dry_run"));        pk.pack (dryRun);
    pk.pack (std::string ("scanned_count"));  pk.pack (static_cast<uint64_t> (scannedCount));
    pk.pack (std::string ("matched_count"));  pk.pack (static_cast<uint64_t> (matchedCount));
    pk.pack (std::string ("replaced_count")); pk.pack (static_cast<uint64_t> (replacedCount));
    pk.pack (std::string ("matches"));
    pk.pack_array (matches.size ());
    for (const Match& mm : matches) {
        pk.pack_map (4);
        pk.pack (std::string ("elementId")); pk.pack (mm.elementId);
        pk.pack (std::string ("type"));      pk.pack (mm.type);
        pk.pack (std::string ("before"));    pk.pack (mm.before);
        pk.pack (std::string ("after"));     pk.pack (mm.after);
    }

    std::string outCompression;
    const std::string outB64 =
        EncodeEnvelope (outBuf.data (), outBuf.size (), outCompression);

    GS::ObjectState response;
    response.Add ("payload_b64", GS::UniString (outB64.c_str ()));
    response.Add ("compression", GS::UniString (outCompression.c_str ()));
    response.Add ("scanned_count", static_cast<Int64> (scannedCount));
    response.Add ("matched_count", static_cast<Int64> (matchedCount));
    response.Add ("replaced_count", static_cast<Int64> (replacedCount));
    return response;
}
