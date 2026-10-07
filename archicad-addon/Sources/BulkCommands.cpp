#include "BulkCommands.hpp"
#include "MigrationHelper.hpp"
#include "ElementCreationCommands.hpp"
#include "PropertyConversionUtils.hpp"

// BulkGetElementMesh использует низкоуровневый 3D-component API
// (ACAPI_ModelAccess_Get3DInfo / ACAPI_ModelAccess_GetComponent).
// Он доступен на AC25+AC26 — alias'ы в MigrationHelper.hpp определены
// только при !ServerMainVers_2700. На AC27+ сигнатура API другая,
// там mesh временно не работает (см. AI_PRINCIPLES §4f).
//
// Раньше использовался ModelerAPI (SightPtr + EXPGetModel). На AC26
// он крешил (см. AI_PRINCIPLES §4f, 2026-10-07). Текущий путь не
// требует активного 3D-окна и работает по component-индексам.

#include <unordered_map>

#include <string>
#include <vector>
#include <map>
#include <set>
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

// Общий путь записи: TextLabelDetails::ApplyTextContent (тот же, что
// ModifyTexts / ModifyLabels / SetDetailsOfElements — проверен на AC26)
// + маски на top-level поля API_TextType + один ACAPI_Element_Change.
//
// Раньше использовали SetTextContentAndParagraphs — она пишет memo
// короче (без just, без runs), и на AC26 ACAPI_Element_Change отбивал
// её как APIERR_BADPARS (-2130313112). ApplyTextContent заполняет
// (*memo.paragraphs)[0].just и корректно строит runs.
//
// isLabel = true, если element.label.u.text; false — для element.text.
GSErrCode ApplyTextToElement (API_Element& element,
                              const GS::UniString& text,
                              bool isLabel)
{
    API_Element mask = {};
    ACAPI_ELEMENT_MASK_CLEAR (mask);
    API_ElementMemo clipMemo = {};

    // ApplyTextContent ждёт GS::ObjectState с 'text' (или 'runs').
    GS::ObjectState contentParams;
    contentParams.Add ("text", text);

    API_TextType* textPtr = isLabel ? &element.label.u.text : &element.text;

    const auto contentErr = TextLabelDetails::ApplyTextContent (clipMemo, *textPtr, contentParams);
    if (contentErr.HasValue ()) {
        ACAPI_DisposeElemMemoHdls (&clipMemo);
        return APIERR_BADPARS;
    }

    if (isLabel) {
        ACAPI_ELEMENT_MASK_SET (mask, API_LabelType, u.text.nLine);
        ACAPI_ELEMENT_MASK_SET (mask, API_LabelType, u.text.useEolPos);
#ifndef ServerMainVers_2800
        // Порядок как в upstream (ElementCommands.cpp SetDetailsOfElements):
        // charCode идёт сразу после useEolPos, ДО nonBreaking/width/height.
        ACAPI_ELEMENT_MASK_SET (mask, API_LabelType, u.text.charCode);
#endif
        ACAPI_ELEMENT_MASK_SET (mask, API_LabelType, u.text.nonBreaking);
        ACAPI_ELEMENT_MASK_SET (mask, API_LabelType, u.text.width);
        ACAPI_ELEMENT_MASK_SET (mask, API_LabelType, u.text.height);
    } else {
        ACAPI_ELEMENT_MASK_SET (mask, API_TextType, nLine);
        ACAPI_ELEMENT_MASK_SET (mask, API_TextType, useEolPos);
#ifndef ServerMainVers_2800
        ACAPI_ELEMENT_MASK_SET (mask, API_TextType, charCode);
#endif
        ACAPI_ELEMENT_MASK_SET (mask, API_TextType, nonBreaking);
        ACAPI_ELEMENT_MASK_SET (mask, API_TextType, width);
        ACAPI_ELEMENT_MASK_SET (mask, API_TextType, height);
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
// Переиспользует TextLabelDetails::ApplyTextContent (тот же путь,
// что CreateTexts / ModifyTexts / ModifyLabels) через ApplyTextToElement.

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
//  BulkGetElementDataCommand  (V2)
// ---------------------------------------------------------------------
//
// Комплексное чтение объекта: details + bbox + properties + GDL +
// classifications + connected (relations) + mesh. Всё за один Execute.
//
// Вход (msgpack):
//   { "elements":         ["guid", ...],
//     "properties":       ["prop-guid", ...],          // опц.
//     "gdl_names":        ["A", "B"] | "all",          // опц.
//     "classifications":  ["system-guid", ...] | "all",// опц.
//     "connected_types":  ["Door", "Window", ...],     // опц.
//     "with_bbox":        true,                        // default true
//     "with_mesh":        false,                       // default false
//     "apply_transform":  true }                       // default true
//
// Выход (msgpack):
//   { "entities": [
//       { "guid": "...", "element_type": "Object",
//         "parameters": { "story_index": 0, "layer_index": 679,
//                         "bbox_min_x": ..., "bbox_size_z": ...,
//                         "<prop-guid>": "<value>",
//                         "GDL/A": 2.0,
//                         "class/<system-guid>": "<item-guid>" },
//         "metadata": { "source": "archicad",
//                       "aspects_loaded": [...] },
//         "mesh": { // если with_mesh=true
//           "vertexCount": N, "triangleCount": M,
//           "vertices":  <binary float32 xyz>,
//           "triangles": <binary uint32 ijk> } }
//     ],
//     "relations": [
//       { "from_guid": "...", "to_guid": "...",
//         "kind": "connected_to", "via": "Door" } ] }

namespace {

const char* ElementTypeName (API_ElemTypeID t)
{
    switch (t) {
        case API_ObjectID:       return "Object";
        case API_WallID:         return "Wall";
        case API_SlabID:         return "Slab";
        case API_ZoneID:         return "Zone";
        case API_ColumnID:       return "Column";
        case API_BeamID:         return "Beam";
        case API_WindowID:       return "Window";
        case API_DoorID:         return "Door";
        case API_TextID:         return "Text";
        case API_LabelID:        return "Label";
        case API_MorphID:        return "Morph";
        case API_MeshID:         return "Mesh";
        case API_RoofID:         return "Roof";
        case API_ShellID:        return "Shell";
        case API_StairID:        return "Stair";
        case API_RailingID:      return "Railing";
        case API_CurtainWallID:  return "CurtainWall";
        case API_HatchID:        return "Hatch";
        case API_DrawingID:      return "Drawing";
        case API_CutPlaneID:     return "CutPlane";
        case API_LineID:         return "Line";
        case API_PolyLineID:     return "PolyLine";
        case API_ArcID:          return "Arc";
        case API_CircleID:       return "Circle";
        case API_SplineID:       return "Spline";
        case API_HotspotID:      return "Hotspot";
        case API_ChangeMarkerID: return "ChangeMarker";
        case API_DetailID:       return "Detail";
        case API_WorksheetID:    return "Worksheet";
        default:                 return "Unknown";
    }
}

// Обратный маппинг имени типа -> API_ElemTypeID.
// Только те, что поддерживает ACAPI_Grouping_GetConnectedElements на AC26.
bool StringToElemTypeID (const std::string& s, API_ElemTypeID& out)
{
    static const std::map<std::string, API_ElemTypeID> table = {
        {"Wall",         API_WallID},
        {"Column",       API_ColumnID},
        {"Beam",         API_BeamID},
        {"Slab",         API_SlabID},
        {"Roof",         API_RoofID},
        {"Shell",        API_ShellID},
        {"Mesh",         API_MeshID},
        {"Morph",        API_MorphID},
        {"Zone",         API_ZoneID},
        {"Door",         API_DoorID},
        {"Window",       API_WindowID},
        {"Object",       API_ObjectID},
        {"CurtainWall",  API_CurtainWallID},
        {"Stair",        API_StairID},
        {"Railing",      API_RailingID},
    };
    auto it = table.find (s);
    if (it == table.end ()) return false;
    out = it->second;
    return true;
}

// GDL value -> JSON. Best-effort.
// Реальные имена APIParT_* и полей API_AddParType — из рабочего
// ElementGDLParameterCommands.cpp (ConvertAddParIDToString + AddValue*):
//   • численные типы всех мастей лежат в p.value.real (даже Integer —
//     это double, приведение к int64 делается явно);
//   • строки — p.value.uStr (UTF-16), конвертируем через GS::UniString;
//   • APIParT_Ratio не существует в AC26;
//   • Linetype пишется как APIParT_LineTyp (не APIParT_Linetype).
nlohmann::ordered_json GdlValueToJson (const API_AddParType& p)
{
    switch (p.typeID) {
        case APIParT_Integer:
        case APIParT_Boolean:
        case APIParT_PenCol:
        case APIParT_LineTyp:
        case APIParT_Mater:
        case APIParT_FillPat:
        case APIParT_BuildingMaterial:
        case APIParT_Profile:
        case APIParT_LightSw:
            return static_cast<int64_t> (p.value.real);
        case APIParT_ColRGB:
        case APIParT_Intens:
        case APIParT_RealNum:
        case APIParT_Length:
        case APIParT_Angle:
            return p.value.real;
        case APIParT_CString:
        case APIParT_Title:
            return p.value.uStr != nullptr
                ? std::string (GS::UniString (p.value.uStr).ToCStr ().Get ())
                : std::string ();
        default:
            return nullptr;
    }
}

// ---- Element data collector ----
// Общий сборщик element-data, используется:
//   • BulkGetElementData::Execute (эта команда содержит свою КОПИЮ логики ниже —
//     схлопнуть её в вызов CollectElementData отдельным патчем, когда
//     with_data в BulkGetGroupMembers обкатается на живом AC26);
//   • BulkGetGroupMembers::Execute с with_data=true.
//
// Структура опций зеркалит набор параметров payload'а BulkGetElementData.
struct ElementDataOptions {
    std::vector<std::string>     elemGuids;
    std::vector<std::string>     propGuids;
    std::vector<std::string>     gdlNames;
    bool                         gdlAll = false;
    std::vector<std::string>     classSystemGuids;
    std::vector<API_ElemTypeID>  connectedTypes;
    bool                         readGdl = false;
    bool                         readClass = false;
    bool                         withBbox = true;
    bool                         withMesh = false;
    bool                         applyTransform = true;
    bool                         with2DGeometry = false;
    bool                         withGroupInfo = true;
    bool                         withGroupMembers = false;
};

// Заполняет out["geometry"] 2D-геометрией элемента (PolyLine / Line / Arc /
// Circle / Hatch / Label / Text / Hotspot). Имена ключей совпадают с теми,
// что отдаёт GetDetailsOfElements — единый контракт для per-element и bulk.
// Контуры (PolyLine/Hatch) читаются через существующий helper
// GetPolygonsFromMemoCoords из CommandBase.hpp — без дублирования разбора.
void Collect2DGeometryToJson (const API_Element& element, nlohmann::ordered_json& out)
{
    const API_ElemTypeID tid = GetElemTypeId (element.header);

    auto add2D = [] (nlohmann::ordered_json& j, const API_Coord& c) {
        nlohmann::ordered_json o;
        o["x"] = c.x;
        o["y"] = c.y;
        j.push_back (o);
    };

    switch (tid) {
        case API_PolyLineID: {
            auto polys = GetPolygonsFromMemoCoords (element.header.guid, false);
            if (polys.empty ()) return;
            auto& g = out["geometry"];
            g["type"] = "PolyLine";
            const auto& p = polys[0];
            for (const auto& c : p.coords) add2D (g["coordinates"], c);
            for (const auto& a : p.arcs) {
                nlohmann::ordered_json ao;
                ao["begIndex"] = a.begIndex;
                ao["endIndex"] = a.endIndex;
                ao["arcAngle"] = a.arcAngle;
                g["arcs"].push_back (ao);
            }
            g["room_separator"] = element.polyLine.roomSeparator;
            g["line_pen_index"] = element.polyLine.linePen.penIndex;
            g["line_type_id"]   = APIGuidToString (GetAttributeGuidFromIndex (API_LinetypeID, element.polyLine.ltypeInd)).ToCStr ().Get ();
            g["z_coordinate"]   = static_cast<int> (element.header.floorInd);
        } break;

        case API_HatchID: {
            auto polys = GetPolygonsFromMemoCoords (element.header.guid, false);
            if (polys.empty ()) return;
            auto& g = out["geometry"];
            g["type"] = "Hatch";
            const auto& outline = polys[0];
            for (const auto& c : outline.coords) add2D (g["coordinates"], c);
            for (const auto& a : outline.arcs) {
                nlohmann::ordered_json ao;
                ao["begIndex"] = a.begIndex;
                ao["endIndex"] = a.endIndex;
                ao["arcAngle"] = a.arcAngle;
                g["arcs"].push_back (ao);
            }
            for (size_t hi = 1; hi < polys.size (); ++hi) {
                nlohmann::ordered_json ho;
                for (const auto& c : polys[hi].coords) add2D (ho["coordinates"], c);
                for (const auto& a : polys[hi].arcs) {
                    nlohmann::ordered_json ao;
                    ao["begIndex"] = a.begIndex;
                    ao["endIndex"] = a.endIndex;
                    ao["arcAngle"] = a.arcAngle;
                    ho["arcs"].push_back (ao);
                }
                g["holes"].push_back (ho);
            }
            g["contour_pen_index"]         = element.hatch.contPen.penIndex;
            g["fill_pen_index"]            = element.hatch.fillPen.penIndex;
            g["fill_background_pen_index"] = element.hatch.fillBGPen;
            g["fill_id"]                   = APIGuidToString (GetAttributeGuidFromIndex (API_FilltypeID, element.hatch.fillInd)).ToCStr ().Get ();
            g["building_material_id"]      = APIGuidToString (GetAttributeGuidFromIndex (API_BuildingMaterialID, element.hatch.buildingMaterial)).ToCStr ().Get ();
            g["room_special"]              = element.hatch.roomSpecial;
            g["show_area"]                 = element.hatch.showArea != 0;
            g["z_coordinate"]              = static_cast<int> (element.header.floorInd);
        } break;

        case API_LineID: {
            auto& g = out["geometry"];
            g["type"] = "Line";
            nlohmann::ordered_json b, e;
            b["x"] = element.line.begC.x; b["y"] = element.line.begC.y;
            e["x"] = element.line.endC.x; e["y"] = element.line.endC.y;
            g["beg_coordinate"] = b;
            g["end_coordinate"] = e;
            g["room_separator"] = element.line.roomSeparator;
            g["line_pen_index"] = element.line.linePen.penIndex;
            g["line_type_id"]   = APIGuidToString (GetAttributeGuidFromIndex (API_LinetypeID, element.line.ltypeInd)).ToCStr ().Get ();
            g["z_coordinate"]   = static_cast<int> (element.header.floorInd);
        } break;

        case API_ArcID:
        case API_CircleID: {
            auto& g = out["geometry"];
            g["type"] = (tid == API_CircleID) ? "Circle" : "Arc";
            nlohmann::ordered_json o;
            o["x"] = element.arc.origC.x; o["y"] = element.arc.origC.y;
            g["origin"]         = o;
            g["radius"]         = element.arc.r;
            g["angle"]          = element.arc.angle;
            g["ratio"]          = element.arc.ratio;
            g["reflected"]      = element.arc.reflected;
            g["room_separator"] = element.arc.roomSeparator;
            g["line_pen_index"] = element.arc.linePen.penIndex;
            g["line_type_id"]   = APIGuidToString (GetAttributeGuidFromIndex (API_LinetypeID, element.arc.ltypeInd)).ToCStr ().Get ();
            g["z_coordinate"]   = static_cast<int> (element.header.floorInd);
            if (tid == API_ArcID) {
                g["beg_angle"] = element.arc.begAng;
                g["end_angle"] = element.arc.endAng;
            }
        } break;

        case API_LabelID: {
            auto& g = out["geometry"];
            g["type"] = "Label";
            g["label_class"] = (element.label.labelClass == APILblClass_Symbol) ? "Symbol" : "Text";
            if (element.label.parent != APINULLGuid) {
                g["owner_element_id"] = APIGuidToString (element.label.parent).ToCStr ().Get ();
            }
            nlohmann::ordered_json b, m, e;
            b["x"] = element.label.begC.x; b["y"] = element.label.begC.y;
            m["x"] = element.label.midC.x; m["y"] = element.label.midC.y;
            e["x"] = element.label.endC.x; e["y"] = element.label.endC.y;
            g["beg_coordinate"]  = b;
            g["mid_coordinate"]  = m;
            g["end_coordinate"]  = e;
            g["has_leader_line"] = element.label.hasLeaderLine;
            g["z_coordinate"]    = static_cast<int> (element.header.floorInd);
        } break;

        case API_TextID: {
            auto& g = out["geometry"];
            g["type"] = "Text";
            nlohmann::ordered_json p;
            p["x"] = element.text.loc.x; p["y"] = element.text.loc.y;
            g["position"] = p;
            g["angle"]    = element.text.angle;
            g["height"]   = element.text.size;
            g["pen"]      = static_cast<int> (element.text.pen);
            g["z_coordinate"] = static_cast<int> (element.header.floorInd);
        } break;

        case API_HotspotID: {
            auto& g = out["geometry"];
            g["type"] = "Hotspot";
            nlohmann::ordered_json p;
            p["x"] = element.hotspot.pos.x; p["y"] = element.hotspot.pos.y;
            g["position"] = p;
            g["z_coordinate"] = static_cast<int> (element.header.floorInd);
        } break;

        default:
            return;   // not a 2D element
    }
}

nlohmann::ordered_json CollectElementData (const ElementDataOptions& opts);

// ---- Forward declarations helper'ов, определённых ниже ----
// Эти функции живут в блоке BulkGetElementMeshCommand (ниже по файлу),
// но BulkGetElementData вызывает их до их определений. Объявляем заранее.
void ApplyTranmat (const API_Tranmat& t, double& x, double& y, double& z);
bool ExtractElementMesh (const API_Elem_Head& elemHead,
                         bool applyTransform,
                         std::vector<float>& outVertices,
                         std::vector<uint32_t>& outTriangles,
                         std::string& errOut);
std::vector<uint8_t> FloatsToBytes (const std::vector<float>& v);
std::vector<uint8_t> UIntsToBytes (const std::vector<uint32_t>& v);


// ---- CollectElementData ----
// Полная логика сбора данных для списка гуидов. Единая точка для
// BulkGetElementData (TODO: перевести на неё) и BulkGetGroupMembers с with_data.
// Возвращает { entities: [...], relations: [...] }.
nlohmann::ordered_json CollectElementData (const ElementDataOptions& opts)
{
    nlohmann::ordered_json out;
    out["entities"]  = nlohmann::json::array ();
    out["relations"] = nlohmann::json::array ();

    const size_t kPropertyChunkSize = 20;

    for (const std::string& guidStr : opts.elemGuids) {
        nlohmann::ordered_json entity;
        entity["guid"]         = guidStr;
        entity["element_type"] = "Unknown";
        entity["parameters"]   = nlohmann::json::object ();
        entity["metadata"]     = nlohmann::json::object ();
        entity["metadata"]["source"]          = "archicad";
        entity["metadata"]["aspects_loaded"]  = nlohmann::json::array ();

        API_Guid guid = APIGuidFromString (guidStr.c_str ());
        API_Element element = {};
        element.header.guid = guid;
        if (guid == APINULLGuid || ACAPI_Element_Get (&element) != NoError) {
            entity["metadata"]["error"] = "element not found";
            out["entities"].push_back (entity);
            continue;
        }

        entity["element_type"] = ElementTypeName (GetElemTypeId (element.header));

        auto& params = entity["parameters"];
        params["story_index"] = static_cast<int64_t> (element.header.floorInd);
        params["layer_index"] = static_cast<int64_t> (GetAttributeIndex (element.header.layer));

        if (GetElemTypeId (element.header) == API_ObjectID) {
            params["object_lib_part_index"] = static_cast<int64_t> (element.object.libInd);
            params["object_pos_x"]  = element.object.pos.x;
            params["object_pos_y"]  = element.object.pos.y;
            params["object_level"]  = element.object.level;
            params["object_angle"]  = element.object.angle;
            params["object_x_ratio"] = element.object.xRatio;
            params["object_y_ratio"] = element.object.yRatio;
#if TAPIR_AC26_ONLY
            API_LibPart lp = {};
            lp.index = element.object.libInd;
            if (ACAPI_LibPart_Get (&lp) == NoError) {
                params["object_lib_part_name"] =
                    GS::UniString (lp.docu_UName).ToCStr ().Get ();
            }
#endif
        }

        if (opts.withGroupInfo && element.header.groupGuid != APINULLGuid) {
            params["group_guid"] = APIGuidToString (element.header.groupGuid).ToCStr ().Get ();
        }
        entity["metadata"]["aspects_loaded"].push_back ("details");

        // ---- bbox ----
        if (opts.withBbox) {
            API_Box3D box3D = {};
            bool haveBox = false;
            const API_ElemTypeID bboxTypeID = GetElemTypeId (element.header);

            if (bboxTypeID == API_SlabID) {
                API_ElemInfo3D info3D = {};
                if (ACAPI_ModelAccess_Get3DInfo (element.header, &info3D) == NoError) {
                    double xMin = 1e30, yMin = 1e30, zMin = 1e30;
                    double xMax = -1e30, yMax = -1e30, zMax = -1e30;
                    bool found = false;
                    for (Int32 iBody = info3D.fbody; iBody <= info3D.lbody; ++iBody) {
                        API_Component3D bc = {};
                        bc.header.typeID = API_BodyID;
                        bc.header.index  = iBody;
                        if (ACAPI_ModelAccess_GetComponent (&bc) != NoError) continue;
                        if (bc.body.nPgon == 0) continue;
                        found = true;
                        if (bc.body.xmin < xMin) xMin = bc.body.xmin;
                        if (bc.body.xmax > xMax) xMax = bc.body.xmax;
                        if (bc.body.ymin < yMin) yMin = bc.body.ymin;
                        if (bc.body.ymax > yMax) yMax = bc.body.ymax;
                        if (bc.body.zmin < zMin) zMin = bc.body.zmin;
                        if (bc.body.zmax > zMax) zMax = bc.body.zmax;
                    }
                    if (found) {
                        box3D.xMin = xMin; box3D.yMin = yMin; box3D.zMin = zMin;
                        box3D.xMax = xMax; box3D.yMax = yMax; box3D.zMax = zMax;
                        haveBox = true;
                    }
                }
            } else {
                if (ACAPI_Element_CalcBounds (&element.header, &box3D) == NoError) {
                    haveBox = true;
                }
            }

            if (haveBox) {
                params["bbox_min_x"] = box3D.xMin;
                params["bbox_min_y"] = box3D.yMin;
                params["bbox_min_z"] = box3D.zMin;
                params["bbox_max_x"] = box3D.xMax;
                params["bbox_max_y"] = box3D.yMax;
                params["bbox_max_z"] = box3D.zMax;
                params["bbox_center_x"] = (box3D.xMin + box3D.xMax) * 0.5;
                params["bbox_center_y"] = (box3D.yMin + box3D.yMax) * 0.5;
                params["bbox_center_z"] = (box3D.zMin + box3D.zMax) * 0.5;
                params["bbox_size_x"] = box3D.xMax - box3D.xMin;
                params["bbox_size_y"] = box3D.yMax - box3D.yMin;
                params["bbox_size_z"] = box3D.zMax - box3D.zMin;
                entity["metadata"]["aspects_loaded"].push_back ("bbox");
            }
        }

        // ---- properties ----
        if (!opts.propGuids.empty ()) {
            bool anyProp = false;
            for (size_t start = 0; start < opts.propGuids.size (); start += kPropertyChunkSize) {
                const size_t end = std::min (start + kPropertyChunkSize, opts.propGuids.size ());
                GS::Array<API_Guid> chunkGuids;
                for (size_t k = start; k < end; ++k) {
                    API_Guid g = APIGuidFromString (opts.propGuids[k].c_str ());
                    if (g != APINULLGuid) chunkGuids.Push (g);
                }
                GS::Array<API_Property> fetched;
                if (ACAPI_Element_GetPropertyValuesByGuid (guid, chunkGuids, fetched) != NoError) continue;
                for (const API_Property& p : fetched) {
                    GS::UniString val;
                    if (ACAPI_Property_GetPropertyValueString (p, &val) == NoError) {
                        const std::string key = APIGuidToString (p.definition.guid).ToCStr ().Get ();
                        params[key] = val.ToCStr ().Get ();
                        anyProp = true;
                    }
                }
            }
            if (anyProp) entity["metadata"]["aspects_loaded"].push_back ("properties");
        }

        // ---- GDL ----
        if (opts.readGdl) {
            API_ElementMemo memo = {};
            if (ACAPI_Element_GetMemo (guid, &memo, APIMemoMask_AddPars) == NoError && memo.params != nullptr) {
                const GSSize nParams = BMGetHandleSize ((GSHandle) memo.params) / sizeof (API_AddParType);
                for (GSIndex ii = 0; ii < nParams; ++ii) {
                    const API_AddParType& p = (*memo.params)[ii];
                    const std::string name (p.name);
                    if (!opts.gdlAll) {
                        if (std::find (opts.gdlNames.begin (), opts.gdlNames.end (), name) == opts.gdlNames.end ()) continue;
                    }
                    nlohmann::ordered_json v = GdlValueToJson (p);
                    if (!v.is_null ()) params["GDL/" + name] = v;
                }
                entity["metadata"]["aspects_loaded"].push_back ("gdl");
            }
            ACAPI_DisposeElemMemoHdls (&memo);
        }

        // ---- classifications ----
        if (opts.readClass && !opts.classSystemGuids.empty ()) {
            bool anyClass = false;
            for (const std::string& sysStr : opts.classSystemGuids) {
                API_Guid sysGuid = APIGuidFromString (sysStr.c_str ());
                if (sysGuid == APINULLGuid) continue;
                API_ClassificationItem item = {};
                if (ACAPI_Element_GetClassificationInSystem (guid, sysGuid, item) == NoError
                        && item.guid != APINULLGuid) {
                    params["class/" + sysStr] = APIGuidToString (item.guid).ToCStr ().Get ();
                    anyClass = true;
                }
            }
            if (anyClass) entity["metadata"]["aspects_loaded"].push_back ("classifications");
        }

        // ---- connected ----
        if (!opts.connectedTypes.empty ()) {
            bool anyConn = false;
            const bool ownerIsZone = (GetElemTypeId (element.header) == API_ZoneID);
            for (API_ElemTypeID t : opts.connectedTypes) {
                GS::Array<API_Guid> connectedElements;

                if (ownerIsZone) {
                    // Zone: ACAPI_Grouping_GetConnectedElements не работает на AC26
                    // (проверено 2026-10-07: и Object, и Zone дают 0 рёбер).
                    // Используем ACAPI_Element_GetRelations + API_RoomRelation —
                    // та же логика, что в GetRelationsOfElementsCommand (ElementCommands.cpp).
                    API_RoomRelation relation = {};
                    API_ElemType other;
                    other.typeID = t;
                    if (ACAPI_Element_GetRelations (guid, other, &relation) == NoError) {
                        relation.elementsGroupedByType.Enumerate (
                            [&] (const API_ElemType& et, const GS::Array<API_Guid>& arr) {
                                if (et.typeID != t) return;
                                for (const API_Guid& g : arr) connectedElements.Push (g);
                            });
                    }
                    ACAPI_DisposeRoomRelationHdls (&relation);
                } else {
                    if (ACAPI_Grouping_GetConnectedElements (guid, t, &connectedElements) != NoError) continue;
                }

                for (const API_Guid& toGuid : connectedElements) {
                    nlohmann::ordered_json rel;
                    rel["from_guid"] = guidStr;
                    rel["to_guid"]   = APIGuidToString (toGuid).ToCStr ().Get ();

                    // kind по паре (owner_type, target_type).
                    std::string kind = "connected_to";
                    if (ownerIsZone) {
                        if      (t == API_ObjectID) kind = "zone_content";
                        else if (t == API_ZoneID)   kind = "zone_neighbour";
                        else                        kind = "zone_boundary";
                    }
                    rel["kind"] = kind;
                    rel["via"]  = ElementTypeName (t);
                    out["relations"].push_back (rel);
                    anyConn = true;
                }
            }
            if (anyConn) entity["metadata"]["aspects_loaded"].push_back ("connected");
        }

        // ---- 2D geometry ----
        if (opts.with2DGeometry) {
            Collect2DGeometryToJson (element, entity);
        }

        // ---- mesh (только AC26 — на других версиях graceful stub) ----
        if (opts.withMesh) {
            std::vector<float>    vertices;
            std::vector<uint32_t> triangles;
            std::string           meshErr;
            const bool ok = ExtractElementMesh (element.header, opts.applyTransform,
                                                vertices, triangles, meshErr);
            nlohmann::ordered_json mesh;
            mesh["vertexCount"]   = static_cast<uint64_t> (vertices.size () / 3);
            mesh["triangleCount"] = static_cast<uint64_t> (triangles.size () / 3);
            mesh["vertices"]      = nlohmann::json::binary (FloatsToBytes (vertices));
            mesh["triangles"]     = nlohmann::json::binary (UIntsToBytes (triangles));
            mesh["error"]         = ok ? nlohmann::json (nullptr) : nlohmann::json (meshErr);
            entity["mesh"] = mesh;
            if (ok) entity["metadata"]["aspects_loaded"].push_back ("mesh");
        }

        out["entities"].push_back (entity);
    }
    return out;
}

}  // namespace

BulkGetElementDataCommand::BulkGetElementDataCommand () :
    CommandBase (CommonSchema::Used)
{
}

GS::String BulkGetElementDataCommand::GetName () const
{
    return "BulkGetElementData";
}

GS::Optional<GS::UniString> BulkGetElementDataCommand::GetInputParametersSchema () const
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

GS::Optional<GS::UniString> BulkGetElementDataCommand::GetRawResponseSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "payload_b64":     { "type": "string" },
            "compression":     { "type": "string" },
            "elements_count":  { "type": "integer" },
            "entities_count":  { "type": "integer" }
        },
        "additionalProperties": false,
        "required": [ "payload_b64", "compression" ]
    })";
}

GS::ObjectState BulkGetElementDataCommand::Execute (
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
    std::vector<std::string> propGuids;
    std::vector<std::string> gdlNames;
    std::vector<std::string> classSystemGuids;
    std::vector<std::string> connectedTypeNames;
    bool readGdl = false;
    bool gdlAll = false;
    bool readClass = false;
    bool classAll = false;
    bool withBbox = true;
    bool withMesh = false;
    bool applyTransform = true;
    bool readSelection = false;   // selected=true: читать текущее выделение
    bool withGroupInfo = true;    // добавлять group_guid в params
    bool withGroupMembers = false; // добавлять groups[...] в ответ

    try {
        nlohmann::json j = nlohmann::json::from_msgpack (raw);

        // selected=true: элементы берём из выделения Archicad.
        // elements тогда не обязателен. Если оба переданы — selected
        // имеет приоритет (пользователь хочет «что сейчас выделено»).
        if (j.contains ("selected")) {
            readSelection = j["selected"].get<bool> ();
        }
        if (readSelection) {
            API_SelectionInfo selInfo = {};
            GS::Array<API_Neig> neigs;
            if (ACAPI_Selection_Get (&selInfo, &neigs, false, true) != NoError) {
                return CreateErrorResponse (APIERR_GENERAL, "ACAPI_Selection_Get failed");
            }
            for (const API_Neig& neig : neigs) {
                if (neig.guid != APINULLGuid) {
                    elemGuids.push_back (APIGuidToString (neig.guid).ToCStr ().Get ());
                }
            }
        } else if (j.contains ("elements")) {
            for (const auto& s : j["elements"]) elemGuids.push_back (s.get<std::string> ());
        } else {
            return CreateErrorResponse (APIERR_BADPARS,
                "payload must contain 'elements' or 'selected: true'");
        }

        if (j.contains ("with_group_info"))    withGroupInfo    = j["with_group_info"].get<bool> ();
        if (j.contains ("with_group_members")) withGroupMembers = j["with_group_members"].get<bool> ();

        if (j.contains ("properties")) {
            for (const auto& s : j["properties"]) propGuids.push_back (s.get<std::string> ());
        }
        if (j.contains ("gdl_names")) {
            readGdl = true;
            if (j["gdl_names"].is_string () && j["gdl_names"].get<std::string> () == "all") {
                gdlAll = true;
            } else if (j["gdl_names"].is_array ()) {
                for (const auto& s : j["gdl_names"]) gdlNames.push_back (s.get<std::string> ());
            }
        }
        if (j.contains ("classifications")) {
            readClass = true;
            if (j["classifications"].is_string () && j["classifications"].get<std::string> () == "all") {
                classAll = true;
            } else if (j["classifications"].is_array ()) {
                for (const auto& s : j["classifications"]) classSystemGuids.push_back (s.get<std::string> ());
            }
        }
        if (j.contains ("connected_types")) {
            for (const auto& s : j["connected_types"]) connectedTypeNames.push_back (s.get<std::string> ());
        }
        if (j.contains ("with_bbox"))       withBbox       = j["with_bbox"].get<bool> ();
        if (j.contains ("with_mesh"))       withMesh       = j["with_mesh"].get<bool> ();
        if (j.contains ("apply_transform")) applyTransform = j["apply_transform"].get<bool> ();
    } catch (const std::exception& e) {
        const std::string msg = std::string ("msgpack decode failed: ") + e.what ();
        return CreateErrorResponse (APIERR_BADPARS, GS::UniString (msg.c_str ()));
    }

    // Раскрываем classAll: собираем все системы классификации проекта.
    if (classAll) {
        GS::Array<API_ClassificationSystem> allSystems;
        if (ACAPI_Classification_GetClassificationSystems (allSystems) == NoError) {
            for (const API_ClassificationSystem& s : allSystems) {
                classSystemGuids.push_back (APIGuidToString (s.guid).ToCStr ().Get ());
            }
        }
    }

    // Раскрываем connected_types: строка -> API_ElemTypeID.
    std::vector<API_ElemTypeID> connectedTypes;
    for (const std::string& s : connectedTypeNames) {
        API_ElemTypeID t;
        if (StringToElemTypeID (s, t)) connectedTypes.push_back (t);
    }

    nlohmann::ordered_json out;
    out["entities"]  = nlohmann::json::array ();
    out["relations"] = nlohmann::json::array ();

    const size_t kPropertyChunkSize = 20;

    for (const std::string& guidStr : elemGuids) {
        nlohmann::ordered_json entity;
        entity["guid"] = guidStr;
        entity["element_type"] = "Unknown";
        entity["parameters"] = nlohmann::json::object ();
        entity["metadata"] = nlohmann::json::object ();
        entity["metadata"]["source"] = "archicad";
        entity["metadata"]["aspects_loaded"] = nlohmann::json::array ();

        API_Guid guid = APIGuidFromString (guidStr.c_str ());
        API_Element element = {};
        element.header.guid = guid;
        if (guid == APINULLGuid || ACAPI_Element_Get (&element) != NoError) {
            entity["metadata"]["error"] = "element not found";
            out["entities"].push_back (entity);
            continue;
        }

        entity["element_type"] = ElementTypeName (GetElemTypeId (element.header));

        auto& params = entity["parameters"];
        params["story_index"] = static_cast<int64_t> (element.header.floorInd);
        // API_AttributeIndex — typedef Int32 в AC25/26, но класс в AC27+;
        // GetAttributeIndex (MigrationHelper.hpp) даёт int в обоих случаях.
        params["layer_index"] = static_cast<int64_t> (GetAttributeIndex (element.header.layer));
        // ---- Object-specific fields ----
        // Для Object (самый частый в проекте) читаем всё нужное
        // для воссоздания в другом проекте (BulkCreateElementsFromData):
        //   libPart (index + имя) — что именно ставить
        //   pos/level/angle/ratios — куда и как
        if (GetElemTypeId (element.header) == API_ObjectID) {
            params["object_lib_part_index"] = static_cast<int64_t> (element.object.libInd);
            params["object_pos_x"]  = element.object.pos.x;
            params["object_pos_y"]  = element.object.pos.y;
            params["object_level"]  = element.object.level;
            params["object_angle"]  = element.object.angle;
            params["object_x_ratio"] = element.object.xRatio;
            params["object_y_ratio"] = element.object.yRatio;
            // Имя libPart — для переноса между проектами (индексы
            // разные, а имя стабильно).
            //
            // ⚠ AC26-only: ACAPI_LibPart_Get есть в DevKit 25/26; в AC27+
            // функция переименована (в MigrationHelper.hpp есть алиасы
            // ACAPI_LibraryPart_*, но пары для ACAPI_LibPart_Get нет).
            // На 27+ object_lib_part_name не читается — остаётся только
            // object_lib_part_index.
            // TODO(§10b): version-aware путь для 27+.
#if TAPIR_AC26_ONLY
            API_LibPart lp = {};
            lp.index = element.object.libInd;
            if (ACAPI_LibPart_Get (&lp) == NoError) {
                params["object_lib_part_name"] =
                    GS::UniString (lp.docu_UName).ToCStr ().Get ();
            }
#endif
        }

        // ---- group info ----
        // element.header.groupGuid — нативная группировка Archicad (Ctrl+G).
        // APINULLGuid если элемент не в группе.
        if (withGroupInfo && element.header.groupGuid != APINULLGuid) {
            params["group_guid"] = APIGuidToString (element.header.groupGuid).ToCStr ().Get ();
        }
        entity["metadata"]["aspects_loaded"].push_back ("details");

        // ---- bbox ----
        // Стратегия: для всех типов, КРОМЕ Slab, — ACAPI_Element_CalcBounds
        // (одна функция, работает для Object / Wall / Roof / Stair / Zone и др.).
        // Для Slab CalcBounds идёт через 2D draw environment и падает с SIGSEGV,
        // когда элемент не отрисован в текущем окне (#686, см. ElementCommands.cpp:4283).
        // Поэтому для Slab — обход 3D-компонент (как в AccumulateSolidBodyBounds).
        // Раньше мы применяли обход ко всем типам — у Object с GDL-геометрией
        // body.nPgon == 0, и bbox не писался вообще.
        if (withBbox) {
            API_Box3D box3D = {};
            bool haveBox = false;
            const API_ElemTypeID bboxTypeID = GetElemTypeId (element.header);

            if (bboxTypeID == API_SlabID) {
                API_ElemInfo3D info3D = {};
                if (ACAPI_ModelAccess_Get3DInfo (element.header, &info3D) == NoError) {
                    double xMin = 1e30, yMin = 1e30, zMin = 1e30;
                    double xMax = -1e30, yMax = -1e30, zMax = -1e30;
                    bool found = false;
                    for (Int32 iBody = info3D.fbody; iBody <= info3D.lbody; ++iBody) {
                        API_Component3D bc = {};
                        bc.header.typeID = API_BodyID;
                        bc.header.index  = iBody;
                        if (ACAPI_ModelAccess_GetComponent (&bc) != NoError) continue;
                        if (bc.body.nPgon == 0) continue;
                        found = true;
                        if (bc.body.xmin < xMin) xMin = bc.body.xmin;
                        if (bc.body.xmax > xMax) xMax = bc.body.xmax;
                        if (bc.body.ymin < yMin) yMin = bc.body.ymin;
                        if (bc.body.ymax > yMax) yMax = bc.body.ymax;
                        if (bc.body.zmin < zMin) zMin = bc.body.zmin;
                        if (bc.body.zmax > zMax) zMax = bc.body.zmax;
                    }
                    if (found) {
                        box3D.xMin = xMin; box3D.yMin = yMin; box3D.zMin = zMin;
                        box3D.xMax = xMax; box3D.yMax = yMax; box3D.zMax = zMax;
                        haveBox = true;
                    }
                }
            } else {
                if (ACAPI_Element_CalcBounds (&element.header, &box3D) == NoError) {
                    haveBox = true;
                }
            }

            if (haveBox) {
                params["bbox_min_x"] = box3D.xMin;
                params["bbox_min_y"] = box3D.yMin;
                params["bbox_min_z"] = box3D.zMin;
                params["bbox_max_x"] = box3D.xMax;
                params["bbox_max_y"] = box3D.yMax;
                params["bbox_max_z"] = box3D.zMax;
                params["bbox_center_x"] = (box3D.xMin + box3D.xMax) * 0.5;
                params["bbox_center_y"] = (box3D.yMin + box3D.yMax) * 0.5;
                params["bbox_center_z"] = (box3D.zMin + box3D.zMax) * 0.5;
                params["bbox_size_x"] = box3D.xMax - box3D.xMin;
                params["bbox_size_y"] = box3D.yMax - box3D.yMin;
                params["bbox_size_z"] = box3D.zMax - box3D.zMin;
                entity["metadata"]["aspects_loaded"].push_back ("bbox");
            }
        }

        // ---- properties ----
        if (!propGuids.empty ()) {
            bool anyProp = false;
            for (size_t start = 0; start < propGuids.size (); start += kPropertyChunkSize) {
                const size_t end = std::min (start + kPropertyChunkSize, propGuids.size ());
                GS::Array<API_Guid> chunkGuids;
                for (size_t k = start; k < end; ++k) {
                    API_Guid g = APIGuidFromString (propGuids[k].c_str ());
                    if (g != APINULLGuid) chunkGuids.Push (g);
                }
                GS::Array<API_Property> fetched;
                if (ACAPI_Element_GetPropertyValuesByGuid (guid, chunkGuids, fetched) != NoError) continue;
                for (const API_Property& p : fetched) {
                    GS::UniString val;
                    if (ACAPI_Property_GetPropertyValueString (p, &val) == NoError) {
                        const std::string key = APIGuidToString (p.definition.guid).ToCStr ().Get ();
                        params[key] = val.ToCStr ().Get ();
                        anyProp = true;
                    }
                }
            }
            if (anyProp) entity["metadata"]["aspects_loaded"].push_back ("properties");
        }

        // ---- GDL ----
        if (readGdl) {
            API_ElementMemo memo = {};
            if (ACAPI_Element_GetMemo (guid, &memo, APIMemoMask_AddPars) == NoError && memo.params != nullptr) {
                const GSSize nParams = BMGetHandleSize ((GSHandle) memo.params) / sizeof (API_AddParType);
                for (GSIndex ii = 0; ii < nParams; ++ii) {
                    const API_AddParType& p = (*memo.params)[ii];
                    // p.name — const char[32] (не GS::UniString): см. рабочий
                    // ElementGDLParameterCommands.cpp, где actParam.name просто
                    // пишется в ObjectState как есть.
                    const std::string name (p.name);
                    if (!gdlAll) {
                        if (std::find (gdlNames.begin (), gdlNames.end (), name) == gdlNames.end ()) continue;
                    }
                    nlohmann::ordered_json v = GdlValueToJson (p);
                    if (!v.is_null ()) params["GDL/" + name] = v;
                }
                entity["metadata"]["aspects_loaded"].push_back ("gdl");
            }
            ACAPI_DisposeElemMemoHdls (&memo);
        }

        // ---- classifications ----
        if (readClass && !classSystemGuids.empty ()) {
            bool anyClass = false;
            for (const std::string& sysStr : classSystemGuids) {
                API_Guid sysGuid = APIGuidFromString (sysStr.c_str ());
                if (sysGuid == APINULLGuid) continue;
                API_ClassificationItem item = {};
                if (ACAPI_Element_GetClassificationInSystem (guid, sysGuid, item) == NoError
                        && item.guid != APINULLGuid) {
                    params["class/" + sysStr] = APIGuidToString (item.guid).ToCStr ().Get ();
                    anyClass = true;
                }
            }
            if (anyClass) entity["metadata"]["aspects_loaded"].push_back ("classifications");
        }

        // ---- connected (relations) ----
        if (!connectedTypes.empty ()) {
            bool anyConn = false;
            const bool ownerIsZone = (GetElemTypeId (element.header) == API_ZoneID);
            for (API_ElemTypeID t : connectedTypes) {
                GS::Array<API_Guid> connectedElements;

                if (ownerIsZone) {
                    // Zone: Grouping не работает на AC26, см. комментарий в CollectElementData.
                    API_RoomRelation relation = {};
                    API_ElemType other;
                    other.typeID = t;
                    if (ACAPI_Element_GetRelations (guid, other, &relation) == NoError) {
                        relation.elementsGroupedByType.Enumerate (
                            [&] (const API_ElemType& et, const GS::Array<API_Guid>& arr) {
                                if (et.typeID != t) return;
                                for (const API_Guid& g : arr) connectedElements.Push (g);
                            });
                    }
                    ACAPI_DisposeRoomRelationHdls (&relation);
                } else {
                    if (ACAPI_Grouping_GetConnectedElements (guid, t, &connectedElements) != NoError) continue;
                }

                for (const API_Guid& toGuid : connectedElements) {
                    nlohmann::ordered_json rel;
                    rel["from_guid"] = guidStr;
                    rel["to_guid"]   = APIGuidToString (toGuid).ToCStr ().Get ();

                    std::string kind = "connected_to";
                    if (ownerIsZone) {
                        if      (t == API_ObjectID) kind = "zone_content";
                        else if (t == API_ZoneID)   kind = "zone_neighbour";
                        else                        kind = "zone_boundary";
                    }
                    rel["kind"] = kind;
                    rel["via"]  = ElementTypeName (t);
                    out["relations"].push_back (rel);
                    anyConn = true;
                }
            }
            if (anyConn) entity["metadata"]["aspects_loaded"].push_back ("connected");
        }

        // ---- mesh ----
        if (withMesh) {
            std::vector<float> vertices;
            std::vector<uint32_t> triangles;
            std::string meshErr;
            const bool ok = ExtractElementMesh (element.header, applyTransform,
                                                vertices, triangles, meshErr);
            nlohmann::ordered_json mesh;
            mesh["vertexCount"]   = static_cast<uint64_t> (vertices.size () / 3);
            mesh["triangleCount"] = static_cast<uint64_t> (triangles.size () / 3);
            mesh["vertices"]      = nlohmann::json::binary (FloatsToBytes (vertices));
            mesh["triangles"]     = nlohmann::json::binary (UIntsToBytes (triangles));
            mesh["error"]         = ok ? nlohmann::json (nullptr) : nlohmann::json (meshErr);
            entity["mesh"] = mesh;
            if (ok) entity["metadata"]["aspects_loaded"].push_back ("mesh");
        }

        out["entities"].push_back (entity);
    }

    std::vector<uint8_t> outBytes = nlohmann::json::to_msgpack (out);
    std::string outCompression;
    const std::string outB64 =
        EncodeEnvelope (outBytes.data (), outBytes.size (), outCompression);

    GS::ObjectState response;
    response.Add ("payload_b64", GS::UniString (outB64.c_str ()));
    response.Add ("compression", GS::UniString (outCompression.c_str ()));
    response.Add ("elements_count", static_cast<Int64> (elemGuids.size ()));
    response.Add ("entities_count", static_cast<Int64> (out["entities"].size ()));
    response.Add ("relations_count", static_cast<Int64> (out["relations"].size ()));
    return response;
}


// ---------------------------------------------------------------------
//  BulkGetElementMeshCommand
// ---------------------------------------------------------------------
//
// Вход (msgpack в payload_b64):
//   { "elements":       ["guid", ...],
//     "apply_transform": true }
//
// Выход (msgpack в payload_b64):
//   { "rows": [
//       { "elementId":     "guid",
//         "vertexCount":   N,
//         "triangleCount": M,
//         "vertices":      <binary float32 LE, xyz xyz ...>,
//         "triangles":     <binary uint32  LE, ijk ijk ...>,
//         "error":         null | "Get3DInfo failed" | "no solid body" },
//       ... ] }
//
// Внутри — обход 3D-компонент по образцу AccumulateSolidBodyBounds
// (ElementCommands.cpp:4127): ACAPI_ModelAccess_Get3DInfo даёт диапазон
// тел [fbody..lbody], ACAPI_ModelAccess_GetComponent(API_BodyID) —
// каждое тело. Тело ссылается на диапазоны вершин/рёбер/полигонов
// через body.fvert..lvert, body.fpedg..lpedg, body.fpgon..lpgon,
// каждый из которых читается отдельным GetComponent с нужным
// typeID (API_VertID / API_PedgID / API_PgonID). Триангуляция —
// fan (v0,vi,vi+1) для простых полигонов без дырок.

namespace {

// Применяет 4x3 трансформацию body.tranmat к точке (x,y,z).
// Индексация tmx[] — как в AddMorphBodyFromMemo (ExtendedElementCommands.cpp:3175).
void ApplyTranmat (const API_Tranmat& t, double& x, double& y, double& z)
{
    const double nx = t.tmx[0]*x + t.tmx[4]*y + t.tmx[8]*z  + t.tmx[3];
    const double ny = t.tmx[1]*x + t.tmx[5]*y + t.tmx[9]*z  + t.tmx[7];
    const double nz = t.tmx[2]*x + t.tmx[6]*y + t.tmx[10]*z + t.tmx[11];
    x = nx; y = ny; z = nz;
}

// Извлекает mesh одного элемента через низкоуровневый 3D-component API.
// Не требует активного 3D-окна; на AC26 он не крешится (в отличие от
// прежнего пути через SightPtr + EXPGetModel).
//
// Схема обхода (APIdefs_3D.h, AC26):
//   ACAPI_ModelAccess_Get3DInfo(elemHead, &info3D) → диапазон тел
//                                                     [fbody..lbody]
//   Для каждого тела iBody:
//     GetComponent(API_BodyID, iBody)      → nVert, nPgon, nPedg, nEdge, tranmat
//     GetComponent(API_VertID, 1..nVert)   → (x, y, z) в локальных координатах
//     GetComponent(API_PgonID, 1..nPgon)   → fpedg, lpedg, status
//     GetComponent(API_PedgID, fpedg..lpedg)→ pedg (знак = направление)
//     GetComponent(API_EdgeID, abs(pedg))  → vert1, vert2
//   Триангуляция: fan по контуру полигона.
//
// Все индексы компонент у ACAPI 1-based (body, vert, pgon, pedg, edge),
// в outVertices/outTriangles — 0-based (стандарт для GL/VTK/ifcopenshell).
//
// applyTransform=true: к координатам вершин применяется body.tranmat
// (ACAPI-вершины идут в локальных координатах, а не в world — см.
// ElementCommands.cpp:4148 про #563, где bbox уже world, а вертексы — нет).
#if !defined (ServerMainVers_2700)
bool ExtractElementMesh (const API_Elem_Head& elemHead,
                         bool applyTransform,
                         std::vector<float>& outVertices,
                         std::vector<uint32_t>& outTriangles,
                         std::string& errOut)
{
    outVertices.clear ();
    outTriangles.clear ();

    API_ElemInfo3D info3D = {};
    const GSErrCode infoErr = ACAPI_ModelAccess_Get3DInfo (elemHead, &info3D);
    if (infoErr != NoError) {
        errOut = "Get3DInfo failed (code=" +
                 std::to_string (static_cast<long long> (infoErr)) + ")";
        return false;
    }

    if (info3D.fbody <= 0 || info3D.lbody < info3D.fbody) {
        errOut = "no solid bodies in element";
        return false;
    }

    for (Int32 iBody = info3D.fbody; iBody <= info3D.lbody; ++iBody) {
        API_Component3D bodyComp = {};
        bodyComp.header.typeID = API_BodyID;
        bodyComp.header.index  = iBody;
        if (ACAPI_ModelAccess_GetComponent (&bodyComp) != NoError) continue;
        if (bodyComp.body.nPgon == 0 || bodyComp.body.nVert == 0) continue;

        const Int32 nVert = bodyComp.body.nVert;
        const Int32 nPgon = bodyComp.body.nPgon;
        const API_Tranmat bodyTran = bodyComp.body.tranmat;

        // 1) Вершины. Собираем все локальные координаты этого тела в буфер
        //    localVerts[localIdx-1] = world (x,y,z) (после возможной tranmat).
        //    Индексация — 1-based, копируем в outVertices с базовым сдвигом.
        const uint32_t bodyBaseIdx = static_cast<uint32_t> (outVertices.size () / 3);
        std::vector<uint32_t> localToGlobal (static_cast<size_t> (nVert) + 1, 0);
        for (Int32 iVert = 1; iVert <= nVert; ++iVert) {
            API_Component3D vc = {};
            vc.header.typeID = API_VertID;
            vc.header.index  = iVert;
            if (ACAPI_ModelAccess_GetComponent (&vc) != NoError) continue;
            double x = vc.vert.x;
            double y = vc.vert.y;
            double z = vc.vert.z;
            if (applyTransform) {
                ApplyTranmat (bodyTran, x, y, z);
            }
            const uint32_t globalIdx = static_cast<uint32_t> (outVertices.size () / 3);
            localToGlobal[static_cast<size_t> (iVert)] = globalIdx;
            outVertices.push_back (static_cast<float> (x));
            outVertices.push_back (static_cast<float> (y));
            outVertices.push_back (static_cast<float> (z));
        }

        // 2) Полигоны 1..nPgon.
        for (Int32 iPgon = 1; iPgon <= nPgon; ++iPgon) {
            API_Component3D pc = {};
            pc.header.typeID = API_PgonID;
            pc.header.index  = iPgon;
            if (ACAPI_ModelAccess_GetComponent (&pc) != NoError) continue;
            if (pc.pgon.status & APIPgon_Invis) continue;

            // Контур: рёбра fpedg..lpedg. Каждая pedg ссылается на edge
            // (знаковый index: <0 = обратное направление). pedg == 0 —
            // начало «дырки»; в v1 просто обрываем контур.
            std::vector<uint32_t> polyIdx;
            polyIdx.reserve (8);
            for (Int32 iPedge = pc.pgon.fpedg; iPedge <= pc.pgon.lpedg; ++iPedge) {
                API_Component3D ec = {};
                ec.header.typeID = API_PedgID;
                ec.header.index  = iPedge;
                if (ACAPI_ModelAccess_GetComponent (&ec) != NoError) continue;
                const Int32 pedgSigned = ec.pedg.pedg;
                if (pedgSigned == 0) break;
                const Int32 edgeIdx = (pedgSigned < 0) ? -pedgSigned : pedgSigned;
                const bool reverse = (pedgSigned < 0);

                API_Component3D edgeComp = {};
                edgeComp.header.typeID = API_EdgeID;
                edgeComp.header.index  = edgeIdx;
                if (ACAPI_ModelAccess_GetComponent (&edgeComp) != NoError) continue;

                Int32 v1 = edgeComp.edge.vert1;
                Int32 v2 = edgeComp.edge.vert2;
                if (reverse) { const Int32 t = v1; v1 = v2; v2 = t; }

                if (v1 <= 0 || v1 > nVert || v2 <= 0 || v2 > nVert) continue;
                const uint32_t gv1 = localToGlobal[static_cast<size_t> (v1)];
                const uint32_t gv2 = localToGlobal[static_cast<size_t> (v2)];
                if (gv1 == 0 || gv2 == 0) continue;

                if (polyIdx.empty ()) polyIdx.push_back (gv1);
                if (polyIdx.back () != gv2) polyIdx.push_back (gv2);
            }

            // Замкнуть контур: если последняя != первой, добавить первую.
            if (polyIdx.size () >= 3 && polyIdx.front () != polyIdx.back ()) {
                polyIdx.push_back (polyIdx.front ());
            }
            if (polyIdx.size () < 4) continue;  // <3 уникальных вершин

            // Fan-триангуляция: (v0, vi, vi+1), i=1..n-2.
            for (size_t k = 1; k + 1 < polyIdx.size (); ++k) {
                outTriangles.push_back (polyIdx[0]);
                outTriangles.push_back (polyIdx[k]);
                outTriangles.push_back (polyIdx[k + 1]);
            }
        }
    }

    if (outVertices.empty () || outTriangles.empty ()) {
        errOut = "empty mesh (no triangles collected)";
        return false;
    }
    return true;
}
#else
bool ExtractElementMesh (const API_Elem_Head& elemHead,
                         bool /*applyTransform*/,
                         std::vector<float>& outVertices,
                         std::vector<uint32_t>& outTriangles,
                         std::string& errOut)
{
    outVertices.clear ();
    outTriangles.clear ();
    // На AC27+ alias'ы ACAPI_ModelAccess_Get3DInfo / GetComponent
    // в MigrationHelper.hpp отсутствуют. Возвращаем graceful stub.
    (void) elemHead;
    errOut = "mesh not supported on this Archicad version (only AC25/AC26 for now)";
    return false;
}
#endif

// Плоский массив float32/uint32 -> байтовый вектор little-endian.
std::vector<uint8_t> FloatsToBytes (const std::vector<float>& v)
{
    std::vector<uint8_t> out (v.size () * sizeof (float));
    if (!v.empty ()) std::memcpy (out.data (), v.data (), out.size ());
    return out;
}
std::vector<uint8_t> UIntsToBytes (const std::vector<uint32_t>& v)
{
    std::vector<uint8_t> out (v.size () * sizeof (uint32_t));
    if (!v.empty ()) std::memcpy (out.data (), v.data (), out.size ());
    return out;
}

}  // namespace

BulkGetElementMeshCommand::BulkGetElementMeshCommand () :
    CommandBase (CommonSchema::Used)
{
}

GS::String BulkGetElementMeshCommand::GetName () const
{
    return "BulkGetElementMesh";
}

GS::Optional<GS::UniString> BulkGetElementMeshCommand::GetInputParametersSchema () const
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

GS::Optional<GS::UniString> BulkGetElementMeshCommand::GetRawResponseSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "payload_b64": { "type": "string" },
            "compression": { "type": "string" },
            "elements_count": { "type": "integer" },
            "with_mesh_count": { "type": "integer" },
            "total_triangles": { "type": "integer" }
        },
        "additionalProperties": false,
        "required": [ "payload_b64", "compression" ]
    })";
}

GS::ObjectState BulkGetElementMeshCommand::Execute (
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
    bool applyTransform = true;
    try {
        nlohmann::json j = nlohmann::json::from_msgpack (raw);
        if (!j.contains ("elements")) {
            return CreateErrorResponse (APIERR_BADPARS,
                "payload must contain 'elements'");
        }
        for (const auto& s : j["elements"]) elemGuids.push_back (s.get<std::string> ());
        if (j.contains ("apply_transform")) applyTransform = j["apply_transform"].get<bool> ();
    } catch (const std::exception& e) {
        const std::string msg = std::string ("msgpack decode failed: ") + e.what ();
        return CreateErrorResponse (APIERR_BADPARS, GS::UniString (msg.c_str ()));
    }

    nlohmann::ordered_json out;
    out["rows"] = nlohmann::json::array ();
    size_t withMeshCount = 0;
    size_t totalTriangles = 0;

    for (const std::string& guidStr : elemGuids) {
        nlohmann::ordered_json row;
        row["elementId"] = guidStr;

        API_Guid guid = APIGuidFromString (guidStr.c_str ());
        if (guid == APINULLGuid) {
            row["vertexCount"] = 0;
            row["triangleCount"] = 0;
            row["vertices"] = nlohmann::json::binary (std::vector<uint8_t> {});
            row["triangles"] = nlohmann::json::binary (std::vector<uint8_t> {});
            row["error"] = "invalid guid";
            out["rows"].push_back (row);
            continue;
        }

        API_Element element = {};
        element.header.guid = guid;
        if (ACAPI_Element_Get (&element) != NoError) {
            row["vertexCount"] = 0;
            row["triangleCount"] = 0;
            row["vertices"] = nlohmann::json::binary (std::vector<uint8_t> {});
            row["triangles"] = nlohmann::json::binary (std::vector<uint8_t> {});
            row["error"] = "element not found";
            out["rows"].push_back (row);
            continue;
        }

        std::vector<float> vertices;
        std::vector<uint32_t> triangles;
        std::string meshErr;
        const bool ok = ExtractElementMesh (element.header, applyTransform,
                                            vertices, triangles, meshErr);

        if (ok) {
            ++withMeshCount;
            totalTriangles += triangles.size () / 3;
        }

        row["vertexCount"]   = static_cast<uint64_t> (vertices.size () / 3);
        row["triangleCount"] = static_cast<uint64_t> (triangles.size () / 3);
        row["vertices"]      = nlohmann::json::binary (FloatsToBytes (vertices));
        row["triangles"]     = nlohmann::json::binary (UIntsToBytes (triangles));
        row["error"]         = ok ? nlohmann::json (nullptr) : nlohmann::json (meshErr);
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
    response.Add ("with_mesh_count", static_cast<Int64> (withMeshCount));
    response.Add ("total_triangles", static_cast<Int64> (totalTriangles));
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


// ---------------------------------------------------------------------
//  BulkCloneElementCommand
// ---------------------------------------------------------------------
//
// Универсальный bulk-клон с одного донора. Не копирование выделенного
// (для этого есть RotateElementsByAngle с withCopy), а «донор + N позиций».
//
// Зачем: клиент прочитал элемент-эталон через BulkGetElementData
// (selected=true), выбрал донора, и хочет расставить N экземпляров с
// заданными позициями/углами/параметрами. Не надо знать libPart, GDL —
// всё наследуется от донора.
//
// v1: только Object (API_ObjectID). Wall/Slab/Door/Window — TODO (для
// них посложнее: у Wall — endC + толщина + профиль, у Slab — контур).
//
// Вход (msgpack в payload_b64):
//   { "source_guid": "<guid донора>",
//     "instances": [
//       { "pos_x":    361.7,          // обязательно (для Object)
//         "pos_y":   -8.8,
//         "level":    0.0,           // опц., default = level донора
//         "angle":    0.0,           // опц., рад
//         "story_index":  0,         // опц.
//         "layer_index":  853,       // опц.
//         "params_override": {"A": 2.0, "B": 4.0}  // опц.: GDL по имени
//       }, ... ],
//     "delete_source": false }
//
// Выход (msgpack):
//   { "created_guids": ["...", ...],
//     "errors": [{"index": 0, "msg": "..."}, ...] }

BulkCloneElementCommand::BulkCloneElementCommand () :
    CommandBase (CommonSchema::Used)
{
}

GS::String BulkCloneElementCommand::GetName () const
{
    return "BulkCloneElement";
}

GS::Optional<GS::UniString> BulkCloneElementCommand::GetInputParametersSchema () const
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

GS::Optional<GS::UniString> BulkCloneElementCommand::GetRawResponseSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "payload_b64": { "type": "string" },
            "compression": { "type": "string" },
            "created_count": { "type": "integer" },
            "errors_count":  { "type": "integer" }
        },
        "additionalProperties": false,
        "required": [ "payload_b64", "compression" ]
    })";
}

// Помощник: применить override из JSON к GDL-параметру p (по имени).
// Возвращает true, если параметр найден и обновлён. Не пересоздаёт
// значение с нуля: меняет только value.real или value.uStr — тип
// сохраняется (Integer остаётся Integer, Length — Length и т.д.).
static bool ApplyGdlOverride (API_AddParType& p, const nlohmann::json& v)
{
    switch (p.typeID) {
        case APIParT_Integer:
        case APIParT_Boolean:
        case APIParT_PenCol:
        case APIParT_LineTyp:
        case APIParT_Mater:
        case APIParT_FillPat:
        case APIParT_BuildingMaterial:
        case APIParT_Profile:
        case APIParT_LightSw:
            if (v.is_number_integer () || v.is_number_float ()) {
                p.value.real = v.get<double> ();
                return true;
            }
            if (v.is_boolean ()) {
                p.value.real = v.get<bool> () ? 1.0 : 0.0;
                return true;
            }
            return false;
        case APIParT_RealNum:
        case APIParT_Length:
        case APIParT_Angle:
        case APIParT_Intens:
            if (v.is_number ()) {
                p.value.real = v.get<double> ();
                return true;
            }
            return false;
        case APIParT_CString: {
            if (!v.is_string ()) return false;
            const std::string s = v.get<std::string> ();
            const GS::UniString u (s.c_str ());
            // value.uStr — UTF-16 буфер фиксированной длины.
            // Копируем, не пересоздавая handle: BMhAllClear уже сделан
            // при первом получении memo.
            if (p.value.uStr != nullptr) {
                GS::ucscpy (p.value.uStr, u.ToUStr ());
                return true;
            }
            return false;
        }
        default:
            return false;
    }
}

GS::ObjectState BulkCloneElementCommand::Execute (
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

    // v2: payload принимает либо один донор (source_guid + instances),
    // либо МАССИВ доноров: sources: [{source_guid, instances}, ...].
    // Каждый донор имеет свои instances — можно и разные объекты размножить,
    // и задать каждому свои позиции за один Execute.
    struct SourceTask {
        std::string sourceGuid;
        std::vector<nlohmann::ordered_json> instances;
    };
    std::vector<SourceTask> sources;
    bool deleteSource = false;

    try {
        nlohmann::json j = nlohmann::json::from_msgpack (raw);
        if (j.contains ("sources")) {
            for (const auto& s : j["sources"]) {
                if (!s.contains ("source_guid") || !s.contains ("instances")) {
                    return CreateErrorResponse (APIERR_BADPARS,
                        "'sources[i]' must contain 'source_guid' and 'instances'");
                }
                SourceTask t;
                t.sourceGuid = s["source_guid"].get<std::string> ();
                for (const auto& inst : s["instances"]) {
                    t.instances.push_back (inst);
                }
                sources.push_back (t);
            }
        } else if (j.contains ("source_guid") && j.contains ("instances")) {
            SourceTask t;
            t.sourceGuid = j["source_guid"].get<std::string> ();
            for (const auto& inst : j["instances"]) {
                t.instances.push_back (inst);
            }
            sources.push_back (t);
        } else {
            return CreateErrorResponse (APIERR_BADPARS,
                "payload must contain either 'sources' array, or 'source_guid' + 'instances'");
        }
        if (j.contains ("delete_source")) deleteSource = j["delete_source"].get<bool> ();
    } catch (const std::exception& e) {
        const std::string msg = std::string ("msgpack decode failed: ") + e.what ();
        return CreateErrorResponse (APIERR_BADPARS, GS::UniString (msg.c_str ()));
    }

    nlohmann::ordered_json out;
    out["per_source"]    = nlohmann::json::array ();
    out["created_guids"] = nlohmann::json::array ();
    out["errors"]        = nlohmann::json::array ();

    // Один undo на ВСЕ доноры и все их копии сразу.
    ACAPI_CallUndoableCommand ("BulkCloneElement", [&] () -> GSErrCode {
        constexpr UInt64 kAllMemoMask = APIMemoMask_All;
        for (const SourceTask& task : sources) {
            nlohmann::ordered_json srcOut;
            srcOut["source_guid"]   = task.sourceGuid;
            srcOut["created_guids"] = nlohmann::json::array ();
            srcOut["errors"]        = nlohmann::json::array ();

            API_Guid srcGuid = APIGuidFromString (task.sourceGuid.c_str ());
            if (srcGuid == APINULLGuid) {
                nlohmann::ordered_json e; e["msg"] = "source_guid is invalid";
                srcOut["errors"].push_back (e);
                out["per_source"].push_back (srcOut);
                continue;
            }
            API_Element srcElem = {};
            srcElem.header.guid = srcGuid;
            if (ACAPI_Element_Get (&srcElem) != NoError) {
                nlohmann::ordered_json e; e["msg"] = "source element not found";
                srcOut["errors"].push_back (e);
                out["per_source"].push_back (srcOut);
                continue;
            }
            const API_ElemTypeID srcType = GetElemTypeId (srcElem.header);
            if (srcType != API_ObjectID) {
                nlohmann::ordered_json e; e["msg"] = "BulkCloneElement supports only Object (source is not an Object)";
                srcOut["errors"].push_back (e);
                out["per_source"].push_back (srcOut);
                continue;
            }

            for (size_t i = 0; i < task.instances.size (); ++i) {
                const auto& inst = task.instances[i];
                API_Element el = srcElem;
                el.header.guid = APINULLGuid;
                el.header.modiStamp = 0;
                el.header.groupGuid = APINULLGuid;

                if (inst.contains ("story_index"))   el.header.floorInd = static_cast<short> (inst["story_index"].get<int> ());
                if (inst.contains ("layer_index"))
                    el.header.layer = ACAPI_CreateAttributeIndex (inst["layer_index"].get<Int32> ());
                if (inst.contains ("pos_x"))  el.object.pos.x = inst["pos_x"].get<double> ();
                if (inst.contains ("pos_y"))  el.object.pos.y = inst["pos_y"].get<double> ();
                if (inst.contains ("level"))  el.object.level = inst["level"].get<double> ();
                if (inst.contains ("angle"))  el.object.angle = inst["angle"].get<double> ();

                API_ElementMemo memo = {};
                if (ACAPI_Element_GetMemo (srcGuid, &memo, kAllMemoMask) != NoError) {
                    nlohmann::ordered_json e;
                    e["index"] = static_cast<int64_t> (i);
                    e["msg"] = "failed to get memo from source";
                    srcOut["errors"].push_back (e);
                    out["errors"].push_back (e);
                    ACAPI_DisposeElemMemoHdls (&memo);
                    continue;
                }

                if (inst.contains ("params_override") && memo.params != nullptr) {
                    const auto& ovr = inst["params_override"];
                    const GSSize nParams = BMGetHandleSize ((GSHandle) memo.params) / sizeof (API_AddParType);
                    for (GSIndex k = 0; k < nParams; ++k) {
                        API_AddParType& p = (*memo.params)[k];
                        const std::string pname (p.name);
                        if (ovr.contains (pname)) {
                            ApplyGdlOverride (p, ovr[pname]);
                        }
                    }
                }

                GSErrCode createErr = ACAPI_Element_Create (&el, &memo);
                if (createErr == NoError) {
                    const std::string g = APIGuidToString (el.header.guid).ToCStr ().Get ();
                    srcOut["created_guids"].push_back (g);
                    out["created_guids"].push_back (g);
                } else {
                    nlohmann::ordered_json e;
                    e["index"] = static_cast<int64_t> (i);
                    e["msg"]   = "ACAPI_Element_Create failed";
                    e["code"]  = static_cast<int64_t> (createErr);
                    srcOut["errors"].push_back (e);
                    out["errors"].push_back (e);
                }
                ACAPI_DisposeElemMemoHdls (&memo);
            }

            out["per_source"].push_back (srcOut);
        }

        if (deleteSource) {
            GS::Array<API_Guid> toDelete;
            for (const SourceTask& t : sources) {
                API_Guid g = APIGuidFromString (t.sourceGuid.c_str ());
                if (g != APINULLGuid) toDelete.Push (g);
            }
            if (!toDelete.IsEmpty ()) {
                ACAPI_Element_Delete (toDelete);
            }
        }
        return NoError;
    });

    std::vector<uint8_t> outBytes = nlohmann::json::to_msgpack (out);
    std::string outCompression;
    const std::string outB64 =
        EncodeEnvelope (outBytes.data (), outBytes.size (), outCompression);

    GS::ObjectState response;
    response.Add ("payload_b64", GS::UniString (outB64.c_str ()));
    response.Add ("compression", GS::UniString (outCompression.c_str ()));
    response.Add ("created_count", static_cast<Int64> (out["created_guids"].size ()));
    response.Add ("errors_count",  static_cast<Int64> (out["errors"].size ()));
    return response;
}


// ---------------------------------------------------------------------
//  BulkGetGroupMembersCommand
// ---------------------------------------------------------------------
//
// Работа с нативными группами Archicad (Ctrl+G).
//
// Вход (msgpack в payload_b64):
//   { "element_guids": ["guid", ...],   // опц.: для этих элементов найти
//                                         //       родительскую группу
//     "group_guids":   ["guid", ...],   // опц.: для этих групп развернуть
//                                         //       членов
//     "recursive":     true }            // default true: включать вложенные
//                                         //       подгруппы
//
// Хотя бы одно из element_guids / group_guids должно быть непустым.
//
// Выход (msgpack в payload_b64):
//   { "groups": [
//       { "source_guid":   "<элемент или группа из запроса>",
//         "source_kind":   "element" | "group",
//         "group_guid":    "<guid группы>" | null,
//         "member_guids":  ["...", ...]   // для source_kind=group
//       }, ...
//     ] }
//
// Зачем: клиент по одному элементу (клик по прибору) получает всю его
// группу (обвязка, стояк). Или по уже известному group_guid разворачивает
// членов без повторного обхода.

BulkGetGroupMembersCommand::BulkGetGroupMembersCommand () :
    CommandBase (CommonSchema::Used)
{
}

GS::String BulkGetGroupMembersCommand::GetName () const
{
    return "BulkGetGroupMembers";
}

GS::Optional<GS::UniString> BulkGetGroupMembersCommand::GetInputParametersSchema () const
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

// Payload (msgpack в payload_b64):
//   { "element_guids": [...],           // опц.: для каждого найти его группу
//     "group_guids":   [...],           // опц.: для каждой группы развернуть членов
//     "recursive":     true,            // default true — включать подгруппы
//     "with_data":     false,           // default false — только гуиды. true — полные entity
//                                       //   через CollectElementData
//     // при with_data=true также можно передать всё то же, что BulkGetElementData:
//     "properties": [...], "gdl_names": "all"|[...], "classifications": "all"|[...],
//     "connected_types": [...], "with_bbox": bool, "with_mesh": bool,
//     "apply_transform": bool }

GS::Optional<GS::UniString> BulkGetGroupMembersCommand::GetRawResponseSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "payload_b64":    { "type": "string"  },
            "compression":    { "type": "string"  },
            "elements_count": { "type": "integer" },
            "groups_count":   { "type": "integer" },
            "entities_count": { "type": "integer" },
            "relations_count":{ "type": "integer" }
        },
        "additionalProperties": false,
        "required": [ "payload_b64", "compression" ]
    })";
}

GS::ObjectState BulkGetGroupMembersCommand::Execute (
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

    std::vector<std::string> elementGuids;
    std::vector<std::string> groupGuids;
    bool recursive = true;

    // with_data: после дедупа групп тянем полные данные для всех уникальных
    // member_guids через общий CollectElementData. Опции ниже 1-в-1 совпадают
    // с payload BulkGetElementData. По умолчанию — false (старое поведение).
    bool withData = false;
    ElementDataOptions dataOpts;

    try {
        nlohmann::json j = nlohmann::json::from_msgpack (raw);
        if (j.contains ("element_guids")) {
            for (const auto& s : j["element_guids"]) elementGuids.push_back (s.get<std::string> ());
        }
        if (j.contains ("group_guids")) {
            for (const auto& s : j["group_guids"]) groupGuids.push_back (s.get<std::string> ());
        }
        if (j.contains ("recursive")) recursive = j["recursive"].get<bool> ();
        if (j.contains ("with_data")) withData = j["with_data"].get<bool> ();

        if (withData) {
            if (j.contains ("properties")) {
                for (const auto& s : j["properties"]) dataOpts.propGuids.push_back (s.get<std::string> ());
            }
            if (j.contains ("gdl_names")) {
                dataOpts.readGdl = true;
                if (j["gdl_names"].is_string () && j["gdl_names"].get<std::string> () == "all") {
                    dataOpts.gdlAll = true;
                } else if (j["gdl_names"].is_array ()) {
                    for (const auto& s : j["gdl_names"]) dataOpts.gdlNames.push_back (s.get<std::string> ());
                }
            }
            if (j.contains ("classifications")) {
                dataOpts.readClass = true;
                if (j["classifications"].is_string () && j["classifications"].get<std::string> () == "all") {
                    GS::Array<API_ClassificationSystem> allSystems;
                    if (ACAPI_Classification_GetClassificationSystems (allSystems) == NoError) {
                        for (const API_ClassificationSystem& s : allSystems) {
                            dataOpts.classSystemGuids.push_back (APIGuidToString (s.guid).ToCStr ().Get ());
                        }
                    }
                } else if (j["classifications"].is_array ()) {
                    for (const auto& s : j["classifications"]) dataOpts.classSystemGuids.push_back (s.get<std::string> ());
                }
            }
            if (j.contains ("connected_types")) {
                for (const auto& s : j["connected_types"]) {
                    API_ElemTypeID t;
                    if (StringToElemTypeID (s.get<std::string> (), t)) dataOpts.connectedTypes.push_back (t);
                }
            }
            if (j.contains ("with_bbox"))       dataOpts.withBbox       = j["with_bbox"].get<bool> ();
            if (j.contains ("with_mesh"))       dataOpts.withMesh       = j["with_mesh"].get<bool> ();
            if (j.contains ("apply_transform")) dataOpts.applyTransform = j["apply_transform"].get<bool> ();

            // Члены группы — уже знаем их группу. Не рекурсируем обратно.
            dataOpts.withGroupInfo    = false;
            dataOpts.withGroupMembers = false;
        }
    } catch (const std::exception& e) {
        const std::string msg = std::string ("msgpack decode failed: ") + e.what ();
        return CreateErrorResponse (APIERR_BADPARS, GS::UniString (msg.c_str ()));
    }

    if (elementGuids.empty () && groupGuids.empty ()) {
        return CreateErrorResponse (APIERR_BADPARS,
            "payload must contain 'element_guids' or 'group_guids'");
    }

    nlohmann::ordered_json out;
    out["groups"] = nlohmann::json::array ();

    // Дедупликация: одна запись на уникальную группу (group_guid).
    // Если source-элементы без группы (groupGuid == APINULLGuid) — по-прежнему
    // отдельная запись на каждый source (их нельзя «слить» по смыслу).
    std::map<std::string, size_t> groupIndexByGuid;

    auto expandGroup = [&](const std::string& sourceGuid,
                           const char* sourceKind,
                           const API_Guid& groupGuid) {
        if (groupGuid == APINULLGuid) {
            nlohmann::ordered_json g;
            g["source_guid"]  = sourceGuid;
            g["source_kind"]  = sourceKind;
            g["group_guid"]   = nullptr;
            g["member_guids"] = nlohmann::json::array ();
            out["groups"].push_back (g);
            return;
        }

        const std::string groupGuidStr = APIGuidToString (groupGuid).ToCStr ().Get ();
        auto it = groupIndexByGuid.find (groupGuidStr);
        if (it != groupIndexByGuid.end ()) {
            // Уже есть — добавляем source в source_guids/source_kinds этой записи.
            auto& g = out["groups"][it->second];
            g["source_guids"].push_back (sourceGuid);
            g["source_kinds"].push_back (sourceKind);
            return;
        }

        nlohmann::ordered_json g;
        g["group_guid"]   = groupGuidStr;
        g["member_guids"] = nlohmann::json::array ();
        g["source_guids"] = nlohmann::json::array ();
        g["source_kinds"] = nlohmann::json::array ();
        g["source_guids"].push_back (sourceGuid);
        g["source_kinds"].push_back (sourceKind);
        // Обратная совместимость: первый source_guid / source_kind.
        g["source_guid"] = sourceGuid;
        g["source_kind"] = sourceKind;

        GS::Array<API_Guid> members;
        GSErrCode e = recursive
            ? ACAPI_Grouping_GetAllGroupedElems (groupGuid, &members)
            : ACAPI_Grouping_GetGroupedElems (groupGuid, &members);
        if (e == NoError) {
            for (const API_Guid& m : members) {
                g["member_guids"].push_back (APIGuidToString (m).ToCStr ().Get ());
            }
        }
        out["groups"].push_back (g);
        groupIndexByGuid[groupGuidStr] = out["groups"].size () - 1;
    };

    // 1. element_guids: для каждого найти его группу, развернуть.
    for (const std::string& eg : elementGuids) {
        API_Guid elemGuid = APIGuidFromString (eg.c_str ());
        if (elemGuid == APINULLGuid) {
            expandGroup (eg, "element", APINULLGuid);
            continue;
        }
        API_Guid parentGroup = APINULLGuid;
        if (ACAPI_Grouping_GetGroup (elemGuid, &parentGroup) != NoError) {
            parentGroup = APINULLGuid;
        }
        expandGroup (eg, "element", parentGroup);
    }

    // 2. group_guids: развернуть напрямую.
    for (const std::string& gg : groupGuids) {
        API_Guid groupGuid = APIGuidFromString (gg.c_str ());
        if (groupGuid == APINULLGuid) {
            expandGroup (gg, "group", APINULLGuid);
            continue;
        }
        expandGroup (gg, "group", groupGuid);
    }

    // 3. with_data=true: тянем полные данные для всех уникальных member_guids.
    //    Дедуплицируем по гуиду (один элемент может быть в нескольких группах).
    if (withData) {
        std::set<std::string> seenMembers;
        std::vector<std::string> allMembers;
        for (const auto& g : out["groups"]) {
            if (!g.contains ("member_guids")) continue;
            for (const auto& mg : g["member_guids"]) {
                const std::string s = mg.get<std::string> ();
                if (seenMembers.insert (s).second) allMembers.push_back (s);
            }
        }

        dataOpts.elemGuids = std::move (allMembers);

        nlohmann::ordered_json ed = CollectElementData (dataOpts);
        out["entities"]  = std::move (ed["entities"]);
        out["relations"] = std::move (ed["relations"]);
    }

    std::vector<uint8_t> outBytes = nlohmann::json::to_msgpack (out);
    std::string outCompression;
    const std::string outB64 =
        EncodeEnvelope (outBytes.data (), outBytes.size (), outCompression);

    GS::ObjectState response;
    response.Add ("payload_b64", GS::UniString (outB64.c_str ()));
    response.Add ("compression", GS::UniString (outCompression.c_str ()));
    response.Add ("elements_count", static_cast<Int64> (elementGuids.size ()));
    response.Add ("groups_count", static_cast<Int64> (out["groups"].size ()));
    response.Add ("entities_count", static_cast<Int64> (out.contains ("entities") ? out["entities"].size () : 0));
    response.Add ("relations_count", static_cast<Int64> (out.contains ("relations") ? out["relations"].size () : 0));
    return response;
}


// ---------------------------------------------------------------------
//  BulkMoveElementsCommand
// ---------------------------------------------------------------------
//
// Bulk-перенос элементов по вектору (dx,dy,dz) в одном Execute.
// Один undo-барьер на весь батч: Ctrl+Z откатывает все перемещения.
//
// Вход (msgpack в payload_b64):
//   {
//     "moves": [
//       { "source_guid": "guid",
//         "dx": 2.0, "dy": 0.0, "dz": 0.0,
//         "copy": false }   // default false
//     ]
//   }
//
// Выход (msgpack в payload_b64):
//   {
//     "per_source": [ { "source_guid": "...", "moved": true|false,
//                       "error": null | "<текст>" } ],
//     "moved_count":  N,
//     "errors_count": M
//   }

BulkMoveElementsCommand::BulkMoveElementsCommand () :
    CommandBase (CommonSchema::Used)
{
}

GS::String BulkMoveElementsCommand::GetName () const
{
    return "BulkMoveElements";
}

GS::Optional<GS::UniString> BulkMoveElementsCommand::GetInputParametersSchema () const
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

GS::Optional<GS::UniString> BulkMoveElementsCommand::GetRawResponseSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "payload_b64":  { "type": "string" },
            "compression":  { "type": "string" },
            "moved_count":  { "type": "integer" },
            "errors_count": { "type": "integer" }
        },
        "additionalProperties": false,
        "required": [ "payload_b64", "compression" ]
    })";
}

GS::ObjectState BulkMoveElementsCommand::Execute (
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

    struct MoveTask {
        std::string sourceGuid;
        double dx = 0.0;
        double dy = 0.0;
        double dz = 0.0;
        bool copy = false;
    };
    std::vector<MoveTask> moves;

    try {
        nlohmann::json j = nlohmann::json::from_msgpack (raw);
        if (!j.contains ("moves") || !j["moves"].is_array ()) {
            return CreateErrorResponse (APIERR_BADPARS,
                "payload must contain 'moves' array");
        }
        for (const auto& item : j["moves"]) {
            if (!item.contains ("source_guid")) continue;
            MoveTask t;
            t.sourceGuid = item["source_guid"].get<std::string> ();
            if (item.contains ("dx"))   t.dx   = item["dx"].get<double> ();
            if (item.contains ("dy"))   t.dy   = item["dy"].get<double> ();
            if (item.contains ("dz"))   t.dz   = item["dz"].get<double> ();
            if (item.contains ("copy")) t.copy = item["copy"].get<bool> ();
            moves.push_back (t);
        }
    } catch (const std::exception& e) {
        const std::string msg = std::string ("msgpack decode failed: ") + e.what ();
        return CreateErrorResponse (APIERR_BADPARS, GS::UniString (msg.c_str ()));
    }

    nlohmann::ordered_json out;
    out["per_source"]   = nlohmann::json::array ();
    out["moved_count"]  = 0;
    out["errors_count"] = 0;

    size_t movedCount  = 0;
    size_t errorsCount = 0;

    ACAPI_CallUndoableCommand ("BulkMoveElements", [&] () -> GSErrCode {
        for (const MoveTask& task : moves) {
            nlohmann::ordered_json srcOut;
            srcOut["source_guid"] = task.sourceGuid;
            srcOut["moved"]       = false;
            srcOut["error"]       = nullptr;

            API_Guid srcGuid = APIGuidFromString (task.sourceGuid.c_str ());
            if (srcGuid == APINULLGuid) {
                srcOut["error"] = "source_guid is invalid";
                out["per_source"].push_back (srcOut);
                ++errorsCount;
                continue;
            }

            API_Element element = {};
            element.header.guid = srcGuid;
            if (ACAPI_Element_Get (&element) != NoError) {
                srcOut["error"] = "element not found";
                out["per_source"].push_back (srcOut);
                ++errorsCount;
                continue;
            }

            GS::Array<API_Neig> elementsToEdit = { API_Neig (srcGuid) };
            API_EditPars editPars = {};
            editPars.typeID = APIEdit_Drag;
            editPars.endC.x = task.dx;
            editPars.endC.y = task.dy;
            editPars.endC.z = task.dz;
            editPars.withDelete = !task.copy;

            const GSErrCode e = ACAPI_Element_Edit (&elementsToEdit, editPars);
            if (e == NoError) {
                srcOut["moved"] = true;
                ++movedCount;
            } else {
                srcOut["error"] = std::string ("edit failed (code=") +
                    std::to_string (static_cast<long long> (e)) + ")";
                ++errorsCount;
            }
            out["per_source"].push_back (srcOut);
        }
        return NoError;
    });

    out["moved_count"]  = static_cast<uint64_t> (movedCount);
    out["errors_count"] = static_cast<uint64_t> (errorsCount);

    std::vector<uint8_t> outBytes = nlohmann::json::to_msgpack (out);
    std::string outCompression;
    const std::string outB64 =
        EncodeEnvelope (outBytes.data (), outBytes.size (), outCompression);

    GS::ObjectState response;
    response.Add ("payload_b64",  GS::UniString (outB64.c_str ()));
    response.Add ("compression",  GS::UniString (outCompression.c_str ()));
    response.Add ("moved_count",  static_cast<Int64> (movedCount));
    response.Add ("errors_count", static_cast<Int64> (errorsCount));
    return response;
}


// ---------------------------------------------------------------------
//  BulkRotateElementsCommand
// ---------------------------------------------------------------------
//
// Bulk-поворот элементов на угол angle_rad вокруг центра.
// Если center не задан — берём центр AABB элемента (ACAPI_Element_CalcBounds).
// Один undo-барьер на весь батч.
//
// Вход (msgpack в payload_b64):
//   {
//     "rotations": [
//       { "source_guid": "guid",
//         "angle_rad": 1.5707963267948966,
//         "center_x": 100.0, "center_y": 200.0,   // опц.: если нет — центр AABB
//         "copy": false }
//     ]
//   }
//
// Выход (msgpack в payload_b64):
//   {
//     "per_source": [ { "source_guid": "...", "rotated": true|false,
//                       "error": null | "<текст>" } ],
//     "rotated_count": N,
//     "errors_count":  M
//   }

namespace {

GSErrCode RotateSingleElementByGuid (const API_Guid& elemGuid,
                                     double angleRad,
                                     bool hasCenter, const API_Coord& centerIn,
                                     bool withCopy)
{
    API_Coord orig = centerIn;
    if (!hasCenter) {
        API_Elem_Head head = {};
        head.guid = elemGuid;
        API_Box3D box = {};
        const GSErrCode boundsErr = ACAPI_Element_CalcBounds (&head, &box);
        if (boundsErr != NoError) {
            return boundsErr;
        }
        orig.x = (box.xMin + box.xMax) / 2.0;
        orig.y = (box.yMin + box.yMax) / 2.0;
    }

    API_EditPars pars = {};
    pars.typeID = APIEdit_Rotate;
    pars.origC  = orig;

    const double R = 1.0;
    pars.begC.x = orig.x + R;
    pars.begC.y = orig.y;
    pars.begC.z = 0.0;
    pars.endC.x = orig.x + R * std::cos (angleRad);
    pars.endC.y = orig.y + R * std::sin (angleRad);
    pars.endC.z = 0.0;
    pars.withDelete = !withCopy;

    GS::Array<API_Neig> neigs;
    neigs.Push (API_Neig (elemGuid));
    return ACAPI_Element_Edit (&neigs, pars);
}

}  // namespace

BulkRotateElementsCommand::BulkRotateElementsCommand () :
    CommandBase (CommonSchema::Used)
{
}

GS::String BulkRotateElementsCommand::GetName () const
{
    return "BulkRotateElements";
}

GS::Optional<GS::UniString> BulkRotateElementsCommand::GetInputParametersSchema () const
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

GS::Optional<GS::UniString> BulkRotateElementsCommand::GetRawResponseSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "payload_b64":   { "type": "string" },
            "compression":   { "type": "string" },
            "rotated_count": { "type": "integer" },
            "errors_count":  { "type": "integer" }
        },
        "additionalProperties": false,
        "required": [ "payload_b64", "compression" ]
    })";
}

GS::ObjectState BulkRotateElementsCommand::Execute (
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

    struct RotTask {
        std::string sourceGuid;
        double angleRad = 0.0;
        bool hasCenter = false;
        double cx = 0.0;
        double cy = 0.0;
        bool copy = false;
    };
    std::vector<RotTask> tasks;

    try {
        nlohmann::json j = nlohmann::json::from_msgpack (raw);
        if (!j.contains ("rotations") || !j["rotations"].is_array ()) {
            return CreateErrorResponse (APIERR_BADPARS,
                "payload must contain 'rotations' array");
        }
        for (const auto& item : j["rotations"]) {
            if (!item.contains ("source_guid")) continue;
            if (!item.contains ("angle_rad")) continue;
            RotTask t;
            t.sourceGuid = item["source_guid"].get<std::string> ();
            t.angleRad   = item["angle_rad"].get<double> ();
            if (item.contains ("center_x") && item.contains ("center_y")) {
                t.cx = item["center_x"].get<double> ();
                t.cy = item["center_y"].get<double> ();
                t.hasCenter = true;
            }
            if (item.contains ("copy")) t.copy = item["copy"].get<bool> ();
            tasks.push_back (t);
        }
    } catch (const std::exception& e) {
        const std::string msg = std::string ("msgpack decode failed: ") + e.what ();
        return CreateErrorResponse (APIERR_BADPARS, GS::UniString (msg.c_str ()));
    }

    nlohmann::ordered_json out;
    out["per_source"]    = nlohmann::json::array ();
    out["rotated_count"] = 0;
    out["errors_count"]  = 0;

    size_t rotatedCount = 0;
    size_t errorsCount  = 0;

    ACAPI_CallUndoableCommand ("BulkRotateElements", [&] () -> GSErrCode {
        for (const RotTask& task : tasks) {
            nlohmann::ordered_json srcOut;
            srcOut["source_guid"] = task.sourceGuid;
            srcOut["rotated"]     = false;
            srcOut["error"]       = nullptr;

            API_Guid srcGuid = APIGuidFromString (task.sourceGuid.c_str ());
            if (srcGuid == APINULLGuid) {
                srcOut["error"] = "source_guid is invalid";
                out["per_source"].push_back (srcOut);
                ++errorsCount;
                continue;
            }

            API_Coord center = {};
            center.x = task.cx;
            center.y = task.cy;

            const GSErrCode e = RotateSingleElementByGuid (srcGuid,
                task.angleRad, task.hasCenter, center, task.copy);
            if (e == NoError) {
                srcOut["rotated"] = true;
                ++rotatedCount;
            } else {
                srcOut["error"] = std::string ("rotate failed (code=") +
                    std::to_string (static_cast<long long> (e)) + ")";
                ++errorsCount;
            }
            out["per_source"].push_back (srcOut);
        }
        return NoError;
    });

    out["rotated_count"] = static_cast<uint64_t> (rotatedCount);
    out["errors_count"]  = static_cast<uint64_t> (errorsCount);

    std::vector<uint8_t> outBytes = nlohmann::json::to_msgpack (out);
    std::string outCompression;
    const std::string outB64 =
        EncodeEnvelope (outBytes.data (), outBytes.size (), outCompression);

    GS::ObjectState response;
    response.Add ("payload_b64",   GS::UniString (outB64.c_str ()));
    response.Add ("compression",   GS::UniString (outCompression.c_str ()));
    response.Add ("rotated_count", static_cast<Int64> (rotatedCount));
    response.Add ("errors_count",  static_cast<Int64> (errorsCount));
    return response;
}


// ---------------------------------------------------------------------
//  BulkSetElementDataCommand
// ---------------------------------------------------------------------
//
// Универсальная запись полей в элементы (v0.1.0, итерация 3a).
// Пишет: story_index, layer_index, pos_x/pos_y/level/angle (для Object/Lamp).
// Принимает (но пока не пишет): GDL/*, Archicad/*, class/*, text — уходят в
// ignored_not_implemented[] с явной причиной. bbox_* — в ignored_readonly[].
// Один undo-барьер на весь батч, поддержка dry_run.
//
// Вход (msgpack в payload_b64):
//   {
//     "entities": [
//       { "guid": "guid",
//         "parameters": {
//           "story_index": 0,           // int
//           "layer_index": 853,          // int
//           "object_pos_x": 100.0,       // double (алиас: pos_x)
//           "object_pos_y": 200.0,       // double (алиас: pos_y)
//           "object_level": 0.0,         // double (алиас: level)
//           "object_angle": 1.5708,      // double (алиас: angle)
//           "GDL/A": 2.0,                // not_implemented (3b)
//           "Archicad/...": "...",       // not_implemented (3c)
//           "class/<system>": "<item>",  // not_implemented (3d)
//           "bbox_size_x": 1.0,          // read-only — ignored
//           "text": "..."                // not_implemented (3d) — использовать BulkSetTexts
//         } }
//     ],
//     "dry_run": false
//   }
//
// Выход (msgpack в payload_b64):
//   {
//     "per_source": [
//       { "guid": "...",
//         "applied": ["story_index", "object_pos_x"],
//         "ignored_readonly": ["bbox_size_x"],
//         "ignored_not_implemented": ["GDL/A", "text"],
//         "ignored_unknown": ["foo"],
//         "errors": [ {"key": "...", "msg": "...", "code": -N} ] }
//     ],
//     "applied_count": N,
//     "errors_count":  M,
//     "dry_run": bool
//   }

namespace {

enum class SetKeyKind {
    Story,
    Layer,
    PosX,
    PosY,
    Level,
    Angle,
    Text,
    Gdl,
    Archicad,
    Class,
    ReadOnly,
    Unknown
};

bool StartsWith (const std::string& s, const char* prefix) {
    const size_t n = std::strlen (prefix);
    return s.size () >= n && s.compare (0, n, prefix) == 0;
}

SetKeyKind DetectSetKeyKind (const std::string& key)
{
    if (key == "story_index") return SetKeyKind::Story;
    if (key == "layer_index") return SetKeyKind::Layer;
    if (key == "object_pos_x" || key == "pos_x") return SetKeyKind::PosX;
    if (key == "object_pos_y" || key == "pos_y") return SetKeyKind::PosY;
    if (key == "object_level" || key == "level")  return SetKeyKind::Level;
    if (key == "object_angle" || key == "angle")  return SetKeyKind::Angle;
    if (key == "text") return SetKeyKind::Text;
    if (StartsWith (key, "GDL/")) return SetKeyKind::Gdl;
    if (StartsWith (key, "Archicad/")) return SetKeyKind::Archicad;
    if (StartsWith (key, "class/")) return SetKeyKind::Class;
    if (StartsWith (key, "bbox_")) return SetKeyKind::ReadOnly;
    return SetKeyKind::Unknown;
}

bool IsObjectLike (const API_Element& element)
{
    const API_ElemTypeID t = GetElemTypeId (element.header);
    return t == API_ObjectID || t == API_LampID;
}

struct GdlChange {
    std::string name;
    nlohmann::ordered_json value;
};

struct ClassChange {
    API_Guid systemGuid;
    API_Guid itemGuid;
};

struct PropertyChange {
    API_Guid propertyGuid;
    std::string valueString;
};

GSErrCode ApplyPropertyBatch (const API_Guid& elemGuid,
                              const std::vector<PropertyChange>& props,
                              std::vector<API_Guid>& notAppliedOut)
{
    if (props.empty ()) return NoError;

    GS::Array<API_Guid> propGuids;
    GS::HashTable<API_Guid, GS::UniString> valuesByGuid;
    for (const PropertyChange& pc : props) {
        propGuids.Push (pc.propertyGuid);
        valuesByGuid.Add (pc.propertyGuid, GS::UniString (pc.valueString.c_str ()));
    }

    GS::Array<API_Property> propValues;
    const GSErrCode getErr = ACAPI_Element_GetPropertyValuesByGuid (elemGuid, propGuids, propValues);
    if (getErr != NoError) return getErr;

    PropertyConversionUtils conversionUtils;
    for (API_Property& pv : propValues) {
        GS::UniString* valueStr = valuesByGuid.GetPtr (pv.definition.guid);
        if (valueStr == nullptr) continue;
        GSErrCode e = ACAPI_Property_SetPropertyValueFromString (*valueStr, conversionUtils, &pv);
        if (e != NoError) return e;
        e = ACAPI_Element_SetProperty (elemGuid, pv);
        if (e != NoError) return e;
    }

    // Re-read to detect silently-ignored properties (e.g. read-only Pset).
    // ACAPI_SetProperty returns NoError even when the property was not written.
    GS::Array<API_Property> verify;
    if (ACAPI_Element_GetPropertyValuesByGuid (elemGuid, propGuids, verify) == NoError) {
        for (const API_Property& vp : verify) {
            GS::UniString* wanted = valuesByGuid.GetPtr (vp.definition.guid);
            if (wanted == nullptr) continue;
            GS::UniString current;
            if (ACAPI_Property_GetPropertyValueString (vp, conversionUtils, &current) != NoError
                || current != *wanted)
            {
                notAppliedOut.push_back (vp.definition.guid);
            }
        }
    }
    return NoError;
}

// Compare the requested GDL value against what's actually in the
// API_AddParType. Used to detect locked parameters (ACAPI returns
// NoError but silently discards the write).
static bool GdlValueMatches (const API_AddParType& p, const nlohmann::json& v)
{
    if (v.is_number ()) {
        const double target = v.get<double> ();
        switch (p.typeID) {
            case APIParT_Integer:
                return static_cast<double> (p.value.iNum) == target;
            case APIParT_RealNum:
            case APIParT_Length:
            case APIParT_Angle:
            case APIParT_Ratio: {
                const double diff = p.value.real - target;
                return diff < 1e-9 && diff > -1e-9;
            }
            default:
                return true;   // unknown numeric type — don't lie
        }
    }
    if (v.is_string ()) {
        if (p.typeID != APIParT_CString || p.value.uStr == nullptr) return true;
        const GS::UniString us (p.value.uStr);
        return us == GS::UniString (v.get<std::string> ().c_str ());
    }
    return true;   // non-numeric, non-string — assume matches
}

GSErrCode ApplyGdlBatch (API_Element& element,
                         const std::vector<GdlChange>& gdlChanges,
                         std::vector<std::string>& notAppliedOut)
{
    if (gdlChanges.empty ()) return NoError;

    API_ParamOwnerType paramOwner = {};
    paramOwner.libInd = -1;
#ifdef ServerMainVers_2600
    paramOwner.type   = element.header.type;
#else
    paramOwner.typeID = element.header.typeID;
#endif
    paramOwner.guid   = element.header.guid;

    GSErrCode err = ACAPI_LibraryPart_OpenParameters (&paramOwner);
    if (err != NoError) return err;

    API_GetParamsType getParams = {};
    err = ACAPI_LibraryPart_GetActParameters (&getParams);
    if (err != NoError) {
        ACAPI_LibraryPart_CloseParameters ();
        return err;
    }

    const GSSize nParams = BMGetHandleSize ((GSHandle) getParams.params) / sizeof (API_AddParType);
    for (GSIndex k = 0; k < nParams; ++k) {
        API_AddParType& p = (*getParams.params)[k];
        if (p.typeID == APIParT_Separator) continue;
        const std::string pname (p.name);
        for (const GdlChange& c : gdlChanges) {
            if (c.name == pname) {
                ApplyGdlOverride (p, c.value);
                break;
            }
        }
    }

    API_Element mask = {};
    ACAPI_ELEMENT_MASK_CLEAR (mask);

    API_ElementMemo memo = {};
    memo.params = getParams.params;

    err = ACAPI_Element_Change (&element, &mask, &memo, APIMemoMask_AddPars, true);

    // Re-read (still inside OpenParameters) to detect locked parameters.
    if (err == NoError) {
        ACAPI_DisposeAddParHdl (&getParams.params);
        getParams.params = nullptr;
        if (ACAPI_LibraryPart_GetActParameters (&getParams) == NoError) {
            const GSSize nv = BMGetHandleSize ((GSHandle) getParams.params) / sizeof (API_AddParType);
            for (const GdlChange& c : gdlChanges) {
                bool matches = false;
                for (GSIndex k = 0; k < nv; ++k) {
                    const API_AddParType& p = (*getParams.params)[k];
                    if (p.typeID == APIParT_Separator) continue;
                    if (c.name != std::string (p.name)) continue;
                    matches = GdlValueMatches (p, c.value);
                    break;
                }
                if (!matches) notAppliedOut.push_back (c.name);
            }
        }
    }

    ACAPI_LibraryPart_CloseParameters ();
    ACAPI_DisposeAddParHdl (&getParams.params);
    return err;
}

GSErrCode ApplyClassBatch (const API_Guid& elemGuid,
                           const std::vector<ClassChange>& classes,
                           std::vector<API_Guid>& notAppliedOut)
{
    for (const ClassChange& cc : classes) {
        API_ClassificationItem existing = {};
        const GSErrCode getErr = ACAPI_Element_GetClassificationInSystem (elemGuid, cc.systemGuid, existing);
        if (getErr == NoError && existing.guid != APINULLGuid) {
            const GSErrCode rmErr = ACAPI_Element_RemoveClassificationItem (elemGuid, existing.guid);
            if (rmErr != NoError) return rmErr;
        }
        const GSErrCode addErr = ACAPI_Element_AddClassificationItem (elemGuid, cc.itemGuid);
        if (addErr != NoError) return addErr;

        // Verify the item was really set (locked system → silent ignore).
        API_ClassificationItem verify = {};
        if (ACAPI_Element_GetClassificationInSystem (elemGuid, cc.systemGuid, verify) != NoError
            || verify.guid != cc.itemGuid)
        {
            notAppliedOut.push_back (cc.systemGuid);
        }
    }
    return NoError;
}

}  // namespace

BulkSetElementDataCommand::BulkSetElementDataCommand () :
    CommandBase (CommonSchema::Used)
{
}

GS::String BulkSetElementDataCommand::GetName () const
{
    return "BulkSetElementData";
}

GS::Optional<GS::UniString> BulkSetElementDataCommand::GetInputParametersSchema () const
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

GS::Optional<GS::UniString> BulkSetElementDataCommand::GetRawResponseSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "payload_b64":   { "type": "string" },
            "compression":   { "type": "string" },
            "applied_count": { "type": "integer" },
            "errors_count":  { "type": "integer" },
            "dry_run":       { "type": "boolean" }
        },
        "additionalProperties": false,
        "required": [ "payload_b64", "compression" ]
    })";
}

GS::ObjectState BulkSetElementDataCommand::Execute (
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

    struct EntityTask {
        std::string guid;
        nlohmann::ordered_json parameters;
    };
    std::vector<EntityTask> entities;
    bool dryRun = false;

    try {
        nlohmann::json j = nlohmann::json::from_msgpack (raw);
        if (!j.contains ("entities") || !j["entities"].is_array ()) {
            return CreateErrorResponse (APIERR_BADPARS,
                "payload must contain 'entities' array");
        }
        for (const auto& item : j["entities"]) {
            if (!item.contains ("guid")) continue;
            EntityTask t;
            t.guid = item["guid"].get<std::string> ();
            if (item.contains ("parameters") && item["parameters"].is_object ()) {
                t.parameters = item["parameters"];
            } else {
                t.parameters = nlohmann::ordered_json::object ();
            }
            entities.push_back (t);
        }
        if (j.contains ("dry_run")) dryRun = j["dry_run"].get<bool> ();
    } catch (const std::exception& e) {
        const std::string msg = std::string ("msgpack decode failed: ") + e.what ();
        return CreateErrorResponse (APIERR_BADPARS, GS::UniString (msg.c_str ()));
    }

    nlohmann::ordered_json out;
    out["per_source"]    = nlohmann::json::array ();
    out["applied_count"] = 0;
    out["errors_count"]  = 0;
    out["dry_run"]       = dryRun;

    size_t appliedCount = 0;
    size_t errorsCount  = 0;

    auto doWork = [&] () {
        for (const EntityTask& task : entities) {
            nlohmann::ordered_json srcOut;
            srcOut["guid"]                    = task.guid;
            srcOut["applied"]                 = nlohmann::json::array ();
            srcOut["ignored_readonly"]        = nlohmann::json::array ();
            srcOut["ignored_not_implemented"] = nlohmann::json::array ();
            srcOut["ignored_unknown"]         = nlohmann::json::array ();
            srcOut["errors"]                  = nlohmann::json::array ();

            API_Guid guid = APIGuidFromString (task.guid.c_str ());
            if (guid == APINULLGuid) {
                nlohmann::ordered_json e;
                e["msg"] = "guid is invalid";
                srcOut["errors"].push_back (e);
                out["per_source"].push_back (srcOut);
                ++errorsCount;
                continue;
            }

            API_Element element = {};
            element.header.guid = guid;
            if (ACAPI_Element_Get (&element) != NoError) {
                nlohmann::ordered_json e;
                e["msg"] = "element not found";
                srcOut["errors"].push_back (e);
                out["per_source"].push_back (srcOut);
                ++errorsCount;
                continue;
            }

            API_Element mask = {};
            ACAPI_ELEMENT_MASK_CLEAR (mask);
            bool hasElementChanges = false;
            std::vector<GdlChange>      gdlChanges;
            std::vector<ClassChange>    classChanges;
            std::vector<PropertyChange> propChanges;

            for (auto it = task.parameters.begin (); it != task.parameters.end (); ++it) {
                const std::string key = it.key ();
                const auto& val = it.value ();

                switch (DetectSetKeyKind (key)) {
                    case SetKeyKind::Story: {
                        if (!val.is_number_integer ()) {
                            nlohmann::ordered_json e; e["key"] = key; e["msg"] = "expected integer";
                            srcOut["errors"].push_back (e); ++errorsCount; break;
                        }
                        element.header.floorInd = static_cast<short> (val.get<int> ());
                        ACAPI_ELEMENT_MASK_SET (mask, API_Elem_Head, floorInd);
                        hasElementChanges = true;
                        srcOut["applied"].push_back (key);
                    } break;
                    case SetKeyKind::Layer: {
                        if (!val.is_number_integer ()) {
                            nlohmann::ordered_json e; e["key"] = key; e["msg"] = "expected integer";
                            srcOut["errors"].push_back (e); ++errorsCount; break;
                        }
                        element.header.layer = ACAPI_CreateAttributeIndex (val.get<Int32> ());
                        ACAPI_ELEMENT_MASK_SET (mask, API_Elem_Head, layer);
                        hasElementChanges = true;
                        srcOut["applied"].push_back (key);
                    } break;
                    case SetKeyKind::PosX:
                    case SetKeyKind::PosY:
                    case SetKeyKind::Level:
                    case SetKeyKind::Angle: {
                        if (!IsObjectLike (element)) {
                            srcOut["ignored_unknown"].push_back (key);
                            break;
                        }
                        if (!val.is_number ()) {
                            nlohmann::ordered_json e; e["key"] = key; e["msg"] = "expected number";
                            srcOut["errors"].push_back (e); ++errorsCount; break;
                        }
                        const double d = val.get<double> ();
                        const SetKeyKind kind = DetectSetKeyKind (key);
                        if (kind == SetKeyKind::PosX) {
                            element.object.pos.x = d;
                            ACAPI_ELEMENT_MASK_SET (mask, API_ObjectType, pos.x);
                        } else if (kind == SetKeyKind::PosY) {
                            element.object.pos.y = d;
                            ACAPI_ELEMENT_MASK_SET (mask, API_ObjectType, pos.y);
                        } else if (kind == SetKeyKind::Level) {
                            element.object.level = d;
                            ACAPI_ELEMENT_MASK_SET (mask, API_ObjectType, level);
                        } else {
                            element.object.angle = d;
                            ACAPI_ELEMENT_MASK_SET (mask, API_ObjectType, angle);
                        }
                        hasElementChanges = true;
                        srcOut["applied"].push_back (key);
                    } break;
                    case SetKeyKind::Text:
                        srcOut["ignored_not_implemented"].push_back (key);
                        break;
                    case SetKeyKind::Gdl: {
                        GdlChange c;
                        c.name  = key.substr (4);
                        c.value = val;
                        gdlChanges.push_back (c);
                    } break;
                    case SetKeyKind::Archicad: {
                        const std::string guidStr = key.substr (9);
                        const API_Guid pg = APIGuidFromString (guidStr.c_str ());
                        if (pg == APINULLGuid || !val.is_string ()) {
                            nlohmann::ordered_json e; e["key"] = key;
                            e["msg"] = "expected string value and valid property guid in key";
                            srcOut["errors"].push_back (e); ++errorsCount; break;
                        }
                        PropertyChange pc;
                        pc.propertyGuid = pg;
                        pc.valueString  = val.get<std::string> ();
                        propChanges.push_back (pc);
                    } break;
                    case SetKeyKind::Class: {
                        const std::string sysStr = key.substr (6);
                        const API_Guid sg = APIGuidFromString (sysStr.c_str ());
                        const API_Guid ig = val.is_string ()
                            ? APIGuidFromString (val.get<std::string> ().c_str ())
                            : APINULLGuid;
                        if (sg == APINULLGuid || ig == APINULLGuid) {
                            nlohmann::ordered_json e; e["key"] = key;
                            e["msg"] = "expected '<item-guid>' value and valid system-guid in key";
                            srcOut["errors"].push_back (e); ++errorsCount; break;
                        }
                        ClassChange cc;
                        cc.systemGuid = sg;
                        cc.itemGuid   = ig;
                        classChanges.push_back (cc);
                    } break;
                    case SetKeyKind::ReadOnly:
                        srcOut["ignored_readonly"].push_back (key);
                        break;
                    case SetKeyKind::Unknown:
                    default:
                        srcOut["ignored_unknown"].push_back (key);
                        break;
                }
            }

            if (dryRun) {
                appliedCount += static_cast<int> (srcOut["applied"].size ());
                out["per_source"].push_back (srcOut);
                continue;
            }

            // 1. Element fields (story/layer/pos/level/angle)
            // (parser already pushed the keys into srcOut["applied"])
            if (hasElementChanges) {
                const GSErrCode e = ACAPI_Element_Change (&element, &mask, nullptr, 0, true);
                if (e != NoError) {
                    nlohmann::ordered_json err2;
                    err2["msg"]  = "ACAPI_Element_Change(element) failed";
                    err2["code"] = static_cast<int64_t> (e);
                    srcOut["errors"].push_back (err2);
                    ++errorsCount;
                }
            }

            // 2. GDL parameters
            if (!gdlChanges.empty ()) {
                std::vector<std::string> notApplied;
                const GSErrCode e = ApplyGdlBatch (element, gdlChanges, notApplied);
                if (e == NoError) {
                    for (const GdlChange& c : gdlChanges) {
                        bool bad = false;
                        for (const std::string& n : notApplied) {
                            if (n == c.name) { bad = true; break; }
                        }
                        if (bad) srcOut["ignored_locked"].push_back ("GDL/" + c.name);
                        else     srcOut["applied"].push_back ("GDL/" + c.name);
                    }
                } else {
                    nlohmann::ordered_json err2;
                    err2["msg"]  = "GDL batch failed";
                    err2["code"] = static_cast<int64_t> (e);
                    srcOut["errors"].push_back (err2);
                    ++errorsCount;
                }
            }

            // 3. Properties (Archicad/*)
            if (!propChanges.empty ()) {
                std::vector<API_Guid> notApplied;
                const GSErrCode e = ApplyPropertyBatch (guid, propChanges, notApplied);
                if (e == NoError) {
                    for (const PropertyChange& pc : propChanges) {
                        bool bad = false;
                        for (const API_Guid& g : notApplied) {
                            if (g == pc.propertyGuid) { bad = true; break; }
                        }
                        const std::string key =
                            std::string ("Archicad/") + APIGuidToString (pc.propertyGuid).ToCStr ().Get ();
                        if (bad) srcOut["ignored_locked"].push_back (key);
                        else     srcOut["applied"].push_back (key);
                    }
                } else {
                    nlohmann::ordered_json err2;
                    err2["msg"]  = "Property batch failed";
                    err2["code"] = static_cast<int64_t> (e);
                    srcOut["errors"].push_back (err2);
                    ++errorsCount;
                }
            }

            // 4. Classifications (class/*)
            if (!classChanges.empty ()) {
                std::vector<API_Guid> notApplied;
                const GSErrCode e = ApplyClassBatch (guid, classChanges, notApplied);
                if (e == NoError) {
                    for (const ClassChange& cc : classChanges) {
                        bool bad = false;
                        for (const API_Guid& g : notApplied) {
                            if (g == cc.systemGuid) { bad = true; break; }
                        }
                        const std::string key =
                            std::string ("class/") + APIGuidToString (cc.systemGuid).ToCStr ().Get ();
                        if (bad) srcOut["ignored_locked"].push_back (key);
                        else     srcOut["applied"].push_back (key);
                    }
                } else {
                    nlohmann::ordered_json err2;
                    err2["msg"]  = "Class batch failed";
                    err2["code"] = static_cast<int64_t> (e);
                    srcOut["errors"].push_back (err2);
                    ++errorsCount;
                }
            }

            appliedCount += static_cast<int> (srcOut["applied"].size ());
            out["per_source"].push_back (srcOut);
        }
    };

    if (dryRun) {
        doWork ();
    } else {
        ACAPI_CallUndoableCommand ("BulkSetElementData", [&] () -> GSErrCode {
            doWork ();
            return NoError;
        });
    }

    out["applied_count"] = static_cast<uint64_t> (appliedCount);
    out["errors_count"]  = static_cast<uint64_t> (errorsCount);

    std::vector<uint8_t> outBytes = nlohmann::json::to_msgpack (out);
    std::string outCompression;
    const std::string outB64 =
        EncodeEnvelope (outBytes.data (), outBytes.size (), outCompression);

    GS::ObjectState response;
    response.Add ("payload_b64",   GS::UniString (outB64.c_str ()));
    response.Add ("compression",   GS::UniString (outCompression.c_str ()));
    response.Add ("applied_count", static_cast<Int64> (appliedCount));
    response.Add ("errors_count",  static_cast<Int64> (errorsCount));
    response.Add ("dry_run",       dryRun);
    return response;
}


// ---------------------------------------------------------------------
//  BulkDeleteElementsCommand
// ---------------------------------------------------------------------
//
// Массовое удаление элементов одним Execute. Один undo на весь батч.
//
// Вход (msgpack в payload_b64):
//   { "guids": ["guid", "guid", ...] }
//
// Выход (msgpack в payload_b64):
//   {
//     "per_source":   [ { "guid": "...", "deleted": true|false, "error": null|"<текст>" } ],
//     "deleted_count": N,
//     "errors_count":  M
//   }

BulkDeleteElementsCommand::BulkDeleteElementsCommand () :
    CommandBase (CommonSchema::Used)
{
}

GS::String BulkDeleteElementsCommand::GetName () const
{
    return "BulkDeleteElements";
}

GS::Optional<GS::UniString> BulkDeleteElementsCommand::GetInputParametersSchema () const
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

GS::Optional<GS::UniString> BulkDeleteElementsCommand::GetRawResponseSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "payload_b64":   { "type": "string" },
            "compression":   { "type": "string" },
            "deleted_count": { "type": "integer" },
            "errors_count":  { "type": "integer" }
        },
        "additionalProperties": false,
        "required": [ "payload_b64", "compression" ]
    })";
}

GS::ObjectState BulkDeleteElementsCommand::Execute (
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

    std::vector<std::string> guids;
    try {
        nlohmann::json j = nlohmann::json::from_msgpack (raw);
        if (!j.contains ("guids") || !j["guids"].is_array ()) {
            return CreateErrorResponse (APIERR_BADPARS, "payload must contain 'guids' array");
        }
        for (const auto& item : j["guids"]) {
            if (item.is_string ()) guids.push_back (item.get<std::string> ());
        }
    } catch (const std::exception& e) {
        const std::string msg = std::string ("msgpack decode failed: ") + e.what ();
        return CreateErrorResponse (APIERR_BADPARS, GS::UniString (msg.c_str ()));
    }

    nlohmann::ordered_json out;
    out["per_source"]    = nlohmann::json::array ();
    out["deleted_count"] = 0;
    out["errors_count"]  = 0;

    size_t deletedCount = 0;
    size_t errorsCount  = 0;

    ACAPI_CallUndoableCommand ("BulkDeleteElements", [&] () -> GSErrCode {
        // Собираем валидные гуиды для одного массива; невалидные — в отчёт.
        GS::Array<API_Guid> toDelete;
        std::vector<std::string> validSources;
        for (const std::string& g : guids) {
            API_Guid guid = APIGuidFromString (g.c_str ());
            if (guid == APINULLGuid) {
                nlohmann::ordered_json srcOut;
                srcOut["guid"]    = g;
                srcOut["deleted"] = false;
                srcOut["error"]   = "guid is invalid";
                out["per_source"].push_back (srcOut);
                ++errorsCount;
                continue;
            }
            toDelete.Push (guid);
            validSources.push_back (g);
        }

        GSErrCode delErr = NoError;
        if (!toDelete.IsEmpty ()) {
            delErr = ACAPI_Element_Delete (toDelete);
        }

        for (const std::string& g : validSources) {
            nlohmann::ordered_json srcOut;
            srcOut["guid"] = g;
            if (delErr == NoError) {
                srcOut["deleted"] = true;
                srcOut["error"]   = nullptr;
                ++deletedCount;
            } else {
                srcOut["deleted"] = false;
                srcOut["error"]   = std::string ("delete failed (code=") +
                    std::to_string (static_cast<long long> (delErr)) + ")";
                ++errorsCount;
            }
            out["per_source"].push_back (srcOut);
        }
        return NoError;
    });

    out["deleted_count"] = static_cast<uint64_t> (deletedCount);
    out["errors_count"]  = static_cast<uint64_t> (errorsCount);

    std::vector<uint8_t> outBytes = nlohmann::json::to_msgpack (out);
    std::string outCompression;
    const std::string outB64 =
        EncodeEnvelope (outBytes.data (), outBytes.size (), outCompression);

    GS::ObjectState response;
    response.Add ("payload_b64",   GS::UniString (outB64.c_str ()));
    response.Add ("compression",   GS::UniString (outCompression.c_str ()));
    response.Add ("deleted_count", static_cast<Int64> (deletedCount));
    response.Add ("errors_count",  static_cast<Int64> (errorsCount));
    return response;
}
