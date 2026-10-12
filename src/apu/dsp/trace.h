/* Tracing is compiled out in the standalone DSP. */
#ifndef XBOXRECOMP_DSP_TRACE_H
#define XBOXRECOMP_DSP_TRACE_H
#define trace_event_get_state(x) 0
#define trace_dsp56k_execute_instruction(...) ((void)0)
#define trace_dsp56k_execute_instruction_disasm(...) ((void)0)
#define trace_dsp_read_peripheral(...) ((void)0)
#define trace_dsp_write_peripheral(...) ((void)0)
#endif
