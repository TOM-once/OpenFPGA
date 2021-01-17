/********************************************************************
 * This file includes functions to compress the hierachy of routing architecture
 *******************************************************************/
/* Headers from vtrutil library */
#include "vtr_time.h"
#include "vtr_log.h"

/* Headers from openfpgashell library */
#include "command_exit_codes.h"

/* Headers from librepackdc library */
#include "repack_design_constraints.h"
#include "read_xml_repack_design_constraints.h"

#include "build_physical_truth_table.h"
#include "repack.h"
#include "openfpga_repack.h"


/* Include global variables of VPR */
#include "globals.h"

/* begin namespace openfpga */
namespace openfpga {

/********************************************************************
 * A wrapper function to call the fabric_verilog function of FPGA-Verilog 
 *******************************************************************/
int repack(OpenfpgaContext& openfpga_ctx,
           const Command& cmd, const CommandContext& cmd_context) {

  CommandOptionId opt_design_constraints = cmd.option("design_constraints");
  CommandOptionId opt_verbose = cmd.option("verbose");

  /* Load design constraints from file */
  RepackDesignConstraints repack_design_constraints;
  if (true == cmd_context.option_enable(cmd, opt_design_constraints)) {
    std::string dc_fname = cmd_context.option_value(cmd, opt_design_constraints);
    VTR_ASSERT(false == dc_fname.empty());
    repack_design_constraints = read_xml_repack_design_constraints(dc_fname.c_str());
  }

  pack_physical_pbs(g_vpr_ctx.device(),
                    g_vpr_ctx.atom(),
                    g_vpr_ctx.clustering(),
                    openfpga_ctx.mutable_vpr_device_annotation(),
                    openfpga_ctx.mutable_vpr_clustering_annotation(),
                    cmd_context.option_enable(cmd, opt_verbose));

  build_physical_lut_truth_tables(openfpga_ctx.mutable_vpr_clustering_annotation(),
                                  g_vpr_ctx.atom(),
                                  g_vpr_ctx.clustering(),
                                  openfpga_ctx.vpr_device_annotation(),
                                  openfpga_ctx.arch().circuit_lib,
                                  cmd_context.option_enable(cmd, opt_verbose));

  /* TODO: should identify the error code from internal function execution */
  return CMD_EXEC_SUCCESS;
} 

} /* end namespace openfpga */
