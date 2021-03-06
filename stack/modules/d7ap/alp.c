/*! \file alp.c
 *

 *  \copyright (C) Copyright 2015 University of Antwerp and others (http://oss-7.cosys.be)
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 *  \author glenn.ergeerts@uantwerpen.be
 *  \author maarten.weyn@uantwerpen.be
 *
 */

#include "debug.h"
#include "ng.h"

#include "alp.h"
#include "packet.h"
#include "fs.h"
#include "fifo.h"
#include "log.h"
#include "alp_cmd_handler.h"

#if defined(FRAMEWORK_LOG_ENABLED) && defined(MODULE_D7AP_ALP_LOG_ENABLED)
#define DPRINT(...) log_print_stack_string(LOG_STACK_ALP, __VA_ARGS__)
#else
#define DPRINT(...)
#endif

static alp_command_origin_t NGDEF(_current_command_origin);
#define current_command_origin NG(_current_command_origin)

alp_operation_t alp_get_operation(uint8_t* alp_command)
{
    alp_control_t alp_ctrl;
    alp_ctrl.raw = (*alp_command);
    return alp_ctrl.operation;
}

static uint8_t process_action(uint8_t* alp_action, uint8_t* alp_response, uint8_t* alp_response_length)
{

}

static uint8_t process_op_read_file_data(fifo_t* alp_command_fifo, fifo_t* alp_response_fifo) {
  alp_operand_file_data_request_t operand;
  error_t err;
  err = fifo_pop(alp_command_fifo, &operand.file_offset.file_id, 1); assert(err == SUCCESS);
  err = fifo_pop(alp_command_fifo, &operand.file_offset.offset, 1); assert(err == SUCCESS); // TODO can be 1-4 bytes, assume 1 for now
  err = fifo_pop(alp_command_fifo, &operand.requested_data_length, 1); assert(err == SUCCESS);
  DPRINT("READ FILE %i LEN %i", operand.file_offset.file_id, operand.requested_data_length);

  if(operand.requested_data_length <= 0)
    return 0; // TODO status

  // fill response
  fifo_put_byte(alp_response_fifo, ALP_OP_RETURN_FILE_DATA);
  fifo_put_byte(alp_response_fifo, operand.file_offset.file_id);
  fifo_put_byte(alp_response_fifo, operand.file_offset.offset); // TODO can be 1-4 bytes, assume 1 for now
  fifo_put_byte(alp_response_fifo, operand.requested_data_length);
  uint8_t data[operand.requested_data_length];
  alp_status_codes_t alp_status = fs_read_file(operand.file_offset.file_id, operand.file_offset.offset, data, operand.requested_data_length); // TODO status
  fifo_put(alp_response_fifo, data, operand.requested_data_length);
}

static uint8_t process_op_write_file_data(fifo_t* alp_command_fifo, fifo_t* alp_response_fifo) {
  alp_operand_file_data_t operand;
  error_t err;
  err = fifo_pop(alp_command_fifo, &operand.file_offset.file_id, 1); assert(err == SUCCESS);
  err = fifo_pop(alp_command_fifo, &operand.file_offset.offset, 1); assert(err == SUCCESS); // TODO can be 1-4 bytes, assume 1 for now
  err = fifo_pop(alp_command_fifo, &operand.provided_data_length, 1); assert(err == SUCCESS);
  DPRINT("WRITE FILE %i LEN %i", operand.file_offset.file_id, operand.provided_data_length);

  uint8_t data[operand.provided_data_length];
  err = fifo_pop(alp_command_fifo, data, operand.provided_data_length);
  alp_status_codes_t alp_status = fs_write_file(operand.file_offset.file_id, operand.file_offset.offset, data, operand.provided_data_length); // TODO status
}

static uint8_t process_op_forward(fifo_t* alp_command_fifo, fifo_t* alp_response_fifo, d7asp_fifo_config_t* fifo_config) {
  uint8_t interface_id;
  error_t err;
  err = fifo_pop(alp_command_fifo, &interface_id, 1); assert(err == SUCCESS);
  assert(interface_id == ALP_ITF_ID_D7ASP); // only D7ASP supported for now // TODO return error instead of asserting
  err = fifo_pop(alp_command_fifo, &fifo_config->qos.raw, 1); assert(err == SUCCESS);
  err = fifo_pop(alp_command_fifo, &fifo_config->dormant_timeout, 1); assert(err == SUCCESS);
  err = fifo_pop(alp_command_fifo, &fifo_config->addressee.ctrl.raw, 1); assert(err == SUCCESS);
  uint8_t id_length = d7anp_addressee_id_length(fifo_config->addressee.ctrl.id_type);
  err = fifo_pop(alp_command_fifo, fifo_config->addressee.id, id_length); assert(err == SUCCESS);
  DPRINT("FORWARD");
}

void alp_process_command_result_on_d7asp(d7asp_fifo_config_t* d7asp_fifo_config, uint8_t* alp_command, uint8_t alp_command_length, alp_command_origin_t origin)
{
  uint8_t alp_response[ALP_PAYLOAD_MAX_SIZE];
  uint8_t alp_response_length = 0;
  alp_process_command(alp_command, alp_command_length, alp_response, &alp_response_length, origin);
  d7asp_queue_alp_actions(d7asp_fifo_config, alp_response, alp_response_length);
}

void alp_process_command_console_output(uint8_t* alp_command, uint8_t alp_command_length) {
  uint8_t alp_response[ALP_PAYLOAD_MAX_SIZE];
  uint8_t alp_response_length = 0;
  alp_process_command(alp_command, alp_command_length, alp_response, &alp_response_length, ALP_CMD_ORIGIN_SERIAL_CONSOLE);
}

bool alp_process_command(uint8_t* alp_command, uint8_t alp_command_length, uint8_t* alp_response, uint8_t* alp_response_length, alp_command_origin_t origin)
{
  current_command_origin = origin; // TODO support more than 1 active cmd
  (*alp_response_length) = 0;
  d7asp_fifo_config_t d7asp_fifo_config;
  bool do_forward = false;

  fifo_t alp_command_fifo, alp_response_fifo;
  fifo_init_filled(&alp_command_fifo, alp_command, alp_command_length, alp_command_length);
  fifo_init(&alp_response_fifo, alp_response, ALP_PAYLOAD_MAX_SIZE);

  while(fifo_get_size(&alp_command_fifo) > 0) {
    if(do_forward) {
      // forward rest of the actions over the D7ASP interface
      // TODO support multiple FIFOs
      uint8_t forwarded_alp_size = fifo_get_size(&alp_command_fifo);
      uint8_t forwarded_alp_actions[forwarded_alp_size];
      fifo_pop(&alp_command_fifo, forwarded_alp_actions, forwarded_alp_size);
      d7asp_queue_alp_actions(&d7asp_fifo_config, forwarded_alp_actions, forwarded_alp_size); // TODO pass fifo directly?
      break; // TODO return response
    }

    alp_control_t control;
    fifo_pop(&alp_command_fifo, &control.raw, 1);
    switch(control.operation) {
      case ALP_OP_READ_FILE_DATA:
        process_op_read_file_data(&alp_command_fifo, &alp_response_fifo);
        break;
      case ALP_OP_WRITE_FILE_DATA:
        process_op_write_file_data(&alp_command_fifo, &alp_response_fifo);
        break;
      case ALP_OP_FORWARD:
        process_op_forward(&alp_command_fifo, &alp_response_fifo, &d7asp_fifo_config);
        do_forward = true;
        break;
      default:
        assert(false); // TODO return error
        //alp_status = ALP_STATUS_UNKNOWN_OPERATION;
    }
  }

  (*alp_response_length) = fifo_get_size(&alp_response_fifo);

  if((*alp_response_length) > 0) {
    if(current_command_origin == ALP_CMD_ORIGIN_SERIAL_CONSOLE)
      alp_cmd_handler_output_alp_command(alp_response, (*alp_response_length));

    // TODO APP
  }

    // TODO return ALP status if requested

//    if(alp_status != ALP_STATUS_OK)
//      return false;

    return true;
}

void alp_d7asp_request_completed(d7asp_result_t result, uint8_t* payload, uint8_t payload_length) {
  switch(current_command_origin) {
    case ALP_CMD_ORIGIN_SERIAL_CONSOLE:
      alp_cmd_handler_output_d7asp_response(result, payload, payload_length);
      break;
    case ALP_CMD_ORIGIN_APP:
      // TODO callback
      break;
    case ALP_CMD_ORIGIN_D7AACTP:
      // do nothing?
      break;
    default:
      assert(false); // ALP_CMD_ORIGIN_D7ASP this would imply a slave session
  }

  // TODO further bookkeeping
}

void alp_d7asp_fifo_flush_completed(uint8_t fifo_token, uint8_t* progress_bitmap, uint8_t* success_bitmap, uint8_t bitmap_byte_count) {
  switch(current_command_origin) {
    case ALP_CMD_ORIGIN_SERIAL_CONSOLE:
      alp_cmd_handler_output_d7asp_flush_result(fifo_token, progress_bitmap, success_bitmap, bitmap_byte_count);
      break;
    case ALP_CMD_ORIGIN_APP:
      // TODO callback
      break;
    case ALP_CMD_ORIGIN_D7AACTP:
      // do nothing?
      break;
    default:
      assert(false); // ALP_CMD_ORIGIN_D7ASP this would imply a slave session
  }
}
