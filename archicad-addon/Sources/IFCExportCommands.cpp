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

    API_SavePars_Ifc savePars = {};
    savePars.subType = APISaveIfc_Export;
    savePars.file = new IO::Location(filePath);
    if (!translatorName.IsEmpty())
        savePars.translatorName = new GS::UniString(translatorName);

    // Фильтр: экспортируем только указанные GUID
    GS::Array<API_Guid> filterGuids;
    for (const auto& item : elementGuids) {
        GS::String guidStr;
        if (item.Get("guid", guidStr))
            filterGuids.Push(APIGuidFromString(guidStr.ToCStr()));
    }
    if (!filterGuids.IsEmpty()) {
        savePars.filter = new API_IfcExportFilter;
        savePars.filter->elements = filterGuids;
    }

    GSErrCode err = ACAPI_IFC_SaveProject(&savePars);
    delete savePars.file;
    delete savePars.translatorName;
    delete savePars.filter;

    return err == NoError
        ? CreateSuccessfulExecutionResult()
        : CreateFailedExecutionResult(err, "Failed to export IFC with filter");
}