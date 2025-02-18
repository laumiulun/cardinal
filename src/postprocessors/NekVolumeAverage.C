/********************************************************************/
/*                  SOFTWARE COPYRIGHT NOTIFICATION                 */
/*                             Cardinal                             */
/*                                                                  */
/*                  (c) 2021 UChicago Argonne, LLC                  */
/*                        ALL RIGHTS RESERVED                       */
/*                                                                  */
/*                 Prepared by UChicago Argonne, LLC                */
/*               Under Contract No. DE-AC02-06CH11357               */
/*                With the U. S. Department of Energy               */
/*                                                                  */
/*             Prepared by Battelle Energy Alliance, LLC            */
/*               Under Contract No. DE-AC07-05ID14517               */
/*                With the U. S. Department of Energy               */
/*                                                                  */
/*                 See LICENSE for full restrictions                */
/********************************************************************/

#include "NekVolumeAverage.h"

registerMooseObject("CardinalApp", NekVolumeAverage);

InputParameters
NekVolumeAverage::validParams()
{
  InputParameters params = NekVolumeIntegral::validParams();
  params.addClassDescription("Compute a volume average of a specified field over the NekRS mesh");
  return params;
}

NekVolumeAverage::NekVolumeAverage(const InputParameters & parameters) :
  NekVolumeIntegral(parameters)
{
}

Real
NekVolumeAverage::getValue()
{
  return NekVolumeIntegral::getValue() / _volume;
}
