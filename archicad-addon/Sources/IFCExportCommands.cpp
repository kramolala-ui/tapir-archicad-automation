// IFCExportCommands.cpp
#include "IFCExportCommands.hpp"
#include "MigrationHelper.hpp"
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
                    "properties": { "guid": { "type": "string" } },
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

    // 1. Сохраняем список всех элементов для последующего восстановления
    GS::Array<API_Guid> allElemGuids;
    ACAPI_Element_GetElemList(API_ZombieElemID, &allElemGuids);

    // 2. Скрываем все элементы
    for (const auto& g : allElemGuids)
        ACAPI_Element_Hide(g, true);

    // 3. Показываем только указанные элементы
    GS::Array<API_Guid> filterGuids;
    for (const auto& item : elementGuids) {
        GS::String guidStr;
        if (item.Get("guid", guidStr)) {
            API_Guid guid = APIGuidFromString(guidStr.ToCStr());
            if (guid != APINULLGuid) {
                ACAPI_Element_Hide(guid, false);
                filterGuids.Push(guid);
            }
        }
    }

    // 4. Выполняем экспорт IFC с настройкой "Видимые элементы"
    API_SavePars_Ifc savePars = {};
    savePars.subType = APISaveIfc_Export;
    savePars.file = new IO::Location(filePath);
    if (!translatorName.IsEmpty())
        savePars.translatorName = new GS::UniString(translatorName);
    savePars.exportVisibleOnly = true;   // ключевой параметр

    GSErrCode err = ACAPI_IFC_SaveProject(&savePars);

    // 5. Восстанавливаем видимость всех элементов
    for (const auto& g : allElemGuids)
        ACAPI_Element_Hide(g, false);

    delete savePars.file;
    delete savePars.translatorName;

    return err == NoError
        ? CreateSuccessfulExecutionResult()
        : CreateFailedExecutionResult(err, "Failed to export filtered IFC");
}
