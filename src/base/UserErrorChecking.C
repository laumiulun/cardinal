#include "UserErrorChecking.h"
#include "CardinalApp.h"

void
checkUnusedParam(const InputParameters & p, const std::string & name, const std::string & explanation)
{
  if (p.isParamSetByUser(name))
    mooseWarning("When " + explanation + ", the '" + name + "' parameter is unused!");
}

void
checkRequiredParam(const InputParameters & p, const std::string & name, const std::string & explanation)
{
  if (!p.isParamValid(name))
    mooseError("When " + explanation + ", the '" + name + "' parameter is required!");
}

void
checkJointParams(const InputParameters & p, const std::vector<std::string> & name, const std::string & explanation)
{
  bool at_least_one_present = false;
  bool at_least_one_not_present = false;
  std::string name_list = "";

  for (const auto & s : name)
  {
    name_list += " '" + s + "',";

    if (p.isParamValid(s))
      at_least_one_present = true;
    else
      at_least_one_not_present = true;
  }

  if (at_least_one_present && at_least_one_not_present)
  {
    name_list.pop_back();
    mooseError("When " + explanation + ", the" + name_list + " parameters\nmust either ALL "
      "be specified or ALL omitted; you have only provided a subset of parameters!");
  }
}
