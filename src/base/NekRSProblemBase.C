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

#include "NekRSProblemBase.h"
#include "Moose.h"
#include "AuxiliarySystem.h"
#include "TimeStepper.h"
#include "NekInterface.h"
#include "TimedPrint.h"
#include "MooseUtils.h"
#include "CardinalUtils.h"
#include "VariadicTable.h"
#include "UserErrorChecking.h"

#include "nekrs.hpp"
#include "nekInterface/nekInterfaceAdapter.hpp"

InputParameters
NekRSProblemBase::validParams()
{
  InputParameters params = ExternalProblem::validParams();
  params.addParam<std::string>("casename", "Case name for the NekRS input files; "
    "this is <case> in <case>.par, <case>.udf, <case>.oudf, and <case>.re2. "
    "Can also be provided on the command line with --nekrs-setup, which will override this setting");

  params.addParam<bool>("nondimensional", false, "Whether NekRS is solved in non-dimensional form");
  params.addRangeCheckedParam<Real>("U_ref", 1.0, "U_ref > 0.0", "Reference velocity value for non-dimensional solution");
  params.addRangeCheckedParam<Real>("T_ref", 0.0, "T_ref >= 0.0", "Reference temperature value for non-dimensional solution");
  params.addRangeCheckedParam<Real>("dT_ref", 1.0, "dT_ref > 0.0", "Reference temperature range value for non-dimensional solution");
  params.addRangeCheckedParam<Real>("L_ref", 1.0, "L_ref > 0.0", "Reference length scale value for non-dimensional solution");
  params.addRangeCheckedParam<Real>("rho_0", 1.0, "rho_0 > 0.0", "Density parameter value for non-dimensional solution");
  params.addRangeCheckedParam<Real>("Cp_0", 1.0, "Cp_0 > 0.0", "Heat capacity parameter value for non-dimensional solution");

  MultiMooseEnum nek_outputs("temperature pressure velocity");
  params.addParam<MultiMooseEnum>("output", nek_outputs, "Field(s) to output from NekRS onto the mesh mirror");

  params.addParam<bool>("write_fld_files", false, "Whether to write NekRS field file output "
    "from Cardinal. If true, this will disable any output writing by NekRS itself, and "
    "instead produce output files with names a01...a99pin, b01...b99pin, etc.");
  params.addParam<bool>("disable_fld_file_output", false, "Whether to turn off all NekRS field file output writing");

  params.addParam<bool>("minimize_transfers_in", false, "Whether to only synchronize nekRS "
    "for the direction TO_EXTERNAL_APP on multiapp synchronization steps");
  params.addParam<bool>("minimize_transfers_out", false, "Whether to only synchronize nekRS "
    "for the direction FROM_EXTERNAL_APP on multiapp synchronization steps");
  return params;
}

NekRSProblemBase::NekRSProblemBase(const InputParameters &params) : ExternalProblem(params),
  _nondimensional(getParam<bool>("nondimensional")),
  _U_ref(getParam<Real>("U_ref")),
  _T_ref(getParam<Real>("T_ref")),
  _dT_ref(getParam<Real>("dT_ref")),
  _L_ref(getParam<Real>("L_ref")),
  _rho_0(getParam<Real>("rho_0")),
  _Cp_0(getParam<Real>("Cp_0")),
  _write_fld_files(getParam<bool>("write_fld_files")),
  _disable_fld_file_output(getParam<bool>("disable_fld_file_output")),
  _minimize_transfers_in(getParam<bool>("minimize_transfers_in")),
  _minimize_transfers_out(getParam<bool>("minimize_transfers_out")),
  _start_time(nekrs::startTime())
{
  // the way the data transfers are detected depend on nekRS being a sub-application,
  // so these settings are not invalid if nekRS is the master app (though you could
  // relax this in the future by reversing the synchronization step identification
  // from the nekRS-subapp case to the nekRS-master app case - it's just not implemented yet).
  if (_app.isUltimateMaster())
    if (_minimize_transfers_in || _minimize_transfers_out)
      mooseError("The 'minimize_transfers_in' and 'minimize_transfers_out' capabilities "
        "require that nekRS is receiving and sending data to a master application, but "
        "in your case nekRS is the master application.");

  if (_disable_fld_file_output && _write_fld_files)
    mooseError("Cannot both disable all field file output and write custom field files!\n"
      "'write_fld_files' and 'disable_fld_file_output' cannot both be true!");

  if (_app.isUltimateMaster() && _write_fld_files)
    mooseError("The 'write_fld_files' setting should only be true when multiple Nek simulations "
      "are run as sub-apps on a master app.\nYour input has Nek as the master app.");

  _prefix = fieldFilePrefix(_app.multiAppNumber());

  // will be supported in the future, but it's just not implemented yet
  if (nekrs::hasCHT())
    mooseError("Cardinal does not yet support running NekRS inputs with conjugate heat transfer!");

  _nek_mesh = dynamic_cast<NekRSMesh*>(&mesh());

  if (!_nek_mesh)
    mooseError("Mesh for '" + type() + "' must be of type 'NekRSMesh', but you have specified a '" + mesh().type() + "'!");

  // the Problem constructor is called right after building the mesh. In order
  // to have pretty screen output without conflicting with the timed print messages,
  // print diagnostic info related to the mesh here. If running in JIT mode, this
  // diagnostic info was never set, so the numbers that would be printed are garbage.
  if (!nekrs::buildOnly())
    _nek_mesh->printMeshInfo();

  // if solving in nondimensional form, make sure that the user specified _all_ of the
  // necessary scaling quantities to prevent errors from forgetting one, which would take
  // a non-scaled default otherwise
  std::vector<std::string> scales = {"U_ref", "T_ref", "dT_ref", "L_ref", "rho_0", "Cp_0"};
  for (const auto & s : scales)
  {
    if (_nondimensional)
      checkRequiredParam(params, s, "solving in non-dimensional form");
    else
      checkUnusedParam(params, s, "solving in dimensional form");
  }

  // inform NekRS of the scaling that we are using if solving in non-dimensional form
  nekrs::solution::initializeDimensionalScales(_U_ref, _T_ref, _dT_ref, _L_ref, _rho_0, _Cp_0);

  if (_nondimensional)
  {
    VariadicTable<Real, Real, Real, Real> vt({"Length      ", "Velocity    ", "Temperature ", "d(Temperature)"});
    vt.setColumnFormat({VariadicTableColumnFormat::SCIENTIFIC,
      VariadicTableColumnFormat::SCIENTIFIC,
      VariadicTableColumnFormat::SCIENTIFIC,
      VariadicTableColumnFormat::SCIENTIFIC});

    vt.addRow(_L_ref, _U_ref, _T_ref, _dT_ref);
    _console << "\nNekRS characteristic scales:" << std::endl;
    vt.print(_console);
    _console << std::endl;
  }

  // It's too complicated to make sure that the dimensional form _also_ works when our
  // reference coordinates are different from what MOOSE is expecting, so just throw an error
  if (_nondimensional && !MooseUtils::absoluteFuzzyEqual(_nek_mesh->scaling(), _L_ref))
    paramError("L_ref", "When solving in non-dimensional form, no capability exists to allow "
      "a nondimensional solution based on reference scales that are not in the same units as the "
      "coupled MOOSE application!\n\nIf solving nekRS in nondimensional form, you must choose "
      "reference dimensional scales in the same units as expected by MOOSE, i.e. 'L_ref' "
      "must match 'scaling' in 'NekRSMesh'.");

  // boundary-specific data
  _boundary = _nek_mesh->boundary();
  _n_surface_elems = _nek_mesh->numSurfaceElems();
  _n_vertices_per_surface = _nek_mesh->numVerticesPerSurface();

  // volume-specific data
  _volume = _nek_mesh->volume();
  _n_volume_elems = _nek_mesh->numVolumeElems();
  _n_vertices_per_volume = _nek_mesh->numVerticesPerVolume();

  // generic data
  _n_elems = _nek_mesh->numElems();
  _n_vertices_per_elem = _nek_mesh->numVerticesPerElem();

  if (_volume)
    _n_points = _n_volume_elems * _n_vertices_per_volume;
  else
    _n_points = _n_surface_elems * _n_vertices_per_surface;

  initializeInterpolationMatrices();

  // we can save some effort for the low-order situations where the interpolation
  // matrix is the identity matrix (i.e. for which equi-spaced libMesh nodes are an
  // exact subset of the nekRS GLL points). This will happen for any first-order mesh,
  // and if a second-order mesh is used with a polynomial order of 2 in nekRS. Because
  // we pretty much always use a polynomial order greater than 2 in nekRS, let's just
  // check the first case because this will simplify our code in the nekrs::boundarySolution
  // function. If you change this line, you MUST change the innermost if/else statement
  // in nekrs::boundarySolution!
  _needs_interpolation = _nek_mesh->numQuadraturePoints1D() > 2;

  if (isParamValid("output"))
  {
    _outputs = &getParam<MultiMooseEnum>("output");
    _external_data = (double*) calloc(_n_points, sizeof(double));
  }
}

NekRSProblemBase::~NekRSProblemBase()
{
  // write nekRS solution to output if not already written for this step
  if (!_is_output_step && !_disable_fld_file_output)
  {
    if (_write_fld_files)
      nekrs::write_field_file(_prefix, _timestepper->nondimensionalDT(_time));
    else
      nekrs::outfld(_timestepper->nondimensionalDT(_time));
  }

  freePointer(_external_data);
  freePointer(_interpolation_outgoing);
  freePointer(_interpolation_incoming);
}

void
NekRSProblemBase::initializeInterpolationMatrices()
{
  mesh_t * mesh = nekrs::entireMesh();

  // determine the interpolation matrix for the outgoing transfer
  int starting_points = mesh->Nq;
  int ending_points = _nek_mesh->numQuadraturePoints1D();
  _interpolation_outgoing = (double *) calloc(starting_points * ending_points, sizeof(double));
  nekrs::interpolationMatrix(_interpolation_outgoing, starting_points, ending_points);

  // determine the interpolation matrix for the incoming transfer
  std::swap(starting_points, ending_points);
  _interpolation_incoming = (double *) calloc(starting_points * ending_points, sizeof(double));
  nekrs::interpolationMatrix(_interpolation_incoming, starting_points, ending_points);
}

std::string
NekRSProblemBase::fieldFilePrefix(const int & number) const
{
  const std::string alphabet = "abcdefghijklmnopqrstuvwxyz";
  int letter = number / 100;
  int remainder = number % 100;
  std::string s = remainder < 10 ? "0" : "";

  return alphabet[letter] + s + std::to_string(remainder);
}

void
NekRSProblemBase::fillAuxVariable(const unsigned int var_number, const double * value)
{
  auto & solution = _aux->solution();
  auto sys_number = _aux->number();
  auto pid = _communicator.rank();

  for (unsigned int e = 0; e < _n_elems; e++)
  {
    auto elem_ptr = _nek_mesh->queryElemPtr(e);

    // Only work on elements we can find on our local chunk of a
    // distributed mesh
    if (!elem_ptr)
    {
      libmesh_assert(!_nek_mesh->getMesh().is_serial());
      continue;
    }

    for (unsigned int n = 0; n < _n_vertices_per_elem; n++)
    {
      auto node_ptr = elem_ptr->node_ptr(n);

      // For each face vertex, we can only write into the MOOSE auxiliary fields if that
      // vertex is "owned" by the present MOOSE process.
      if (node_ptr->processor_id() == pid)
      {
        int node_index = _nek_mesh->nodeIndex(n);
        auto node_offset = e * _n_vertices_per_elem + node_index;

        // get the DOF for the auxiliary variable, then use it to set the value in the auxiliary system
        auto dof_idx = node_ptr->dof_number(sys_number, var_number, 0);
        solution.set(dof_idx, value[node_offset]);
      }
    }
  }

  solution.close();
}

void
NekRSProblemBase::initialSetup()
{
  ExternalProblem::initialSetup();

  auto executioner = _app.getExecutioner();
  _transient_executioner = dynamic_cast<Transient *>(executioner);

  // NekRS only supports transient simulations - therefore, it does not make
  // sense to use anything except a Transient-derived executioner
  if (!_transient_executioner)
    mooseError("A 'Transient' executioner must be used with '" + type() + "', but "
      "you have specified the '" + executioner->type() + "' executioner!");

  // If the simulation start time is not zero, the app's time must be shifted
  // relative to its master app (if any). Until this is implemented, make sure
  // a start time of zero is used.
  const auto moose_start_time = _transient_executioner->getStartTime();
  if (moose_start_time != 0.0)
    mooseError("You specified a start time of " + Moose::stringify(moose_start_time) +
      ", but a non-zero start time is not yet supported for '" + type() + "'!");

  // To get the correct time stepping information on the MOOSE side, we also
  // must use the NekTimeStepper
  TimeStepper * stepper = _transient_executioner->getTimeStepper();
  _timestepper = dynamic_cast<NekTimeStepper *>(stepper);
  if (!_timestepper)
    mooseError("The 'NekTimeStepper' stepper must be used with '" + type() + "', "
      "but you have specified the '" + stepper->type() + "' time stepper!");

  // Set the reference time for use in dimensionalizing/non-dimensionalizing the time
  _timestepper->setReferenceTime(_L_ref, _U_ref);

  // Also make sure that the start time is consistent with what MOOSE wants to use.
  // If different from what NekRS internally wants to use, use the MOOSE value.
  if (!MooseUtils::absoluteFuzzyEqual(moose_start_time, _start_time))
  {
    mooseWarning("The start time set on the executioner: " + Moose::stringify(moose_start_time) +
      " does not match the start time set in NekRS's .par file: " + Moose::stringify(_timestepper->dimensionalDT(_start_time)) + ". "
      "\nThis may happen if you are using a restart file in NekRS.\n\n" +
      "Setting start time for '" + type() + "' to: " + Moose::stringify(moose_start_time));
    _start_time = moose_start_time;
  }

  if (_minimize_transfers_in)
    _transfer_in = &getPostprocessorValueByName("transfer_in");

  // Then, dimensionalize the NekRS time so that all occurrences of _dt here are
  // in dimensional form
  _timestepper->dimensionalizeDT();

  // nekRS calls UDF_ExecuteStep once before the time stepping begins
  nekrs::udfExecuteStep(_start_time, _t_step, false /* not an output step */);
}

void NekRSProblemBase::externalSolve()
{
  if (nekrs::buildOnly())
    return;

  // _dt reflects the time step that MOOSE wants Nek to
  // take. For instance, if Nek is controlled by a master app and subcycling is used,
  // Nek must advance to the time interval taken by the master app. If the time step
  // that MOOSE wants nekRS to take (i.e. _dt) is smaller than we'd like nekRS to take, error.
  if (_dt < _timestepper->minDT())
    mooseError("Requested time step of " + std::to_string(_dt) + " is smaller than the minimum "
      "time step of " + Moose::stringify(_timestepper->minDT()) + " allowed in NekRS!\n\n"
      "You can control this behavior with the 'min_dt' parameter on 'NekTimeStepper'.");

  // _time represents the time that we're simulating _to_, but we need to pass sometimes slightly different
  // times into the nekRS routines, which assume that the "time" passed into their
  // routines is sometimes a different interpretation.
  double step_start_time = _time - _dt;
  double step_end_time = _time;

  // Run a nekRS time step. After the time step, this also calls UDF_ExecuteStep,
  // evaluated at (step_end_time, _t_step)
  nekrs::runStep(_timestepper->nondimensionalDT(step_start_time),
    _timestepper->nondimensionalDT(_dt), _t_step);

  // optional entry point to adjust the recently-computed NekRS solution
  adjustNekSolution();

  // Note: here, we copy to both the nrs solution arrays and to the Nek5000 backend arrays,
  // because it is possible that users may interact using the legacy usr-file approach.
  // If we move away from the Nek5000 backend entirely, we could replace this line with
  // direct OCCA memcpy calls. But we do definitely need some type of copy here for _every_
  // time step, even if we're not technically passing data to another app, because we have
  // postprocessors that touch the `nrs` arrays that can be called in an arbitrary fashion
  // by the user.
  nek::ocopyToNek(_timestepper->nondimensionalDT(step_end_time), _t_step);

  _is_output_step = isOutputStep();

  if (_is_output_step && !_disable_fld_file_output)
  {
    if (_write_fld_files)
      nekrs::write_field_file(_prefix, _timestepper->nondimensionalDT(step_end_time));
    else
      nekrs::outfld(_timestepper->nondimensionalDT(step_end_time));
  }

  _time += _dt;
}

void
NekRSProblemBase::syncSolutions(ExternalProblem::Direction direction)
{
  if (nekrs::buildOnly())
    return;

  switch (direction)
  {
    case ExternalProblem::Direction::TO_EXTERNAL_APP:
    {
      if (!synchronizeIn())
        return;

      return;
    }
    case ExternalProblem::Direction::FROM_EXTERNAL_APP:
    {
      if (!synchronizeOut())
        return;

      // extract the NekRS solution onto the mesh mirror, if specified
      extractOutputs();
      return;
    }
    default:
      mooseError("Unhandled Transfer::DIRECTION enum!");
  }
}

bool
NekRSProblemBase::synchronizeIn()
{
  bool synchronize = true;
  static bool first = true;

  if (_minimize_transfers_in)
  {
    // For the minimized incoming synchronization to work correctly, the value
    // of the incoming postprocessor must not be zero. We only need to check this for the very
    // first time we evaluate this function. This ensures that you don't accidentally set a
    // zero value as a default in the master application's postprocessor.
    if (first && *_transfer_in == false)
      mooseError("The default value for the 'transfer_in' postprocessor received by nekRS "
        "must not be false! Make sure that the master application's "
        "postprocessor is not zero.");

    if (*_transfer_in == false)
      synchronize = false;
    else
      setPostprocessorValueByName("transfer_in", false, 0);
  }

  first = false;
  return synchronize;
}

bool
NekRSProblemBase::synchronizeOut()
{
  bool synchronize = true;

  if (_minimize_transfers_out)
  {
    if (std::abs(_time - _dt - _transient_executioner->getTargetTime()) > _transient_executioner->timestepTol())
      synchronize = false;
  }

  return synchronize;
}

bool
NekRSProblemBase::isOutputStep() const
{
  if (_app.isUltimateMaster())
  {
    bool last_step = nekrs::lastStep(_timestepper->nondimensionalDT(_time), _t_step, 0.0 /* dummy elapsed time */);

    // if Nek is controlled by a master application, then the last time step
    // is controlled by that master application, in which case we don't want to
    // write at what nekRS thinks is the last step (since it may or may not be
    // the actual end step), especially because we already ensure that we write on the
    // last time step from MOOSE's perspective in NekRSProblem's destructor.
    if (last_step)
      return true;
  }

  // this routine does not check if we are on the last step - just whether we have
  // met the requested runtime or time step interval
  return nekrs::outputStep(_timestepper->nondimensionalDT(_time), _t_step);
}

void
NekRSProblemBase::extractOutputs()
{
  if (_outputs && _var_names.size())
  {
    _console << "Interpolating" + _var_string + " NekRS solution onto mesh mirror" << std::endl;

    for (std::size_t i = 0; i < _var_names.size(); ++i)
    {
      field::NekFieldEnum field_enum;

      if (_var_names[i] == "temp")
        field_enum = field::temperature;
      else if (_var_names[i] == "P")
        field_enum = field::pressure;
      else if (_var_names[i] == "vel_x")
        field_enum = field::velocity_x;
      else if (_var_names[i] == "vel_y")
        field_enum = field::velocity_y;
      else if (_var_names[i] == "vel_z")
        field_enum = field::velocity_z;
      else
        mooseError("Unhandled NekFieldEnum in NekRSProblemBase!");

      if (!_volume)
        boundarySolution(field_enum, _external_data);

      if (_volume)
        volumeSolution(field_enum, _external_data);

      fillAuxVariable(_external_vars[i], _external_data);
    }
  }
}

InputParameters
NekRSProblemBase::getExternalVariableParameters()
{
  InputParameters var_params = _factory.getValidParams("MooseVariable");
  var_params.set<MooseEnum>("family") = "LAGRANGE";

  switch (_nek_mesh->order())
  {
    case order::first:
      var_params.set<MooseEnum>("order") = "FIRST";
      break;
    case order::second:
      var_params.set<MooseEnum>("order") = "SECOND";
      break;
    default:
      mooseError("Unhandled 'NekOrderEnum' in 'NekRSProblemBase'!");
  }

  return var_params;
}

void
NekRSProblemBase::addTemperatureVariable()
{
  if (!nekrs::hasTemperatureVariable())
    mooseError("Cannot set 'output = temperature' for '" + type() + "' because "
      "your Nek case files do not have a temperature variable!");

  // For the special case of temperature, we want the variable name to be
  // 'temp' instead of 'temperature' due to legacy reasons of what NekRSProblem
  // chose to name the temperature variable. For everything else, we just use
  // the name of the output parameter.
  _var_names.push_back("temp");
}

void
NekRSProblemBase::addExternalVariables()
{
  if (_outputs)
  {
    auto var_params = getExternalVariableParameters();

    for (std::size_t i = 0; i < _outputs->size(); ++i)
    {
      std::string output = (*_outputs)[i];

      if (output == "temperature")
        addTemperatureVariable();
      else if (output == "velocity")
      {
        // For the velocity, we need to explicitly output each component; Paraview
        // will then combine the components together into a vector
        _var_names.push_back("vel_x");
        _var_names.push_back("vel_y");
        _var_names.push_back("vel_z");
      }
      else if (output == "pressure")
        _var_names.push_back("P");
    }

    _var_string = "";
    for (const auto & name : _var_names)
    {
      addAuxVariable("MooseVariable", name, var_params);
      _external_vars.push_back(_aux->getFieldVariable<Real>(0, name).number());

      _var_string += " " + name + ",";
    }

    _var_string.erase(std::prev(_var_string.end()));
  }

  if (_minimize_transfers_in)
  {
    auto pp_params = _factory.getValidParams("Receiver");
    pp_params.set<std::vector<OutputName>>("outputs") = {"none"};
    addPostprocessor("Receiver", "transfer_in", pp_params);
  }
}

void
NekRSProblemBase::volumeSolution(const field::NekFieldEnum & field, double * T)
{
  mesh_t* mesh = nekrs::entireMesh();
  auto vc = _nek_mesh->volumeCoupling();

  double (*f) (int);
  f = nekrs::solution::solutionPointer(field);

  int start_1d = mesh->Nq;
  int end_1d = _nek_mesh->order() + 2;
  int start_3d = start_1d * start_1d * start_1d;
  int end_3d = end_1d * end_1d * end_1d;

  // allocate temporary space to hold the results of the search for each process
  double* Ttmp = (double*) calloc(vc.n_elems * end_3d, sizeof(double));
  double* Telem = (double*) calloc(start_3d, sizeof(double));

  // if we apply the shortcut for first-order interpolations, just hard-code those
  // indices that we'll grab for a volume hex element
  int start_2d = start_1d * start_1d;
  int indices [] = {0, start_1d - 1, start_2d - start_1d, start_2d - 1,
                    start_3d - start_2d, start_3d - start_2d + start_1d - 1, start_3d - start_1d, start_3d - 1};

  int c = 0;
  for (int k = 0; k < mesh->Nelements; ++k)
  {
    int offset = k * start_3d;

    if (_needs_interpolation)
    {
      // get the solution on the element
      for (int v = 0; v < start_3d; ++v)
        Telem[v] = f(offset + v);

      // and then interpolate it
      nekrs::interpolateVolumeHex3D(_interpolation_outgoing, Telem, start_1d, &(Ttmp[c]), end_1d);
      c += end_3d;
    }
    else
    {
      // get the solution on the element - no need to interpolate
      for (int v = 0; v < end_3d; ++v, ++c)
        Ttmp[c] = f(offset + indices[v]);
    }
  }

  // dimensionalize the solution if needed
  int Nlocal = vc.n_elems * end_3d;
  for (int v = 0; v < Nlocal; ++v)
  {
    nekrs::solution::dimensionalize(field, Ttmp[v]);

    // if temperature, we need to add the reference temperature
    if (field == field::temperature)
      Ttmp[v] += _T_ref;
  }

  nekrs::allgatherv(vc.counts, Ttmp, T, end_3d);

  freePointer(Ttmp);
  freePointer(Telem);
}

void
NekRSProblemBase::boundarySolution(const field::NekFieldEnum & field, double * T)
{
  mesh_t* mesh = nekrs::entireMesh();

  auto bc = _nek_mesh->boundaryCoupling();

  double (*f) (int);
  f = nekrs::solution::solutionPointer(field);

  int start_1d = mesh->Nq;
  int end_1d = _nek_mesh->order() + 2;
  int start_2d = start_1d * start_1d;
  int end_2d = end_1d * end_1d;

  // allocate temporary space:
  // - Ttmp: results of the search for each process
  // - Tface: scratch space for face solution to avoid reallocating a bunch
  // - scratch: scratch for the interpolatino process to avoid reallocating a bunch
  double* Ttmp = (double*) calloc(bc.n_faces * end_2d, sizeof(double));
  double* Tface = (double*) calloc(start_2d, sizeof(double));
  double* scratch = (double*) calloc(start_1d * end_1d, sizeof(double));

  // if we apply the shortcut for first-order interpolations, just hard-code those
  // indices that we'll grab for a surface hex element
  int indices [] = {0, start_1d - 1, start_2d - start_1d, start_2d - 1};

  int c = 0;
  for (int k = 0; k < bc.total_n_faces; ++k)
  {
    if (bc.process[k] == nekrs::commRank())
    {
      int i = bc.element[k];
      int j = bc.face[k];
      int offset = i * mesh->Nfaces * start_2d + j * start_2d;

      if (_needs_interpolation)
      {
        // get the solution on the face
        for (int v = 0; v < start_2d; ++v)
        {
          int id = mesh->vmapM[offset + v];
          Tface[v] = f(id);
        }

        // and then interpolate it
        nekrs::interpolateSurfaceFaceHex3D(scratch, _interpolation_outgoing, Tface, start_1d, &(Ttmp[c]), end_1d);
        c += end_2d;
      }
      else
      {
        // get the solution on the face - no need to interpolate
        for (int v = 0; v < end_2d; ++v, ++c)
        {
          int id = mesh->vmapM[offset + indices[v]];
          Ttmp[c] = f(id);
        }
      }
    }
  }

  // dimensionalize the solution if needed
  int Nlocal = bc.n_faces * end_2d;
  for (int v = 0; v < Nlocal; ++v)
  {
    nekrs::solution::dimensionalize(field, Ttmp[v]);

    // if temperature, we need to add the reference temperature
    if (field == field::temperature)
      Ttmp[v] += _T_ref;
  }

  nekrs::allgatherv(bc.counts, Ttmp, T, end_2d);

  freePointer(Ttmp);
  freePointer(Tface);
  freePointer(scratch);
}

void
NekRSProblemBase::writeVolumeSolution(const int elem_id, const field::NekWriteEnum & field, double * T,
  const std::vector<double> * add)
{
  auto vc = _nek_mesh->volumeCoupling();

  // We can only write into the nekRS scratch space if that face is "owned" by the current process
  if (nekrs::commRank() == vc.processor_id(elem_id))
  {
    mesh_t * mesh = nekrs::entireMesh();
    void (*write_solution) (int, dfloat);
    write_solution = nekrs::solution::solutionPointer(field);

    int end_1d = mesh->Nq;
    int start_1d = _nek_mesh->order() + 2;

    int e = vc.element[elem_id];
    double * tmp = (double*) calloc(mesh->Np, sizeof(double));

    nekrs::interpolateVolumeHex3D(_interpolation_incoming, T, start_1d, tmp, end_1d);

    int id = e * mesh->Np;
    for (int v = 0; v < mesh->Np; ++v)
    {
      double extra = (add == nullptr) ? 0.0 : (*add)[id + v];
      write_solution(id + v, tmp[v] + extra);
    }

    freePointer(tmp);
  }
}
