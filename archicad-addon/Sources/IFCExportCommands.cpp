// IFCExportCommands.cpp
#include "IFCExportCommands.hpp"
#include "APIdefs_Goodies.h"
#include "ACAPinc.h"

ExportFilteredIFCCommand::ExportFilteredIFCCommand()
    : CommandBase(CommonSchema::Used) {}

GS::String ExportFilteredIFCCommand::GetName() const { return "ExportFilteredIFC"; }

GS::Optional<GS::UniString> ExportFilteredIFCCommand::GetInputParametersSchema() const {
    return R"({
        "type": "object",
        "properties": {
            "filePath": { "type": "string" },
            "elementGuids": {
                "type": "array",
                "items": {
                    "type": "object",
                    "properties": {
                        "guid": { "type": "string" }
                    },
                    "required": ["guid"]
                }
            },
            "translatorName": { "type": "string", "default": "" }
        },
        "required": ["filePath", "elementGuids"]
    })";
}

GS::Optional<GS::UniString> ExportFilteredIFCCommand::GetResponseSchema() const {
    return R"({ "$ref": "#/ExecutionResult" })";
}

GS::ObjectState ExportFilteredIFCCommand::Execute(const GS::ObjectState& parameters, GS::ProcessControl&) const {
    GS::UniString filePath;
    GS::Array<GS::ObjectState> elementGuids;
    GS::UniString translatorName;

    parameters.Get("filePath", filePath);
    parameters.Get("elementGuids", elementGuids);
    parameters.Get("translatorName", translatorName);

    // Формируем параметры экспорта для Archicad 26+
    API_IFCExportParams exportParams = {};
    exportParams.filePath = filePath;

    // Устанавливаем транслятор, если указан
    if (!translatorName.IsEmpty()) {
        // Получаем список доступных трансляторов и ищем нужный
        GS::Array<API_IFCTranslatorIdentifier> translators;
        if (ACAPI_IFC_GetIFCExportTranslatorsList(translators) == NoError) {
            for (const auto& translator : translators) {
                if (translator.name == translatorName) {
                    exportParams.translatorIdentifier = translator;
                    break;
                }
            }
        }
    }

    // Фильтр элементов: экспортируем только указанные GUID
    GS::Array<API_Guid> filterGuids;
    for (const auto& item : elementGuids) {
        GS::String guidStr;
        if (item.Get("guid", guidStr)) {
            API_Guid guid = APIGuidFromString(guidStr.ToCStr());
            if (guid != APINULLGuid)
                filterGuids.Push(guid);
        }
    }
    if (!filterGuids.IsEmpty())
        exportParams.elementsToExport = filterGuids;  // поле для выборочного экспорта

    GSErrCode err = ACAPI_IFC_Export(exportParams);
    return err == NoError
        ? CreateSuccessfulExecutionResult()
        : CreateFailedExecutionResult(err, "Failed to export filtered IFC");
}
