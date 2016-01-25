/* * OSS-7 - An opensource implementation of the DASH7 Alliance Protocol for ultra
 * lowpower wireless sensor communication
 *
 * Copyright 2015 University of Antwerp
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * \author	glenn.ergeerts@uantwerpen.be
 * \author  maarten.weyn@uantwerpen.be
 * \author  contact@christophe.vg
 */


#include "string.h"
#include "stdio.h"
#include "stdint.h"

#include "hwleds.h"
#include "log.h"
#include "random.h"

#include "d7ap_stack.h"
#include "dll.h"


#ifdef HAS_LCD
#include "hwlcd.h"
#endif

#ifdef FRAMEWORK_LOG_ENABLED
#ifdef HAS_LCD
		#define DPRINT(...) log_print_string(__VA_ARGS__); lcd_write_string(__VA_ARGS__)
	#else
		#define DPRINT(...) log_print_string(__VA_ARGS__)
	#endif

#else
	#ifdef HAS_LCD
		#define DPRINT(...) lcd_write_string(__VA_ARGS__)
	#else
		#define DPRINT(...)
	#endif
#endif

#define SENSOR_FILE_ID           0x40
#define SENSOR_FILE_SIZE         4
#define ACTION_FILE_ID           0x41

#define REPORTING_INTERVAL       5 // seconds
#define REPORTING_INTERVAL_TICKS TIMER_TICKS_PER_SEC * REPORTING_INTERVAL

void execute_sensor_measurement() {
#if HW_NUM_LEDS > 1
    led_toggle(1);
#endif
    // use the counter value for now instead of 'real' sensor
    uint32_t val = timer_get_counter_value();
    // file 0x40 is configured to use D7AActP trigger an ALP action which 
    // broadcasts this file data on Access Class 0
    fs_write_file(0x40, 0, (uint8_t*)&val, 4);
    timer_post_task_delay(&execute_sensor_measurement, REPORTING_INTERVAL_TICKS);
}

void on_unsollicited_response_received(d7asp_result_t d7asp_result,
                                      uint8_t *alp_command, uint8_t alp_command_size,
                                      hw_rx_metadata_t* rx_meta)
{
	DPRINT("Received unsolicited response\n");
}

void init_user_files() {
    // file 0x40: contains our sensor data + configure an action file to be 
    // executed upon write
    fs_file_header_t file_header = (fs_file_header_t){
        .file_properties.action_protocol_enabled  = 1,
        .file_properties.action_file_id           = ACTION_FILE_ID,
        .file_properties.action_condition         = ALP_ACT_COND_WRITE,
        .file_properties.storage_class            = FS_STORAGE_VOLATILE,
        .file_properties.permissions              = 0, // TODO
        .length = SENSOR_FILE_SIZE
    };

    fs_init_file(SENSOR_FILE_ID, &file_header, NULL);

    // configure file notification using D7AActP: write ALP command to 
    // broadcast changes made to file 0x40 in file 0x41
    // first generate ALP command consisting of ALP Control header, ALP File 
    // Data Request operand and D7ASP interface configuration
    alp_control_t alp_ctrl = {
        .group                            = false,
        .response_requested               = false,
        .operation                        = ALP_OP_READ_FILE_DATA
    };

    alp_operand_file_data_request_t file_data_request_operand = {
        .file_offset = {
            .file_id                      = SENSOR_FILE_ID,
            .offset                       = 0
        },
        .requested_data_length            = SENSOR_FILE_SIZE,
    };

    d7asp_fifo_config_t d7asp_fifo_config = {
        .fifo_ctrl_nls                    = false,
        .fifo_ctrl_stop_on_error          = false,
        .fifo_ctrl_preferred              = false,
        .fifo_ctrl_state                  = SESSION_STATE_PENDING,
        .qos = {
            .qos_ctrl_resp_mode           = SESSION_RESP_MODE_ANYCAST,
            .qos_ctrl_ack_not_void        = false,
            .qos_ack_period               = 1,
            .qos_retry_single             = 3,
            .qos_retry_total              = 0
        },
        .dormant_timeout                  = 0,
        .start_id                         = 0, // TODO
        .addressee = {
            .addressee_ctrl_has_id        = false,
            .addressee_ctrl_virtual_id    = false,
            .addressee_ctrl_access_class  = 0,
            .addressee_id                 = 0
        }
    };

    // finally, register D7AActP file
    fs_init_file_with_D7AActP(ACTION_FILE_ID, &d7asp_fifo_config, &alp_ctrl,
                              (uint8_t*)&file_data_request_operand);
}

void on_d7asp_fifo_flush_completed(uint8_t fifo_token, uint8_t* progress_bitmap,
                                   uint8_t* success_bitmap, uint8_t bitmap_byte_count)
{
    if(memcmp(success_bitmap, progress_bitmap, bitmap_byte_count) == 0) {
        DPRINT("All requests acknowledged");
    } else {
        DPRINT("Not all requests acknowledged");
    }
}

static d7asp_init_args_t d7asp_init_args;

void bootstrap() {
    DPRINT("Device booted at time: %d\n", timer_get_counter_value());

    dae_access_profile_t access_classes[1] = {
        {
            .control_scan_type_is_foreground = true,
            .control_csma_ca_mode = CSMA_CA_MODE_UNC,
            .control_number_of_subbands = 1,
            .subnet = 0x00,
            .scan_automation_period = 0,
            .transmission_timeout_period = 0xFF,
            .subbands[0] = (subband_t){
                .channel_header = {
                    .ch_coding = PHY_CODING_PN9,
                    .ch_class = PHY_CLASS_NORMAL_RATE,
                    .ch_freq_band = PHY_BAND_868
                },
                .channel_index_start = 0,
                .channel_index_end = 0,
                .eirp = 0,
                .ccao = 0
            }
        }
    };

    fs_init_args_t fs_init_args = (fs_init_args_t){
        .fs_user_files_init_cb = &init_user_files,
        .access_profiles_count = 1,
        .access_profiles = access_classes
    };

    d7asp_init_args.d7asp_fifo_flush_completed_cb = &on_d7asp_fifo_flush_completed;
    d7asp_init_args.d7asp_received_unsollicited_data_cb = &on_unsollicited_response_received;

    d7ap_stack_init(&fs_init_args, &d7asp_init_args, false);

    sched_register_task((&execute_sensor_measurement));
    timer_post_task_delay(&execute_sensor_measurement, REPORTING_INTERVAL_TICKS);
}

void debugHardfault(uint32_t *sp)
{
//    uint32_t cfsr  = SCB->CFSR;
//    uint32_t hfsr  = SCB->HFSR;
//    uint32_t mmfar = SCB->MMFAR;
//    uint32_t bfar  = SCB->BFAR;
//
//    uint32_t r0  = sp[0];
//    uint32_t r1  = sp[1];
//    uint32_t r2  = sp[2];
//    uint32_t r3  = sp[3];
//    uint32_t r12 = sp[4];
//    uint32_t lr  = sp[5];
//    uint32_t pc  = sp[6];
//    uint32_t psr = sp[7];
//
//    printf("HardFault:\n");
//    printf("SCB->CFSR   0x%08lx\n", cfsr);
//    printf("SCB->HFSR   0x%08lx\n", hfsr);
//    printf("SCB->MMFAR  0x%08lx\n", mmfar);
//    printf("SCB->BFAR   0x%08lx\n", bfar);
//    printf("\n");
//
//    printf("SP          0x%08lx\n", (uint32_t)sp);
//    printf("R0          0x%08lx\n", r0);
//    printf("R1          0x%08lx\n", r1);
//    printf("R2          0x%08lx\n", r2);
//    printf("R3          0x%08lx\n", r3);
//    printf("R12         0x%08lx\n", r12);
//    printf("LR          0x%08lx\n", lr);
//    printf("PC          0x%08lx\n", pc);
//    printf("PSR         0x%08lx\n", psr);

	__asm__("BKPT");
	while(1);
}

__attribute__( (naked) )
void HardFault_Handler(void)
{
	 __asm volatile
	    (
	        "tst lr, #4                                    \n"
	        "ite eq                                        \n"
	        "mrseq r0, msp                                 \n"
	        "mrsne r0, psp                                 \n"
	        "ldr r1, debugHardfault_address                \n"
	        "bx r1                                         \n"
	        "debugHardfault_address: .word debugHardfault  \n"
	    );
}

/*      Hard Fault Handler        */
void MemManage_Handler(void)
{
	__asm__("BKPT");
	while(1);
}

void BusFault_Handler(void)
{
	__asm__("BKPT");
	while(1);
}

void UsageFault_Handler(void)
{
	__asm__("BKPT");
	while(1);
}
