/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_ADRENO_DIAG_H
#define NATIVE_ADRENO_DIAG_H
#include <stdint.h>
/* The records NativeAdrenoPublish.cpp keeps about the processes that reach the published service (opens, progress markers, report
 * lines), shared with a board probe that prints them. Plain counters and short strings; the driver is their only writer. */
#if defined(__cplusplus)
extern "C" {
#endif
/* What the processes that reached this driver did, for the boot diagnostic page. Plain counters and short strings. */
struct NativeAdrenoMetalDiag {
    uint32_t opens, busy, bad, closes, reports, property_reads, foreign_opens, foreign_type, name_reads, calls, last_selector, last_status, ext_entries, ext_selector, ext_status, ext_in, ext_out, tunnel_calls, probes, sel_bits[8], mb_maps, mb_calls, marks_total, marks[16], chk_macf, chk_res, chk_type, type_pass_lo, type_pass_hi, attach_status, inflight, inflight_since_calls, forced_unmaps;
    char opener[3][17];      /* process names of the latest connections, newest first */
    char reader[3][17];      /* process names that read a registry property of the accelerator, newest first */
    char name_reader[17];    /* latest process that read MetalPluginName specifically */
    char starter[3][17];     /* processes that sent plugin marker 1 (the plugin's initWithAcceleratorPort: started), newest first */
    char report[64];         /* the plugin's own result line (SetMetadata text), latest */
    char report2[64];        /* the plugin's Turnip-phase line (text starting "T " or "W "), latest */
    uint32_t grants;         /* opens granted to the render server by this driver in place of the sandbox verdict (the view in GpuPowerProbe.cpp ends before this) */
};
/* One entry per IOServiceOpen of the accelerator (not markers): who, which type, what the two sandbox gates said. */
struct NativeAdrenoOpenRecord {char name[20]; uint32_t type, macf, res, attach, foreign;};
/* One row per process name that opened the plugin type (first eight names): opens granted, refused (sandbox, cap or no free slot),
 * closes, the slot of its live connection (0xff none), the highest GPU address its mappings reached (MiB) and its last refusal. */
struct NativeAdrenoClientRecord {char name[20]; uint32_t opens, refused, closes, slot, peak_mib, last_refusal;};
/* The last text line (the plugin's failure reason) each process other than the render server streamed: process name and a 64-byte line. */
struct NativeAdrenoClientText {char name[20]; char text[64];};
extern struct NativeAdrenoMetalDiag native_adreno_metal_diag;
extern struct NativeAdrenoOpenRecord native_adreno_open_log[8];
extern volatile uint32_t native_adreno_open_log_count;
extern struct NativeAdrenoClientRecord native_adreno_client_log[8];
extern struct NativeAdrenoClientText native_adreno_client_text[8];
extern volatile uint32_t native_adreno_mount_fail[4];
#if defined(__cplusplus)
}
#endif
#endif
