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

#pragma once

#include "NekPlaneSpatialBinUserObject.h"

/**
 * Compute a side integral of the NekRS solution in spatial bins.
 */
class NekBinnedPlaneIntegral : public NekPlaneSpatialBinUserObject
{
public:
  static InputParameters validParams();

  NekBinnedPlaneIntegral(const InputParameters & parameters);

  virtual void execute() override;

  virtual void getBinVolumes() override;

  Real spatialValue(const Point & p, const unsigned int & component) const override;

  /**
   * Compute the integral over the side bins
   * @param[in] integrand field to integrate
   * @param[out] total_integral integral over each bin
   */
  virtual void binnedPlaneIntegral(const field::NekFieldEnum & integrand, double * total_integral);

  /**
   * Compute the integrals
   */
  virtual void computeIntegral();
};
