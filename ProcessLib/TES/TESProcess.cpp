/**
 * \copyright
 * Copyright (c) 2012-2016, OpenGeoSys Community (http://www.opengeosys.org)
 *            Distributed under a Modified BSD License.
 *              See accompanying file LICENSE.txt or
 *              http://www.opengeosys.org/project/license
 *
 */

#include "TESProcess.h"

// TODO that essentially duplicates code which is also present in ProcessOutput.
template<typename GlobalVector>
double
getNodalValue(GlobalVector const& x, MeshLib::Mesh const& mesh,
              AssemblerLib::LocalToGlobalIndexMap const& dof_table,
              std::size_t const node_id, std::size_t const global_component_id)
{
    MeshLib::Location const l{
        mesh.getID(), MeshLib::MeshItemType::Node, node_id};

    auto const index = dof_table.getLocalIndex(
        l, global_component_id, x.getRangeBegin(), x.getRangeEnd());

    return x.get(index);
}


namespace ProcessLib
{

namespace TES
{

template<typename GlobalSetup>
TESProcess<GlobalSetup>::
TESProcess(MeshLib::Mesh& mesh,
           typename Process<GlobalSetup>::NonlinearSolver& nonlinear_solver,
           std::unique_ptr<typename Process<GlobalSetup>::TimeDiscretization>&& time_discretization,
           std::vector<std::reference_wrapper<ProcessVariable>>&& process_variables,
           SecondaryVariableCollection<GlobalVector>&& secondary_variables,
           ProcessOutput<GlobalVector>&& process_output,
           const BaseLib::ConfigTree& config)
    : Process<GlobalSetup>{mesh, nonlinear_solver, std::move(time_discretization),
                           std::move(process_variables),
                           std::move(secondary_variables),
                           std::move(process_output)}
{
    DBUG("Create TESProcess.");

    // physical parameters for local assembly
    {
        std::vector<std::pair<const std::string, double*> > params{
            { "fluid_specific_heat_source",            &_assembly_params.fluid_specific_heat_source },
            { "fluid_specific_isobaric_heat_capacity", &_assembly_params.cpG },
            { "solid_specific_heat_source",            &_assembly_params.solid_specific_heat_source },
            { "solid_heat_conductivity",               &_assembly_params.solid_heat_cond },
            { "solid_specific_isobaric_heat_capacity", &_assembly_params.cpS },
            { "tortuosity",                            &_assembly_params.tortuosity },
            { "diffusion_coefficient",                 &_assembly_params.diffusion_coefficient_component },
            { "porosity",                              &_assembly_params.poro },
            { "solid_density_dry",                     &_assembly_params.rho_SR_dry },
            { "solid_density_initial",                 &_assembly_params.initial_solid_density }
        };

        for (auto const& p : params)
        {
            if (auto const par = config.getConfParamOptional<double>(p.first)) {
                DBUG("setting parameter `%s' to value `%g'", p.first.c_str(), *par);
                *p.second = *par;
            }
        }
    }

    // characteristic values of primary variables
    {
        std::vector<std::pair<const std::string, Trafo*> > const params{
            { "characteristic_pressure",             &_assembly_params.trafo_p },
            { "characteristic_temperature",          &_assembly_params.trafo_T },
            { "characteristic_vapour_mass_fraction", &_assembly_params.trafo_x }
        };

        for (auto const& p : params)
        {
            if (auto const par = config.getConfParamOptional<double>(p.first)) {
                INFO("setting parameter `%s' to value `%g'", p.first.c_str(), *par);
                *p.second = Trafo{*par};
            }
        }
    }

    // permeability
    if (auto par = config.getConfParamOptional<double>("solid_hydraulic_permeability"))
    {
        DBUG("setting parameter `solid_hydraulic_permeability' to isotropic value `%g'", *par);
        const auto dim = mesh.getDimension();
        _assembly_params.solid_perm_tensor
                = Eigen::MatrixXd::Identity(dim, dim) * (*par);
    }

    // reactive system
    _assembly_params.react_sys =
        Adsorption::AdsorptionReaction::newInstance(config.getConfSubtree("reactive_system"));


    // debug output
    if (auto const param = config.getConfParamOptional<bool>("output_element_matrices"))
    {
        DBUG("output_element_matrices: %s", (*param) ? "true" : "false");

        _assembly_params.output_element_matrices = *param;
    }

    // TODO move to process output
    /*
    // debug output
    if (auto const param = config.getConfParamOptional<bool>("output_iteration_results"))
    {
        DBUG("output_iteration_results: %s", (*param) ? "true" : "false");

        BP::_process_output.output_iteration_results = *param;
    }

    // debug output
    if (auto const param = config.getConfParamOptional<bool>("output_global_matrix"))
    {
        DBUG("output_global_matrix: %s", (*param) ? "true" : "false");

        BP::_process_output.output_global_matrix = *param;
    }

    {
        // TODO Why is the full DOF table not built with order anymore?
        auto getOrder = [&config]() -> AssemblerLib::ComponentOrder
        {
            auto const order = config.getConfParam<std::string>("global_matrix_order");
            DBUG("global_matrix_order: %s", order.c_str());

            // TODO for single component order does not matter, right?
            if (order == "BY_COMPONENT")
                return AssemblerLib::ComponentOrder::BY_COMPONENT;
            else if (order == "BY_LOCATION")
                return AssemblerLib::ComponentOrder::BY_LOCATION;
            else {
                ERR("unknown global matrix order `%s'", order.c_str());
                std::abort();
            }
        };
    }
    */
}

template<typename GlobalSetup>
void TESProcess<GlobalSetup>::
initializeConcreteProcess(
    AssemblerLib::LocalToGlobalIndexMap const& dof_table,
    MeshLib::Mesh const& mesh, unsigned const integration_order)
{
    DBUG("Create global assembler.");
    _global_assembler.reset(new GlobalAssembler(dof_table));

    ProcessLib::createLocalAssemblers<GlobalSetup, TESLocalAssembler>(
                mesh.getDimension(), mesh.getElements(),
                dof_table, integration_order, _local_assemblers,
                _assembly_params);

    // TODO move the two data members somewhere else.
    // for extrapolation of secondary variables
    std::vector<std::unique_ptr<MeshLib::MeshSubsets>> all_mesh_subsets_single_component;
    all_mesh_subsets_single_component.emplace_back(
                new MeshLib::MeshSubsets(BP::_mesh_subset_all_nodes.get()));
    _local_to_global_index_map_single_component.reset(
                new AssemblerLib::LocalToGlobalIndexMap(
                    std::move(all_mesh_subsets_single_component),
                    // by location order is needed for output
                    AssemblerLib::ComponentOrder::BY_LOCATION)
                );

    _extrapolator.reset(new ExtrapolatorImplementation(
        { 0u, 0u, nullptr, _local_to_global_index_map_single_component.get(), &mesh }));

    // secondary variables
    auto add2nd = [&](
        std::string const& var_name, unsigned const n_components,
        SecondaryVariableFunctions<GlobalVector>&& fcts)
    {
        BP::_secondary_variables.addSecondaryVariable(
                    var_name, n_components, std::move(fcts));
    };
    auto makeEx = [&](TESIntPtVariables var)
    {
        return ProcessLib::makeExtrapolator(var, *_extrapolator, _local_assemblers);
    };

    add2nd("solid_density",  1, makeEx(TESIntPtVariables::SOLID_DENSITY));
    add2nd("reaction_rate",  1, makeEx(TESIntPtVariables::REACTION_RATE));
    add2nd("velocity_x",     1, makeEx(TESIntPtVariables::VELOCITY_X));
    if (mesh.getDimension() >= 2)
        add2nd("velocity_y", 1, makeEx(TESIntPtVariables::VELOCITY_Y));
    if (mesh.getDimension() >= 3)
        add2nd("velocity_z", 1, makeEx(TESIntPtVariables::VELOCITY_Z));

    add2nd("loading",        1, makeEx(TESIntPtVariables::LOADING));
    add2nd("reaction_damping_factor",
                             1, makeEx(TESIntPtVariables::REACTION_DAMPING_FACTOR));

    namespace PH = std::placeholders;
    using Self = TESProcess<GlobalSetup>;

    add2nd("vapour_partial_pressure", 1,
        {std::bind(&Self::computeVapourPartialPressure, this, PH::_1, PH::_2, PH::_3),
         nullptr});
    add2nd("relative_humidity",       1,
        {std::bind(&Self::computeRelativeHumidity,      this, PH::_1, PH::_2, PH::_3),
         nullptr});
    add2nd("equilibrium_loading",     1,
        {std::bind(&Self::computeEquilibriumLoading,    this, PH::_1, PH::_2, PH::_3),
         nullptr});
}

template<typename GlobalSetup>
void TESProcess<GlobalSetup>::
assembleConcreteProcess(
        const double t, GlobalVector const& x,
        GlobalMatrix& M, GlobalMatrix& K, GlobalVector& b)
{
    DBUG("Assemble TESProcess.");

    // Call global assembler for each local assembly item.
    GlobalSetup::executeMemberDereferenced(
                *_global_assembler, &GlobalAssembler::assemble,
                _local_assemblers, t, x, M, K, b);

#ifndef NDEBUG
    if (_total_iteration == 0)
    {
        MathLib::BLAS::finalizeAssembly(M);
        MathLib::BLAS::finalizeAssembly(K);
        MathLib::BLAS::finalizeAssembly(b);

        // TODO [CL] Those files will be written to the working directory.
        //           Relative path needed.
        M.write("global_matrix_M.txt");
        K.write("global_matrix_K.txt");
        b.write("global_vector_b.txt");
    }
#endif
}

template<typename GlobalSetup>
void
TESProcess<GlobalSetup>::
preTimestep(GlobalVector const& /*x*/, const double t, const double delta_t)
{
    DBUG("new timestep");

    _assembly_params.delta_t = delta_t;
    _assembly_params.current_time = t;
    ++ _timestep; // TODO remove that
}

template<typename GlobalSetup>
void
TESProcess<GlobalSetup>::
preIteration(const unsigned iter, GlobalVector const& /*x*/)
{
    _assembly_params.iteration_in_current_timestep = iter;
}

template<typename GlobalSetup>
NumLib::IterationResult
TESProcess<GlobalSetup>::
postIteration(GlobalVector const& x)
{
    // TODO fix
    /*
    if (BP::_process_output.output_iteration_results)
    {
        DBUG("output results of iteration %li", _total_iteration);
        std::string fn = "tes_iter_" + std::to_string(_total_iteration) +
                         + "_ts_" + std::to_string(_timestep)
                         + "_" +    std::to_string(_assembly_params.iteration_in_current_timestep)
                         + ".vtu";

        BP::output(fn, 0, x);
    }
    */

    bool check_passed = true;

    if (!Trafo::constrained)
    {
        // bounds checking only has to happen if the vapour mass fraction is non-logarithmic.

        auto do_check = [&](
                std::vector<double> const& local_x,
                AssemblerLib::LocalToGlobalIndexMap::RowColumnIndices const& /*r_c_indices*/,
                LocalAssembler& loc_asm)
        {
            if (!loc_asm.checkBounds(local_x)) check_passed = false;
        };

        auto check_variable_bounds
        = [&](std::size_t id, LocalAssembler& loc_asm)
        {
            _global_assembler->passLocalVector(
                        do_check, id, x, loc_asm);
        };

        // TODO Short-circuit evaluation that stops after the first error.
        //      But maybe that's not what I want to use here.
        GlobalSetup::executeDereferenced(
                    check_variable_bounds, _local_assemblers);
    }

    if (!check_passed)
        return NumLib::IterationResult::REPEAT_ITERATION;


    // TODO remove
    DBUG("ts %lu iteration %lu (%lu) try XXXXXX accepted", _timestep, _total_iteration,
         _assembly_params.iteration_in_current_timestep);

    ++ _assembly_params.iteration_in_current_timestep;
    ++_total_iteration;

    return NumLib::IterationResult::SUCCESS;
}

template<typename GlobalSetup>
typename TESProcess<GlobalSetup>::GlobalVector const&
TESProcess<GlobalSetup>::
computeVapourPartialPressure(typename TESProcess::GlobalVector const& x,
                             AssemblerLib::LocalToGlobalIndexMap const& dof_table,
                             std::unique_ptr<GlobalVector>& result_cache)
{
    assert(&dof_table == BP::_local_to_global_index_map.get());

    result_cache = MathLib::MatrixVectorTraits<GlobalVector>::newInstance(
        { 0, 0, nullptr, _local_to_global_index_map_single_component.get(), nullptr });

    GlobalIndexType nnodes = BP::_mesh.getNNodes();

    for (GlobalIndexType node_id = 0; node_id<nnodes; ++node_id)
    {
        auto const p = getNodalValue(
            x, BP::_mesh, dof_table, node_id, COMPONENT_ID_PRESSURE);
        auto const x_mV = getNodalValue(
            x, BP::_mesh, dof_table, node_id, COMPONENT_ID_MASS_FRACTION);

        auto const x_nV = Adsorption::AdsorptionReaction::get_molar_fraction(
            x_mV, _assembly_params.M_react, _assembly_params.M_inert);

        // TODO Problems with PETSc? (local vs. global index)
        result_cache->set(node_id, p*x_nV);
    }

    return *result_cache;
}

template<typename GlobalSetup>
typename TESProcess<GlobalSetup>::GlobalVector const&
TESProcess<GlobalSetup>::
computeRelativeHumidity(typename TESProcess::GlobalVector const& x,
                        AssemblerLib::LocalToGlobalIndexMap const& dof_table,
                        std::unique_ptr<GlobalVector>& result_cache)
{
    assert(&dof_table == BP::_local_to_global_index_map.get());

    result_cache = MathLib::MatrixVectorTraits<GlobalVector>::newInstance(
        { 0, 0, nullptr, _local_to_global_index_map_single_component.get(), nullptr });

    GlobalIndexType nnodes = BP::_mesh.getNNodes();

    for (GlobalIndexType node_id = 0; node_id<nnodes; ++node_id)
    {
        auto const p = getNodalValue(
            x, BP::_mesh, dof_table, node_id, COMPONENT_ID_PRESSURE);
        auto const T = getNodalValue(
            x, BP::_mesh, dof_table, node_id, COMPONENT_ID_TEMPERATURE);
        auto const x_mV = getNodalValue(
            x, BP::_mesh, dof_table, node_id, COMPONENT_ID_MASS_FRACTION);

        auto const x_nV = Adsorption::AdsorptionReaction::get_molar_fraction(
            x_mV, _assembly_params.M_react, _assembly_params.M_inert);

        auto const p_S = Adsorption::AdsorptionReaction
                        ::get_equilibrium_vapour_pressure(T);

        // TODO Problems with PETSc? (local vs. global index)
        result_cache->set(node_id, p*x_nV / p_S);
    }

    return *result_cache;
}

template<typename GlobalSetup>
typename TESProcess<GlobalSetup>::GlobalVector const&
TESProcess<GlobalSetup>::
computeEquilibriumLoading(typename TESProcess::GlobalVector const& x,
                          AssemblerLib::LocalToGlobalIndexMap const& dof_table,
                          std::unique_ptr<GlobalVector>& result_cache)
{
    assert(&dof_table == BP::_local_to_global_index_map.get());

    result_cache = MathLib::MatrixVectorTraits<GlobalVector>::newInstance(
        { 0, 0, nullptr, _local_to_global_index_map_single_component.get(), nullptr });

    GlobalIndexType nnodes = BP::_mesh.getNNodes();

    for (GlobalIndexType node_id = 0; node_id<nnodes; ++node_id)
    {
        auto const p = getNodalValue(
            x, BP::_mesh, dof_table, node_id, COMPONENT_ID_PRESSURE);
        auto const T = getNodalValue(
            x, BP::_mesh, dof_table, node_id, COMPONENT_ID_TEMPERATURE);
        auto const x_mV = getNodalValue(
            x, BP::_mesh, dof_table, node_id, COMPONENT_ID_MASS_FRACTION);

        auto const x_nV = Adsorption::AdsorptionReaction::get_molar_fraction(
            x_mV, _assembly_params.M_react, _assembly_params.M_inert);

        auto const p_V = p*x_nV;
        auto const C_eq = (p_V <= 0.0)
            ? 0.0
            : _assembly_params.react_sys->get_equilibrium_loading(
                  p_V, T, _assembly_params.M_react);

        // TODO Problems with PETSc? (local vs. global index)
        result_cache->set(node_id, C_eq);
    }

    return *result_cache;
}


// Explicitly instantiate TESProcess for GlobalSetupType.
template class TESProcess<GlobalSetupType>;

} // namespace TES

}   // namespace ProcessLib