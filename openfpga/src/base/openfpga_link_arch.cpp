/********************************************************************
 * This file includes functions to read an OpenFPGA architecture file
 * which are built on the libarchopenfpga library
 *******************************************************************/
/* Headers from vtrutil library */
#include "vtr_time.h"
#include "vtr_assert.h"
#include "vtr_log.h"

/* Headers from vpr library */
#include "timing_info.h"
#include "AnalysisDelayCalculator.h"
#include "net_delay.h"

#include "vpr_device_annotation.h"
#include "pb_type_utils.h"
#include "annotate_pb_types.h"
#include "annotate_pb_graph.h"
#include "annotate_routing.h"
#include "annotate_rr_graph.h"
#include "mux_library_builder.h"
#include "build_tile_direct.h"
#include "annotate_placement.h"
#include "openfpga_link_arch.h"

/* Include global variables of VPR */
#include "globals.h"

/* begin namespace openfpga */
namespace openfpga {

/********************************************************************
 * A function to identify if the routing resource graph generated by
 * VPR is support by OpenFPGA
 * - Currently we only support uni-directional
 *   It means every routing tracks must have a direction
 *******************************************************************/
static 
bool is_vpr_rr_graph_supported(const RRGraph& rr_graph) {
  /* Check if the rr_graph is uni-directional*/
  for (const RRNodeId& node : rr_graph.nodes()) {
    if (CHANX != rr_graph.node_type(node) && CHANY != rr_graph.node_type(node)) {
      continue;
    }
    if (BI_DIRECTION == rr_graph.node_direction(node)) {
      VTR_LOG_ERROR("Routing resource graph is bi-directional. OpenFPGA currently supports uni-directional routing architecture only.\n");
      return false;
    }
  }
 
  return true;
}

/********************************************************************
 * Annotate simulation setting based on VPR results
 *  - If the operating clock frequency is set to follow the vpr timing results,
 *    we will set a new operating clock frequency here
 *  - If the number of clock cycles in simulation is set to be automatically determined,
 *    we will infer the number based on the average signal density
 *******************************************************************/
static 
void annotate_simulation_setting(const AtomContext& atom_ctx, 
                                 SimulationSetting& sim_setting) {

  /* Find if the operating frequency is binded to vpr results */
  if (0. == sim_setting.operating_clock_frequency()) {
    VTR_LOG("User specified the operating clock frequency to use VPR results\n");
    /* Run timing analysis and collect critical path delay
     * This code is copied from function vpr_analysis() in vpr_api.h 
     * Should keep updated to latest VPR code base
     * Note:
     *   - MUST mention in documentation that VPR should be run in timing enabled mode
     */
    vtr::vector<ClusterNetId, float*> net_delay;
    vtr::t_chunk net_delay_ch;
    /* Load the net delays */
    net_delay = alloc_net_delay(&net_delay_ch);
    load_net_delay_from_routing(net_delay);

    /* Do final timing analysis */
    auto analysis_delay_calc = std::make_shared<AnalysisDelayCalculator>(atom_ctx.nlist, atom_ctx.lookup, net_delay);
    auto timing_info = make_setup_hold_timing_info(analysis_delay_calc);
    timing_info->update();

    /* Get critical path delay. Update simulation settings */
    float T_crit = timing_info->least_slack_critical_path().delay() * (1. + sim_setting.operating_clock_frequency_slack());
    sim_setting.set_operating_clock_frequency(1 / T_crit); 
    VTR_LOG("Use VPR critical path delay %g [ns] with a %g [%] slack in OpenFPGA.\n",
            T_crit / 1e9, sim_setting.operating_clock_frequency_slack() * 100);
  }
  VTR_LOG("Will apply operating clock frequency %g [MHz] to simulations\n",
          sim_setting.operating_clock_frequency() / 1e6);
}

/********************************************************************
 * Top-level function to link openfpga architecture to VPR, including:
 * - physical pb_type
 * - mode selection bits for pb_type and pb interconnect
 * - circuit models for pb_type and pb interconnect
 * - physical pb_graph nodes and pb_graph pins
 * - circuit models for global routing architecture
 *******************************************************************/
void link_arch(OpenfpgaContext& openfpga_ctx,
               const Command& cmd, const CommandContext& cmd_context) { 

  vtr::ScopedStartFinishTimer timer("Link OpenFPGA architecture to VPR architecture");

  CommandOptionId opt_verbose = cmd.option("verbose");

  /* Annotate pb_type graphs
   * - physical pb_type
   * - mode selection bits for pb_type and pb interconnect
   * - circuit models for pb_type and pb interconnect
   */
  annotate_pb_types(g_vpr_ctx.device(), openfpga_ctx.arch(),
                    openfpga_ctx.mutable_vpr_device_annotation(),
                    cmd_context.option_enable(cmd, opt_verbose));

  /* Annotate pb_graph_nodes
   * - Give unique index to each node in the same type
   * - Bind operating pb_graph_node to their physical pb_graph_node
   * - Bind pins from operating pb_graph_node to their physical pb_graph_node pins
   */
  annotate_pb_graph(g_vpr_ctx.device(),
                    openfpga_ctx.mutable_vpr_device_annotation(),
                    cmd_context.option_enable(cmd, opt_verbose));

  /* Annotate routing architecture to circuit library */
  annotate_rr_graph_circuit_models(g_vpr_ctx.device(),
                                   openfpga_ctx.arch(),
                                   openfpga_ctx.mutable_vpr_device_annotation(),
                                   cmd_context.option_enable(cmd, opt_verbose));

  /* Annotate net mapping to each rr_node 
   */
  openfpga_ctx.mutable_vpr_routing_annotation().init(g_vpr_ctx.device().rr_graph);

  annotate_rr_node_nets(g_vpr_ctx.device(), g_vpr_ctx.clustering(), g_vpr_ctx.routing(), 
                        openfpga_ctx.mutable_vpr_routing_annotation(),
                        cmd_context.option_enable(cmd, opt_verbose));

  /* Build the routing graph annotation
   * - RRGSB
   * - DeviceRRGSB 
   */
  if (false == is_vpr_rr_graph_supported(g_vpr_ctx.device().rr_graph)) {
    return;
  }

  annotate_device_rr_gsb(g_vpr_ctx.device(),
                         openfpga_ctx.mutable_device_rr_gsb(),
                         cmd_context.option_enable(cmd, opt_verbose));

  /* Build multiplexer library */
  openfpga_ctx.mutable_mux_lib() = build_device_mux_library(g_vpr_ctx.device(),
                                                            const_cast<const OpenfpgaContext&>(openfpga_ctx)); 

  /* Build tile direct annotation */
  openfpga_ctx.mutable_tile_direct() = build_device_tile_direct(g_vpr_ctx.device(),
                                                                openfpga_ctx.arch().arch_direct);

  /* Annotate placement results */
  annotate_mapped_blocks(g_vpr_ctx.device(), 
                         g_vpr_ctx.clustering(),
                         g_vpr_ctx.placement(),
                         openfpga_ctx.mutable_vpr_placement_annotation());

  /* TODO: Annotate the number of clock cycles and clock frequency by following VPR results */
  annotate_simulation_setting(g_vpr_ctx.atom(),
                              openfpga_ctx.mutable_arch().sim_setting);
} 

} /* end namespace openfpga */
