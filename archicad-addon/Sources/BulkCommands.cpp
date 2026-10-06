#include "BulkCommands.hpp"
#include "MigrationHelper.hpp"
#include "ElementCreationCommands.hpp"

// ModelerAPI — для BulkGetElementMesh (см. AI_PRINCIPLES.md §2c, §10a).
// Заголовки лежат в Support/Modules/GSModelDevLib и GSModeler. Модули
// линкуются автоматически через LinkGSLibrariesToProject (Tools/CMakeCommon.cmake).
//
// ⚠ Только AC26: на AC25/27/28/29 функция ACAPI_3D_GetCurrentWindowSight
// отсутствует (error C3861 при компиляции), плюс на AC29 подключение
// Model.hpp ломает GDL/PropertyListImp.hpp (каскад C2039/C3083).
// Version-aware путь для 27+ — TODO (§10a). Пока mesh работает на AC26.
//
// ServerMainVers_2600 определён во ВСЕХ DevKit'ах начиная с AC26 — это код
// версии, а не «эта версия = 26». Значит, чтобы выразить «ровно AC26»,
// нужна явная проверка: 2600 есть И 2700 нет. Иначе mesh-код попадает в
// сборку AC27/28/29 — и падает C3861 на ACAPI_3D_GetCurrentWindowSight.
#if defined (ServerMainVers_2600) && !defined (ServerMainVers_2700)
#define TAPIR_AC26_ONLY 1
#else
#define TAPIR_AC26_ONLY 0
#endif

#if TAPIR_AC26_ONLY
#include "Model.hpp"
#include "ModelElement.hpp"
#include "ModelMeshBody.hpp"
#include "Polygon.hpp"
#include "ConvexPolygon.hpp"
#include "Vertex.hpp"
#include "exp.h"
#include "Sight.hpp"
#include "IAttributeReader.hpp"
#endif

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
            API_LibPart lp = {};
            lp.index = element.object.libInd;
            if (ACAPI_LibPart_Get (&lp) == NoError) {
                params["object_lib_part_name"] =
                    GS::UniString (lp.docu_UName).ToCStr ().Get ();
            }
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
            for (API_ElemTypeID t : connectedTypes) {
                GS::Array<API_Guid> connectedElements;
                if (ACAPI_Grouping_GetConnectedElements (guid, t, &connectedElements) != NoError) continue;
                for (const API_Guid& toGuid : connectedElements) {
                    nlohmann::ordered_json rel;
                    rel["from_guid"] = guidStr;
                    rel["to_guid"]   = APIGuidToString (toGuid).ToCStr ().Get ();
                    rel["kind"]      = "connected_to";
                    rel["via"]       = ElementTypeName (t);
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

// Извлекает mesh одного элемента. Возвращает false + errOut — если
// ACAPI не дал информацию или тело пустое.
//
// ⚠ ВРЕМЕННЫЙ STUB. Точные имена полей в API_BodyType / API_VertType /
// API_PgonType / API_PedgType из AC26 DevKit (файл APIdefs_3D.h) на момент
// написания неизвестны, а угадывание (fvert/lvert/fpgon/lpgon/API_PEdgID/
// pedg.vert1) дало 6 ошибок компиляции. Как только поля будут известны
// (или найдётся путь через ModelerAPI::MeshBody) — здесь вернётся полная
// реализация обхода 3D-компонент.
//
// Реализация через ModelerAPI (см. DevKit Examples/ModelAccess_Test):
//   1. ACAPI_3D_GetCurrentWindowSight — получить Sight активного окна.
//      Работает только если активно 3D-окно; в FloorPlan вернёт nullptr.
//   2. EXPGetModel(SightPtr, &Model, IAttributeReader*) — построить Model.
//   3. Обход: Model::GetElement(i, &Element) — поиск по GetElemGuid().
//   4. Element::GetTessellatedBody(iBody, &MeshBody)
//      → MeshBody::GetPolygon(i, &Polygon)
//      → Polygon::GetConvexPolygon(i, &ConvexPolygon)
//      → ConvexPolygon::GetVertexIndex(k) → MeshBody::GetVertex(idx, &Vertex)
//   5. Fan-триангуляция каждого ConvexPolygon.
//
// Все индексы у ModelerAPI 1-based (см. ModelAccess_Test_Exporter.cpp:334).
// В vertices/triangles для msgpack — 0-based (стандарт для GL/VTK/ifcopenshell).
bool ExtractElementMesh (const API_Elem_Head& elemHead,
                         bool /*applyTransform*/,
                         std::vector<float>& outVertices,
                         std::vector<uint32_t>& outTriangles,
                         std::string& errOut)
{
    outVertices.clear ();
    outTriangles.clear ();

#if !TAPIR_AC26_ONLY
    // Mesh реализован только для AC26. На остальных версиях — graceful stub.
    // Причины: (1) ACAPI_3D_GetCurrentWindowSight есть только в AC26; (2) на
    // AC29 Model.hpp ломает GDL/PropertyListImp.hpp. См. AI_PRINCIPLES §10a.
    errOut = "mesh not supported on this Archicad version (only AC26 for now)";
    return false;
#else
    // ---- 1. Sight активного окна ----
    void* sightRaw = nullptr;
    if (ACAPI_3D_GetCurrentWindowSight (&sightRaw) != NoError || sightRaw == nullptr) {
        errOut = "no 3D window is open; call TapirConnection.change_window('3DModel') first";
        return false;
    }

    // ACAPI отдаёт адрес существующего Modeler::SightPtr (GS::SharedPtr<Sight>).
    // Копируем — копия инкрементит refcount, живёт на время вызова, оригинал не трогаем.
    auto* sightPtrPtr = static_cast<Modeler::SightPtr*> (sightRaw);
    if (sightPtrPtr == nullptr) {
        errOut = "invalid SightPtr from ACAPI_3D_GetCurrentWindowSight";
        return false;
    }
    Modeler::SightPtr sight = *sightPtrPtr;

    // ---- 2. AttributeReader + EXPGetModel ----
    GS::Owner<Modeler::IAttributeReader> attrReader (
        ACAPI_Attribute_GetCurrentAttributeSetReader ());
    if (attrReader == nullptr) {
        errOut = "failed to get IAttributeReader";
        return false;
    }
    ModelerAPI::Model model;
    if (EXPGetModel (sight, &model, attrReader.Get ()) != NoError) {
        errOut = "EXPGetModel failed";
        return false;
    }

    // ---- 3. Поиск элемента по GUID ----
    const Int32 nElements = model.GetElementCount ();
    ModelerAPI::Element element;
    bool found = false;
    for (Int32 iElem = 1; iElem <= nElements; ++iElem) {
        model.GetElement (iElem, &element);
        if (element.GetElemGuid () == elemHead.guid) {
            found = true;
            break;
        }
    }
    if (!found) {
        errOut = "element not found in current 3D model (hidden layer? not in view?)";
        return false;
    }

    const Int32 nBodies = element.GetTessellatedBodyCount ();
    if (nBodies < 1) {
        errOut = "element has no tessellated body (not a 3D element?)";
        return false;
    }

    // ---- 4. Обход mesh ----
    // Дедупликация вершин: одна и та же вершина встречается в нескольких
    // полигонах. Без remap получим дубликаты и кривые нормали.
    // Ключ: (bodyIdx << 32) | localVertexIndex (1-based в body).
    std::unordered_map<uint64_t, uint32_t> vertexRemap;

    ModelerAPI::MeshBody body;
    ModelerAPI::Polygon polygon;
    ModelerAPI::ConvexPolygon convexPolygon;
    ModelerAPI::Vertex vertex;

    auto getOrAddVertex = [&](uint32_t bodyIdx, int32_t localIdx, ModelerAPI::MeshBody& b) -> uint32_t {
        const uint64_t key = (static_cast<uint64_t> (bodyIdx) << 32) |
                             static_cast<uint32_t> (localIdx);
        auto it = vertexRemap.find (key);
        if (it != vertexRemap.end ()) return it->second;
        b.GetVertex (localIdx, &vertex);
        const uint32_t newIdx = static_cast<uint32_t> (outVertices.size () / 3);
        outVertices.push_back (static_cast<float> (vertex.x));
        outVertices.push_back (static_cast<float> (vertex.y));
        outVertices.push_back (static_cast<float> (vertex.z));
        vertexRemap[key] = newIdx;
        return newIdx;
    };

    std::vector<uint32_t> polyIndices;
    for (Int32 iBody = 1; iBody <= nBodies; ++iBody) {
        element.GetTessellatedBody (iBody, &body);
        const Int32 nPoly = body.GetPolygonCount ();
        for (Int32 iPgon = 1; iPgon <= nPoly; ++iPgon) {
            body.GetPolygon (iPgon, &polygon);
            const Int32 nCvx = polygon.GetConvexPolygonCount ();
            for (Int32 iCvx = 1; iCvx <= nCvx; ++iCvx) {
                polygon.GetConvexPolygon (iCvx, &convexPolygon);
                const Int32 nPedge = convexPolygon.GetVertexCount ();
                if (nPedge < 3) continue;
                polyIndices.clear ();
                polyIndices.reserve (static_cast<size_t> (nPedge));
                for (Int32 iPedge = 1; iPedge <= nPedge; ++iPedge) {
                    const int32_t origIdx = convexPolygon.GetVertexIndex (iPedge);
                    polyIndices.push_back (
                        getOrAddVertex (static_cast<uint32_t> (iBody), origIdx, body));
                }
                // Fan-триангуляция: (v0, vi, vi+1) для i=1..n-2.
                for (size_t i = 1; i + 1 < polyIndices.size (); ++i) {
                    outTriangles.push_back (polyIndices[0]);
                    outTriangles.push_back (polyIndices[i]);
                    outTriangles.push_back (polyIndices[i + 1]);
                }
            }
        }
    }

    if (outVertices.empty ()) {
        errOut = "empty mesh (no vertices collected)";
        return false;
    }
    return true;
#endif  // ServerMainVers_2600
}

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

    std::string sourceGuidStr;
    std::vector<nlohmann::ordered_json> instances;
    bool deleteSource = false;

    try {
        nlohmann::json j = nlohmann::json::from_msgpack (raw);
        if (!j.contains ("source_guid") || !j.contains ("instances")) {
            return CreateErrorResponse (APIERR_BADPARS,
                "payload must contain 'source_guid' and 'instances'");
        }
        sourceGuidStr = j["source_guid"].get<std::string> ();
        for (const auto& inst : j["instances"]) {
            instances.push_back (inst);
        }
        if (j.contains ("delete_source")) deleteSource = j["delete_source"].get<bool> ();
    } catch (const std::exception& e) {
        const std::string msg = std::string ("msgpack decode failed: ") + e.what ();
        return CreateErrorResponse (APIERR_BADPARS, GS::UniString (msg.c_str ()));
    }

    // Читаем донора.
    API_Guid srcGuid = APIGuidFromString (sourceGuidStr.c_str ());
    if (srcGuid == APINULLGuid) {
        return CreateErrorResponse (APIERR_BADPARS, "source_guid is invalid");
    }
    API_Element srcElem = {};
    srcElem.header.guid = srcGuid;
    if (ACAPI_Element_Get (&srcElem) != NoError) {
        return CreateErrorResponse (APIERR_GENERAL, "source element not found");
    }

    // v1: только Object.
    const API_ElemTypeID srcType = GetElemTypeId (srcElem.header);
    if (srcType != API_ObjectID) {
        return CreateErrorResponse (APIERR_GENERAL,
            "BulkCloneElement v1 supports only Object (source is not an Object)");
    }

    // Получаем полный memo донора (GDL + прочее). Держим на время всей операции.
    API_ElementMemo srcMemo = {};
    constexpr UInt64 kAllMemoMask =
        APIMemoMask_All;
    if (ACAPI_Element_GetMemo (srcGuid, &srcMemo, kAllMemoMask) != NoError) {
        return CreateErrorResponse (APIERR_GENERAL, "failed to get source memo");
    }
    const GS::OnExit srcMemoGuard ([&srcMemo] () { ACAPI_DisposeElemMemoHdls (&srcMemo); });

    nlohmann::ordered_json out;
    out["created_guids"] = nlohmann::json::array ();
    out["errors"] = nlohmann::json::array ();

    // Один undo на весь батч.
    ACAPI_CallUndoableCommand ("BulkCloneElement", [&] () -> GSErrCode {
        for (size_t i = 0; i < instances.size (); ++i) {
            const auto& inst = instances[i];
            API_Element el = srcElem;             // структурная копия шапки + object-полей
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

            // Готовим memo: заново берём от донора (у каждого — свои handle'ы),
            // затем применяем GDL-overrides.
            API_ElementMemo memo = {};
            if (ACAPI_Element_GetMemo (srcGuid, &memo, kAllMemoMask) != NoError) {
                nlohmann::ordered_json e;
                e["index"] = static_cast<int64_t> (i);
                e["msg"] = "failed to get memo from source";
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
                out["created_guids"].push_back (
                    APIGuidToString (el.header.guid).ToCStr ().Get ());
            } else {
                nlohmann::ordered_json e;
                e["index"] = static_cast<int64_t> (i);
                e["msg"]   = "ACAPI_Element_Create failed";
                e["code"]  = static_cast<int64_t> (createErr);
                out["errors"].push_back (e);
            }
            ACAPI_DisposeElemMemoHdls (&memo);
        }

        if (deleteSource) {
            GS::Array<API_Guid> toDelete;
            toDelete.Push (srcGuid);
            ACAPI_Element_Delete (toDelete);
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

GS::Optional<GS::UniString> BulkGetGroupMembersCommand::GetRawResponseSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "payload_b64":   { "type": "string"  },
            "compression":   { "type": "string"  },
            "elements_count":{ "type": "integer" },
            "groups_count":  { "type": "integer" }
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

    try {
        nlohmann::json j = nlohmann::json::from_msgpack (raw);
        if (j.contains ("element_guids")) {
            for (const auto& s : j["element_guids"]) elementGuids.push_back (s.get<std::string> ());
        }
        if (j.contains ("group_guids")) {
            for (const auto& s : j["group_guids"]) groupGuids.push_back (s.get<std::string> ());
        }
        if (j.contains ("recursive")) recursive = j["recursive"].get<bool> ();
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

    auto expandGroup = [&](const std::string& sourceGuid,
                           const char* sourceKind,
                           const API_Guid& groupGuid) {
        nlohmann::ordered_json g;
        g["source_guid"] = sourceGuid;
        g["source_kind"] = sourceKind;
        if (groupGuid == APINULLGuid) {
            g["group_guid"]   = nullptr;
            g["member_guids"] = nlohmann::json::array ();
            out["groups"].push_back (g);
            return;
        }
        g["group_guid"] = APIGuidToString (groupGuid).ToCStr ().Get ();
        g["member_guids"] = nlohmann::json::array ();

        GS::Array<API_Guid> members;
        GSErrCode e = recursive
            ? ACAPI_ElementGroup_GetAllGroupedElems (groupGuid, &members)
            : ACAPI_ElementGroup_GetGroupedElems (groupGuid, &members);
        if (e == NoError) {
            for (const API_Guid& m : members) {
                g["member_guids"].push_back (APIGuidToString (m).ToCStr ().Get ());
            }
        }
        out["groups"].push_back (g);
    };

    // 1. element_guids: для каждого найти его группу, развернуть.
    for (const std::string& eg : elementGuids) {
        API_Guid elemGuid = APIGuidFromString (eg.c_str ());
        if (elemGuid == APINULLGuid) {
            expandGroup (eg, "element", APINULLGuid);
            continue;
        }
        API_Guid parentGroup = APINULLGuid;
        if (ACAPI_ElementGroup_GetGroup (elemGuid, &parentGroup) != NoError) {
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

    std::vector<uint8_t> outBytes = nlohmann::json::to_msgpack (out);
    std::string outCompression;
    const std::string outB64 =
        EncodeEnvelope (outBytes.data (), outBytes.size (), outCompression);

    GS::ObjectState response;
    response.Add ("payload_b64", GS::UniString (outB64.c_str ()));
    response.Add ("compression", GS::UniString (outCompression.c_str ()));
    response.Add ("elements_count", static_cast<Int64> (elementGuids.size ()));
    response.Add ("groups_count", static_cast<Int64> (groupGuids.size ()));
    return response;
}
