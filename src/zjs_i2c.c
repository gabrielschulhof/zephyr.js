// Copyright (c) 2016, Intel Corporation.
#ifdef BUILD_MODULE_I2C
#ifndef QEMU_BUILD
// Zephyr includes
#include <i2c.h>
#include <string.h>

// ZJS includes
#include "zjs_i2c.h"
#include "zjs_ipm.h"
#include "zjs_util.h"
#include "zjs_buffer.h"

#define ZJS_I2C_TIMEOUT_TICKS                      500

static struct nano_sem i2c_sem;


static struct zjs_ipm_message* zjs_i2c_alloc_msg()
{
    struct zjs_ipm_message *msg = task_malloc(sizeof(struct zjs_ipm_message));
    if (!msg) {
        PRINT("zjs_i2c_alloc_msg: cannot allocate message\n");
        return NULL;
    } else {
        memset(msg, 0, sizeof(struct zjs_ipm_message));
    }

    msg->id = MSG_ID_I2C;
    msg->flags = 0 | MSG_SAFE_TO_FREE_FLAG;
    msg->error_code = ERROR_IPM_NONE;
    return msg;
}

static void zjs_i2c_free_msg(struct zjs_ipm_message* msg)
{
    if (!msg)
        return;

    if (msg->flags & MSG_SAFE_TO_FREE_FLAG) {
        task_free(msg);
    } else {
        PRINT("zjs_i2c_free_msg: error! do not free message\n");
    }
}

static bool zjs_i2c_ipm_send_sync(struct zjs_ipm_message* send,
                                  struct zjs_ipm_message* result) {
    send->flags |= MSG_SYNC_FLAG;
    send->user_data = (void *)result;
    send->error_code = ERROR_IPM_NONE;

    if (zjs_ipm_send(MSG_ID_I2C, send) != 0) {
        PRINT("zjs_i2c_ipm_send_sync: failed to send message\n");
        return false;
    }

    // block until reply or timeout
    if (!nano_sem_take(&i2c_sem, ZJS_I2C_TIMEOUT_TICKS)) {
        PRINT("zjs_i2c_ipm_send_sync: ipm timed out\n");
        return false;
    }

    return true;
}

static void ipm_msg_receive_callback(void *context, uint32_t id, volatile void *data)
{
    if (id != MSG_ID_I2C)
        return;

    struct zjs_ipm_message *msg = (struct zjs_ipm_message*)(*(uintptr_t *)data);

    if (msg->flags & MSG_SYNC_FLAG) {
         struct zjs_ipm_message *result = (struct zjs_ipm_message*)msg->user_data;
        // synchronous ipm, copy the results
        if (result)
            memcpy(result, msg, sizeof(struct zjs_ipm_message));

        // un-block sync api
        nano_isr_sem_give(&i2c_sem);
    } else {
        // asynchronous ipm, should not get here
        PRINT("ipm_msg_receive_callback: async message received\n");
    }
}

static jerry_value_t zjs_i2c_read(const jerry_value_t function_obj_val,
                                  const jerry_value_t this_val,
                                  const jerry_value_t args_p[],
                                  const jerry_length_t args_cnt)
{
    // Not implemented yet
    return ZJS_UNDEFINED;
}

static jerry_value_t zjs_i2c_write(const jerry_value_t function_obj_val,
                                   const jerry_value_t this_val,
                                   const jerry_value_t args_p[],
                                   const jerry_length_t args_cnt)
{
    if (args_cnt < 2 || !jerry_value_is_object(args_p[0])) {
        return zjs_error("zjs_i2c_write: missing arguments");
    }

    if (!jerry_value_is_object(args_p[1])) {
        return zjs_error("zjs_i2c_write: message buffer is invalid");
    }

    uint32_t bus;
    zjs_obj_get_uint32(this_val, "bus", &bus);

    jerry_value_t jerryData = args_p[0];

    uint32_t length;
    if (!zjs_obj_get_uint32(jerryData, "length", &length)) {
        return zjs_error("zjs_i2c_write: missing required field (length)");
    }

    struct zjs_buffer_t *dataBuf = zjs_buffer_find(args_p[1]);

    if (!dataBuf) {
        return zjs_error("zjs_i2c_write:  missing data buffer");
    }

    uint32_t address;
    if (!zjs_obj_get_uint32(jerryData, "address", &address)) {
        return zjs_error("zjs_i2c_write: missing required field (address)");
    }

    // send IPM message to the ARC side
    struct zjs_ipm_message* send = zjs_i2c_alloc_msg();
    struct zjs_ipm_message* reply = zjs_i2c_alloc_msg();

    send->type = TYPE_I2C_WRITE;
    send->data.i2c.bus = (uint8_t)bus;
    send->data.i2c.data = dataBuf->buffer;
    send->data.i2c.address = (uint16_t)address;
    send->data.i2c.length = dataBuf->bufsize;
    bool success = zjs_i2c_ipm_send_sync(send, reply);
    zjs_i2c_free_msg(send);

    if (!success) {
        zjs_i2c_free_msg(reply);
        return zjs_error("zjs_i2c_write: ipm message failed or timed out!");
    }

    if (!reply || reply->error_code != ERROR_IPM_NONE) {
        PRINT("error code: %lu\n", reply->error_code);
        zjs_i2c_free_msg(reply);
        return zjs_error("zjs_i2c_write: error received");
    }

    zjs_i2c_free_msg(reply);
    return ZJS_UNDEFINED;
}

static jerry_value_t zjs_i2c_abort(const jerry_value_t function_obj_val,
                                   const jerry_value_t this_val,
                                   const jerry_value_t args_p[],
                                   const jerry_length_t args_cnt)
{
    // Not implemented yet
    return ZJS_UNDEFINED;
}

static jerry_value_t zjs_i2c_close(const jerry_value_t function_obj_val,
                                   const jerry_value_t this_val,
                                   const jerry_value_t args_p[],
                                   const jerry_length_t args_cnt)
{
    // Not implemented yet
    return ZJS_UNDEFINED;
}

static jerry_value_t zjs_i2c_open(const jerry_value_t function_obj_val,
                                  const jerry_value_t this_val,
                                  const jerry_value_t args_p[],
                                  const jerry_length_t args_cnt)
{

    if (args_cnt < 1 || !jerry_value_is_object(args_p[0])) {
        return zjs_error("zjs_i2c_open: invalid argument");
    }

    jerry_value_t data = args_p[0];

    uint32_t bus;
    if (!zjs_obj_get_uint32(data, "bus", &bus)) {
        return zjs_error("zjs_i2c_open: missing required field (bus)");
    }

    uint32_t speed;
    if (!zjs_obj_get_uint32(data, "speed", &speed)) {
        return zjs_error("zjs_i2c_open: missing required field (speed)");
    }

    // send IPM message to the ARC side
    struct zjs_ipm_message* send = zjs_i2c_alloc_msg();
    struct zjs_ipm_message* reply = zjs_i2c_alloc_msg();

    send->type = TYPE_I2C_OPEN;
    send->data.i2c.bus = (uint8_t)bus;
    send->data.i2c.speed = (uint8_t)speed;
    bool success = zjs_i2c_ipm_send_sync(send, reply);
    zjs_i2c_free_msg(send);

    if (!success) {
        zjs_i2c_free_msg(reply);
        return zjs_error("zjs_i2c_write: ipm message failed or timed out!");
    }

    if (!reply || reply->error_code != ERROR_IPM_NONE) {
        PRINT("error code: %lu\n", reply->error_code);
        zjs_i2c_free_msg(reply);
        return zjs_error("zjs_i2c_write: error received");
    }

    zjs_i2c_free_msg(reply);

    // create the I2C object
    jerry_value_t i2c_obj = jerry_create_object();
    zjs_obj_add_function(i2c_obj, zjs_i2c_read, "read");
    zjs_obj_add_function(i2c_obj, zjs_i2c_write, "write");
    zjs_obj_add_function(i2c_obj, zjs_i2c_abort, "abort");
    zjs_obj_add_function(i2c_obj, zjs_i2c_close, "close");
    zjs_obj_add_number(i2c_obj, bus, "bus");
    zjs_obj_add_number(i2c_obj, speed, "speed");

    return i2c_obj;
}

jerry_value_t zjs_i2c_init()
{
    zjs_ipm_init();
    zjs_ipm_register_callback(ipm_msg_receive_callback);

    nano_sem_init(&i2c_sem);

    // create global I2C object
    jerry_value_t i2c_obj = jerry_create_object();
    zjs_obj_add_function(i2c_obj, zjs_i2c_open, "open");
    return i2c_obj;
}

#endif // QEMU_BUILD
#endif // BUILD_MODULE_I2C