#include "ElementRotateCommands.hpp"
#include "MigrationHelper.hpp"
#include <cmath>

RotateElementsCommand::RotateElementsCommand () :
    CommandBase (CommonSchema::Used)
{}

GS::String RotateElementsCommand::GetName () const
{
    return "RotateElements";
}

GS::Optional<GS::UniString> RotateElementsCommand::GetInputParametersSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "elementsWithRotations": {
                "type": "array",
                "description": "The elements with rotation parameters.",
                "items": {
                    "type": "object",
                    "properties": {
                        "elementId": { "$ref": "#/ElementId" },
                        "rotationAngle": {
                            "type": "number",
                            "description": "Rotation angle in radians, counterclockwise."
                        },
                        "rotationCenter": {
                            "type": "object",
                            "description": "Center of rotation. If omitted, the element AABB center is used.",
                            "properties": {
                                "x": { "type": "number" },
                                "y": { "type": "number" }
                            },
                            "additionalProperties": false,
                            "required": [ "x", "y" ]
                        },
                        "withCopy": {
                            "type": "boolean",
                            "description": "If true, a copy of the element is rotated (original is kept). Default false."
                        }
                    },
                    "additionalProperties": false,
                    "required": [ "elementId", "rotationAngle" ]
                }
            }
        },
        "additionalProperties": false,
        "required": [ "elementsWithRotations" ]
    })";
}

GS::Optional<GS::UniString> RotateElementsCommand::GetResponseSchema () const
{
    return R"({
        "type": "object",
        "properties": {
            "executionResults": { "$ref": "#/ExecutionResults" }
        },
        "additionalProperties": false,
        "required": [ "executionResults" ]
    })";
}

static GSErrCode RotateSingleElement (const API_Guid& elemGuid,
                                      double           angleRad,
                                      bool             hasCenter,
                                      const API_Coord& centerIn,
                                      bool             withCopy)
{
    API_Coord orig = centerIn;
    if (!hasCenter) {
        API_Elem_Head head = {};
        head.guid = elemGuid;
        API_Box3D box = {};
        GSErrCode boundsErr = ACAPI_Element_CalcBounds (&head, &box);
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

GS::ObjectState RotateElementsCommand::Execute (const GS::ObjectState& parameters,
                                                GS::ProcessControl& /*processControl*/) const
{
    GS::Array<GS::ObjectState> elementsWithRotations;
    parameters.Get ("elementsWithRotations", elementsWithRotations);

    GS::ObjectState response;
    const auto& executionResults = response.AddList<GS::ObjectState> ("executionResults");

    ACAPI_CallUndoableCommand ("RotateElementsCommand", [&]() -> GSErrCode {
        for (const GS::ObjectState& e : elementsWithRotations) {
            const GS::ObjectState* elementId = e.Get ("elementId");
            if (elementId == nullptr) {
                executionResults (CreateFailedExecutionResult (APIERR_BADPARS, "elementId is missing"));
                continue;
            }

            double angle = 0.0;
            if (!e.Get ("rotationAngle", angle)) {
                executionResults (CreateFailedExecutionResult (APIERR_BADPARS, "rotationAngle is missing"));
                continue;
            }

            bool hasCenter = false;
            API_Coord center = {};
            const GS::ObjectState* centerOS = e.Get ("rotationCenter");
            if (centerOS != nullptr) {
                centerOS->Get ("x", center.x);
                centerOS->Get ("y", center.y);
                hasCenter = true;
            }

            bool withCopy = false;
            e.Get ("withCopy", withCopy);

            const API_Guid guid = GetGuidFromObjectState (*elementId);
            const GSErrCode err = RotateSingleElement (guid, angle, hasCenter, center, withCopy);

            if (err != NoError) {
                executionResults (CreateFailedExecutionResult (
                    err,
                    GS::UniString::Printf ("Failed to rotate element %T",
                                           APIGuidToString (guid).ToPrintf ())));
            } else {
                executionResults (CreateSuccessfulExecutionResult ());
            }
        }
        return NoError;
    });

    return response;
}
