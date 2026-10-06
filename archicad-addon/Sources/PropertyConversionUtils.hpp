#pragma once

// PropertyConversionUtils — реализация API_PropertyConversionUtilsInterface
// с дефолтными символами единиц (градус, минута, секунда, гон, радиан,
// стороны света) и метрическими единицами измерения (м, м², м³, градус).
//
// Используется при чтении/записи свойств Archicad:
//   ACAPI_Property_GetPropertyValueString (..., &resultString)
//   ACAPI_Property_SetPropertyValueFromString (value, utils, &propertyValue)
//
// Раньше класс был приватным внутри PropertyCommands.cpp; теперь вынесен
// сюда, чтобы его мог использовать и BulkCommands.cpp (BulkSetElementData,
// запись Archicad/<property-guid>).

#include "ACAPinc.h"

class PropertyConversionUtils : public API_PropertyConversionUtilsInterface
{
private:
    const GS::UniString degreeSymbol   = L ("\u00B0");
    const GS::UniString minuteSymbol   = "'";
    const GS::UniString secondSymbol   = "\"";
    const GS::UniString gradientSymbol = "G";
    const GS::UniString radianSymbol   = "R";
    const GS::UniString northSymbol    = "N";
    const GS::UniString southSymbol    = "S";
    const GS::UniString eastSymbol     = "E";
    const GS::UniString westSymbol     = "w";

public:
    PropertyConversionUtils () = default;
    virtual ~PropertyConversionUtils () = default;

    virtual const GS::UniString& GetDegreeSymbol1 () const { return degreeSymbol; }
    virtual const GS::UniString& GetDegreeSymbol2 () const { return degreeSymbol; }
    virtual const GS::UniString& GetMinuteSymbol () const { return minuteSymbol; }
    virtual const GS::UniString& GetSecondSymbol () const { return secondSymbol; }

    virtual const GS::UniString& GetGradientSymbol () const { return gradientSymbol; }
    virtual const GS::UniString& GetRadianSymbol () const { return radianSymbol; }

    virtual const GS::UniString& GetNorthSymbol () const { return northSymbol; }
    virtual const GS::UniString& GetSouthSymbol () const { return southSymbol; }
    virtual const GS::UniString& GetEastSymbol () const { return eastSymbol; }
    virtual const GS::UniString& GetWestSymbol () const { return westSymbol; }

    virtual GS::uchar_t GetDecimalDelimiterChar () const { return '.'; }
    virtual GS::Optional<GS::UniChar> GetThousandSeparatorChar () const { return ' '; }

    virtual API_LengthTypeID GetLengthType () const { return API_LengthTypeID::Meter; }
    virtual API_AreaTypeID GetAreaType () const { return API_AreaTypeID::SquareMeter; }
    virtual API_VolumeTypeID GetVolumeType () const { return API_VolumeTypeID::CubicMeter; }
    virtual API_AngleTypeID GetAngleType () const { return API_AngleTypeID::DecimalDegree; }
};
