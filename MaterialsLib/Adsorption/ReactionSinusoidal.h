/**
 * \copyright
 * Copyright (c) 2012-2016, OpenGeoSys Community (http://www.opengeosys.org)
 *            Distributed under a Modified BSD License.
 *              See accompanying file LICENSE.txt or
 *              http://www.opengeosys.org/project/license
 *
 */

#pragma once

#include <logog/include/logog.hpp>

#include "Reaction.h"
#include "BaseLib/ConfigTree.h"

namespace Adsorption
{

class ReactionSinusoidal final : public Reaction
{
public:
    explicit ReactionSinusoidal(BaseLib::ConfigTree const& conf)
        : _enthalpy(conf.getConfParam<double>("reaction_enthalpy"))
    {
    }

    double getEnthalpy(const double /*p_Ads*/, const double /*T_Ads*/,
                        const double /*M_Ads*/) const override
    {
        return _enthalpy;
    }

    double getReactionRate(const double /*p_Ads*/, const double /*T_Ads*/, const double /*M_Ads*/,
                             const double /*loading*/) const override
    {
        ERR("Method getReactionRate() should never be called directly");
        std::abort();
        return 0.0;
    }

private:
    double _enthalpy;
};

}
