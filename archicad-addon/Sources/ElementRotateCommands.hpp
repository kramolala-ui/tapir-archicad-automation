#ifndef ELEMENT_ROTATE_COMMANDS_HPP
#define ELEMENT_ROTATE_COMMANDS_HPP

#include "CommandBase.hpp"

class RotateElementsCommand : public CommandBase {
public:
    RotateElementsCommand ();

    virtual GS::String GetName () const override;
    virtual GS::Optional<GS::UniString> GetInputParametersSchema () const override;
    virtual GS::Optional<GS::UniString> GetResponseSchema () const override;
    virtual GS::ObjectState Execute (const GS::ObjectState& parameters,
                                     GS::ProcessControl& processControl) const override;
};

#endif
