#include "BulkCommands.hpp"
#include "MigrationHelper.hpp"
#include "ElementCreationCommands.hpp"

#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <cstdint>
#include <cstring>

// DevKit AC25/26 определяет `snprintf` как `_snprintf` (hack для старого
// MSVC). nlohmann внутри использует std::snprintf — препроцессор
// превращает это в std::_snprintf, которого нет в namespace std → C2039.
// На AC27+ этот макрос убран, поэтому там сборка проходит. Снимаем
// макрос перед подключением nlohmann — C++17 даёт std::snprintf из <cstdio>.
#ifdef snprintf
    #undef snprintf
#endif

#include <zstd.h>
#include <nlohmann/json.hpp>


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
        nlohmann::json j = nlohmann::json::from_msgpack (raw);
        if (!j.contains ("elements") || !j.contains ("properties")) {
            return CreateErrorResponse (APIERR_BADPARS,
                "payload must contain 'elements' and 'properties'");
        }
        for (const auto& s : j["elements"]) elemGuids.push_back (s.get<std::string> ());
        for (const auto& s : j["properties"]) propGuids.push_back (s.get<std::string> ());
    } catch (const std::exception& e) {
        const std::string msg = std::string ("msgpack decode failed: ") + e.what ();
        return CreateErrorResponse (APIERR_BADPARS, GS::UniString (msg.c_str ()));
    }

    const size_t N = elemGuids.size ();
    const size_t K = propGuids.size ();

    // ---- 2. Пройти по элементам × чанкам свойств ----
    // Структура: rows[i] — map elementId → list of (propertyId, value)
    nlohmann::ordered_json out;
    out["rows"] = nlohmann::json::array ();

    size_t valuesCount = 0;

    for (size_t i = 0; i < N; ++i) {
        nlohmann::ordered_json row;
        row["elementId"] = elemGuids[i];
        row["propertyValues"] = nlohmann::json::array ();

        API_Guid elemGuid = APIGuidFromString (elemGuids[i].c_str ());
        if (elemGuid == APINULLGuid) {
            // Пустой гуид — вернём K пустых записей
            for (size_t k = 0; k < K; ++k) {
                nlohmann::ordered_json pv;
                pv["propertyId"] = propGuids[k];
                pv["value"] = "";
                row["propertyValues"].push_back (pv);
            }
            out["rows"].push_back (row);
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
            nlohmann::ordered_json pv;
            pv["propertyId"] = propGuids[k];
            pv["value"] = values[k];
            row["propertyValues"].push_back (pv);
            if (!values[k].empty ()) ++valuesCount;
        }
        out["rows"].push_back (row);
    }

    // ---- 3. Сжать ответ и закодировать ----
    std::vector<uint8_t> outBytes = nlohmann::json::to_msgpack (out);
    std::string outCompression;
    const std::string outB64 =
        EncodeEnvelope (outBytes.data (), outBytes.size (), outCompression);

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
        nlohmann::json j = nlohmann::json::from_msgpack (raw);
        if (!j.contains ("elements")) {
            return CreateErrorResponse (APIERR_BADPARS,
                "payload must contain 'elements'");
        }
        for (const auto& s : j["elements"]) elemGuids.push_back (s.get<std::string> ());
    } catch (const std::exception& e) {
        const std::string msg = std::string ("msgpack decode failed: ") + e.what ();
        return CreateErrorResponse (APIERR_BADPARS, GS::UniString (msg.c_str ()));
    }

    nlohmann::ordered_json out;
    out["rows"] = nlohmann::json::array ();

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

        nlohmann::ordered_json row;
        row["elementId"] = guidStr;
        row["type"] = typeStr;
        row["text"] = textStr;
        out["rows"].push_back (row);
    }

    std::vector<uint8_t> outBytes = nlohmann::json::to_msgpack (out);
    std::string outCompression;
    const std::string outB64 =
        EncodeEnvelope (outBytes.data (), outBytes.size (), outCompression);

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
        nlohmann::json j = nlohmann::json::from_msgpack (raw);
        if (!j.contains ("rows")) {
            return CreateErrorResponse (APIERR_BADPARS, "payload must contain 'rows'");
        }
        for (const auto& item : j["rows"]) {
            if (!item.contains ("elementId") || !item.contains ("text")) continue;
            Row row;
            row.elementId = item["elementId"].get<std::string> ();
            row.text = item["text"].get<std::string> ();
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
    ACAPI_CallUndoableCommand ("BulkSetTexts", [&]() -> GSErrCode {
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
            else errors.push_back ({row.elementId,
                "change failed (code=" + std::to_string (static_cast<long long> (e)) + ")"});
        }
        return NoError;
    });

    nlohmann::ordered_json out;
    out["updated"] = static_cast<uint64_t> (updatedCount);
    out["total"] = static_cast<uint64_t> (rows.size ());
    out["errors"] = nlohmann::json::array ();
    for (const ErrEntry& ee : errors) {
        nlohmann::ordered_json e;
        e["elementId"] = ee.elementId;
        e["message"] = ee.message;
        out["errors"].push_back (e);
    }

    std::vector<uint8_t> outBytes = nlohmann::json::to_msgpack (out);
    std::string outCompression;
    const std::string outB64 =
        EncodeEnvelope (outBytes.data (), outBytes.size (), outCompression);

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
        nlohmann::json j = nlohmann::json::from_msgpack (raw);
        if (!j.contains ("find")) {
            return CreateErrorResponse (APIERR_BADPARS, "payload must contain 'find'");
        }
        findStr = j["find"].get<std::string> ();
        if (j.contains ("replace")) replaceStr = j["replace"].get<std::string> ();
        if (j.contains ("case_sensitive")) caseSensitive = j["case_sensitive"].get<bool> ();
        if (j.contains ("dry_run")) dryRun = j["dry_run"].get<bool> ();
        if (j.contains ("elements")) {
            for (const auto& s : j["elements"]) elemGuids.push_back (s.get<std::string> ());
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
        return NoError;
    };

    // Undo-барьер только для РЕАЛЬНОЙ мутации. При dry_run модель не
    // меняется — пустая undo-запись в стеке пользователя была бы шумом
    // (Ctrl+Z → «ничего не произошло»).
    if (dryRun) {
        doWork ();
    } else {
        ACAPI_CallUndoableCommand ("BulkFindReplaceText", doWork);
    }

    nlohmann::ordered_json out;
    out["find"] = findStr;
    out["replace"] = replaceStr;
    out["dry_run"] = dryRun;
    out["scanned_count"] = static_cast<uint64_t> (scannedCount);
    out["matched_count"] = static_cast<uint64_t> (matchedCount);
    out["replaced_count"] = static_cast<uint64_t> (replacedCount);
    out["matches"] = nlohmann::json::array ();
    for (const Match& mm : matches) {
        nlohmann::ordered_json m;
        m["elementId"] = mm.elementId;
        m["type"] = mm.type;
        m["before"] = mm.before;
        m["after"] = mm.after;
        out["matches"].push_back (m);
    }

    std::vector<uint8_t> outBytes = nlohmann::json::to_msgpack (out);
    std::string outCompression;
    const std::string outB64 =
        EncodeEnvelope (outBytes.data (), outBytes.size (), outCompression);

    GS::ObjectState response;
    response.Add ("payload_b64", GS::UniString (outB64.c_str ()));
    response.Add ("compression", GS::UniString (outCompression.c_str ()));
    response.Add ("scanned_count", static_cast<Int64> (scannedCount));
    response.Add ("matched_count", static_cast<Int64> (matchedCount));
    response.Add ("replaced_count", static_cast<Int64> (replacedCount));
    return response;
}
